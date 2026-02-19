// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "auth_token_providerImpl.hpp"
#include <everest/helpers/helpers.hpp>

#include <cerrno>
#include <csignal>
#include <cctype>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace module {
namespace main {

static speed_t to_baud(int br) {
    switch (br) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B9600;
        }
}

auth_token_providerImpl::~auth_token_providerImpl() {
    running_ = false;
    q_cv_.notify_all();   // wake anyone waiting in AuthRequired path

    if (serial_thread_.joinable()) {
        serial_thread_.join();
    }
}

void auth_token_providerImpl::init() {
    // Subscribe to EVSE events
    mod->r_evse->subscribe_session_event([this](types::evse_manager::SessionEvent event) {
        if (config.mode == "serial_on_auth_required" &&
            event.event == types::evse_manager::SessionEventEnum::AuthRequired) {
            // Wait up to timeout seconds for a token to show up in queue
            // EVLOG_info << "Auth Required: Waiting upto " << config.timeout << "s for token...";
            EVLOG_info << "AuthRequired: waiting " << (config.timeout <= 0 ? "indefinitely" : ("up to " + std::to_string((int)config.timeout) + "s"))<< " for a token...";

            std::unique_lock<std::mutex> lk(q_mtx_);

            bool got = false;
            // bool got = q_cv_.wait_for(
            //     lk, std::chrono::seconds(static_cast<int>(config.timeout)),
            //     [this] { return !token_queue_.empty(); });

            // if (got) {
            //     std::string token = std::move(token_queue_.front());
            //     token_queue_.pop();
            //     lk.unlock();
            //     publish_token_string(token);
            // } else {
            //     EVLOG_warning << "No token received from serial within timeout " << config.timeout << "s";
            // }

            if (config.timeout <= 0) {
                // ❗ Infinite wait until token arrives or module is shutting down
                q_cv_.wait(lk, [this] { return !token_queue_.empty() || !running_; });
                got = !token_queue_.empty();
            } else {
                // ⏳ Finite wait for timeout seconds
                got = q_cv_.wait_for(lk, std::chrono::seconds(static_cast<int>(config.timeout)), [this] { return !token_queue_.empty(); });
            }

            if (!running_) {
                EVLOG_info << "AuthRequired: stopping wait due to module shutdown.";
                return;
            }

            if (got) {
                std::string token = std::move(token_queue_.front());
                token_queue_.pop();
                lk.unlock();
                publish_token_string(token);
            } else {
                EVLOG_warn << "No token received from serial within timeout " << (int)config.timeout << "s";
            }
    }

        }

        // (Optional) Keep dummy SessionStarted behavior if you want compatibility:
        if (config.mode == "dummy" &&
            event.event == types::evse_manager::SessionEventEnum::SessionStarted) {
            types::authorization::ProvidedIdToken token;
            token.id_token = {config.token, types::authorization::IdTokenType::ISO14443};
            token.authorization_type = types::authorization::string_to_authorization_type(config.type);
            if (config.connector_id > 0) {
                token.connectors.emplace(std::initializer_list<int>{config.connector_id});
            }
            token.parent_id_token = {config.token, types::authorization::IdTokenType::ISO14443};
            EVLOG_info << "Publishing dummy token: " << everest::helpers::redact(token);
            publish_provided_token(token);
        }
    });

    // Compile regex
    try {
        token_re_ = std::regex(config.token_regex);
    } catch (const std::exception& e) {
        EVLOG_error << "Invalid token_regex '" << config.token_regex << "': " << e.what()
                    << " -> falling back to ([0-9A-Fa-f]+)";
        token_re_ = std::regex("([0-9A-Fa-f]+)");
    }

    // Start serial reader if mode requires it
    start_serial_if_needed();
}

void auth_token_providerImpl::ready() {
}

void auth_token_providerImpl::start_serial_if_needed() {
    if (config.mode == "dummy") {
        EVLOG_info << "auth_token_provider running in 'dummy' mode (serial disabled).";
        return;
    }
    if (config.serial_device.empty()) {
        EVLOG_error << "serial_device not set, but mode is '" << config.mode << "'";
        return;
    }

    running_ = true;
    serial_thread_ = std::thread([this] { serial_loop(); });
}

bool auth_token_providerImpl::open_and_configure_serial(int& fd) {
    fd = ::open(config.serial_device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        EVLOG_error << "Failed to open " << config.serial_device << ": " << strerror(errno);
        return false;
    }

    // Make it blocking after open
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        EVLOG_error << "tcgetattr failed: " << strerror(errno);
        ::close(fd);
        return false;
    }

    // Raw mode baseline
    cfmakeraw(&tio);

    // Baudrate
    cfsetispeed(&tio, to_baud(config.baudrate));
    cfsetospeed(&tio, to_baud(config.baudrate));

    // Data bits
    tio.c_cflag &= ~CSIZE;
    switch (config.data_bits) {
        case 5: tio.c_cflag |= CS5; break;
        case 6: tio.c_cflag |= CS6; break;
        case 7: tio.c_cflag |= CS7; break;
        case 8:
        default: tio.c_cflag |= CS8; break;
    }

    // Parity
    if (config.parity == "even") {
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~PARODD;
    } else if (config.parity == "odd") {
        tio.c_cflag |= PARENB;
        tio.c_cflag |= PARODD;
    } else {
        tio.c_cflag &= ~PARENB; // none
    }

    // Stop bits
    if (config.stop_bits == 2) tio.c_cflag |= CSTOPB;
    else                       tio.c_cflag &= ~CSTOPB;

    // Read behavior: return as chars arrive (line-assemble in userland)
    tio.c_cc[VMIN]  = 0;   // 0 => return immediately with available bytes
    tio.c_cc[VTIME] = 10;  // 1.0s read timeout (deciseconds)

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        EVLOG_error << "tcsetattr failed: " << strerror(errno);
        ::close(fd);
        return false;
    }

    // Flush input
    tcflush(fd, TCIFLUSH);
    return true;
}

void auth_token_providerImpl::close_serial(int fd) {
    if (fd >= 0) ::close(fd);
}

void auth_token_providerImpl::serial_loop() {
    int fd = -1;
    if (!open_and_configure_serial(fd)) {
        EVLOG_error << "Serial not started.";
        running_ = false;
        return;
    }

    EVLOG_info << "Serial reader started on " << config.serial_device
               << " @ " << config.baudrate << " baud";

    const std::string delim = config.line_delimiter.empty() ? "\n" : config.line_delimiter;
    std::string buf;
    buf.resize(256);

    while (running_) {
        // Read up to buf.size() bytes (blocking up to ~1s due to VTIME)
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            EVLOG_error << "Serial read error: " << strerror(errno);
            break;
        } else if (n == 0) {
            // timeout (VTIME) -> continue loop
            continue;
        }

        // Accumulate and split by delimiter
        pending_.append(buf.data(), static_cast<size_t>(n));

        size_t pos = 0;
        while (running_) {
            size_t dpos = pending_.find(delim, pos);
            if (dpos == std::string::npos) {
                // keep the remaining partial data in pending_
                pending_.erase(0, pos);
                break;
            }
            // Extract one full line [pos, dpos)
            std::string line = pending_.substr(pos, dpos - pos);
            // Advance past delimiter
            pos = dpos + delim.size();

            // Handle optional CR if delimiter is "\n" but sender uses "\r\n"
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            handle_line(line);
        }
    }

    close_serial(fd);
    EVLOG_info << "Serial reader stopped.";
}

void auth_token_providerImpl::handle_line(const std::string& line) {
    // Try to extract token using regex’s first capture group
    std::smatch m;
    if (std::regex_search(line, m, token_re_) && m.size() >= 2) {
        std::string tok = m[1].str();

        // Normalize (e.g., uppercase) if your format is hex
        for (auto& c : tok) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        EVLOG_info << "Token received from serial (len=" << tok.size() << ").";

        if (config.mode == "serial_push") {
            publish_token_string(tok);
        } else if (config.mode == "serial_on_auth_required") {
            // queue for AuthRequired handler
            {
                std::lock_guard<std::mutex> lk(q_mtx_);
                token_queue_.push(std::move(tok));
            }
            q_cv_.notify_one();
        }
    } else {
        EVLOG_debug << "No token matched in line: \"" << line << "\"";
    }
}

void auth_token_providerImpl::publish_token_string(const std::string& token_str) {
    types::authorization::ProvidedIdToken token;

    // Physical token media type: keep ISO14443 for RFID-like cards
    token.id_token = {token_str, types::authorization::IdTokenType::ISO14443};

    // Logical authorization type (e.g., RFID)
    token.authorization_type = types::authorization::string_to_authorization_type(config.type);

    if (config.connector_id > 0) {
        token.connectors.emplace(std::initializer_list<int>{config.connector_id});
    }

    token.parent_id_token = {token_str, types::authorization::IdTokenType::ISO14443};

    EVLOG_info << "Publishing token from serial.";
    publish_provided_token(token);
}

} // namespace main
} // namespace module
