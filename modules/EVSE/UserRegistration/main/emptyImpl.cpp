// SPDX-License-Identifier: Apache-2.0

#include "emptyImpl.hpp"

#include <everest/logging.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <fcntl.h>

namespace module {
namespace main {
namespace {
constexpr uint16_t registration_button_vp = 0x1000;
constexpr uint16_t registration_complete_vp = 0x2000;
constexpr std::array<uint8_t, 2> dwin_header = {0x5A, 0xA5};
constexpr uint8_t dwin_touch_command = 0x83;
constexpr uint8_t dwin_write_command = 0x82;
}

emptyImpl::emptyImpl(Everest::ModuleAdapter* ev, const Everest::PtrContainer<UserRegistration>& mod, Conf& config) :
    emptyImplBase(ev, "main"), mod(mod), config(config) {
}

emptyImpl::~emptyImpl() {
    running = false;
    if (input_thread.joinable()) {
        input_thread.join();
    }
    if (serial_fd >= 0) {
        close(serial_fd);
    }
}

void emptyImpl::init() {
    open_display();
    if (mod->r_token_provider) {
        mod->r_token_provider->subscribe_provided_token([this](const types::authorization::ProvidedIdToken& token) {
            on_token(token);
        });
    }
}

void emptyImpl::ready() {
    if (serial_fd < 0) {
        EVLOG_error << "UserRegistration: DWIN display is unavailable; registration is disabled";
        return;
    }
    running = true;
    input_thread = std::thread([this] { read_display(); });
}

void emptyImpl::open_display() {
    serial_fd = open(config.display_serial_port.c_str(), O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
    if (serial_fd < 0) {
        EVLOG_error << "UserRegistration: Failed to open DWIN serial port " << config.display_serial_port
                    << ": " << strerror(errno);
        return;
    }

    termios tty{};
    if (tcgetattr(serial_fd, &tty) != 0) {
        EVLOG_error << "UserRegistration: tcgetattr failed: " << strerror(errno);
        close(serial_fd);
        serial_fd = -1;
        return;
    }

    speed_t baud = B115200;
    switch (config.display_baud_rate) {
    case 9600:
        baud = B9600;
        break;
    case 19200:
        baud = B19200;
        break;
    case 38400:
        baud = B38400;
        break;
    case 57600:
        baud = B57600;
        break;
    case 115200:
        baud = B115200;
        break;
    default:
        EVLOG_warning << "UserRegistration: Unsupported baud rate " << config.display_baud_rate
                      << "; using 115200";
        break;
    }

    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_iflag = 0;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
        EVLOG_error << "UserRegistration: tcsetattr failed: " << strerror(errno);
        close(serial_fd);
        serial_fd = -1;
        return;
    }

    EVLOG_info << "UserRegistration: Listening for DWIN registration button on " << config.display_serial_port;
}

void emptyImpl::read_display() {
    std::vector<uint8_t> buffer;
    std::array<uint8_t, 128> input{};

    while (running) {
        pollfd descriptor{serial_fd, POLLIN, 0};
        const int poll_result = poll(&descriptor, 1, 200);
        if (poll_result <= 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }

        const ssize_t received = read(serial_fd, input.data(), input.size());
        if (received <= 0) {
            continue;
        }
        buffer.insert(buffer.end(), input.begin(), input.begin() + received);

        while (buffer.size() >= 3) {
            auto header = std::search(buffer.begin(), buffer.end(), dwin_header.begin(), dwin_header.end());
            if (header == buffer.end()) {
                const bool keep_header_prefix = buffer.back() == dwin_header[0];
                buffer.erase(buffer.begin(), buffer.end() - (keep_header_prefix ? 1 : 0));
                break;
            }
            if (header != buffer.begin()) {
                buffer.erase(buffer.begin(), header);
            }
            if (buffer.size() < 3) {
                break;
            }
            const size_t frame_size = static_cast<size_t>(buffer[2]) + 3;
            if (frame_size < 4 || frame_size > 64) {
                buffer.erase(buffer.begin());
                continue;
            }
            if (buffer.size() < frame_size) {
                break;
            }
            process_frame(std::vector<uint8_t>(buffer.begin(), buffer.begin() + frame_size));
            buffer.erase(buffer.begin(), buffer.begin() + frame_size);
        }
    }
}

void emptyImpl::process_frame(const std::vector<uint8_t>& frame) {
    if (frame.size() < 8 || frame[4] != (registration_button_vp >> 8) ||
        frame[5] != (registration_button_vp & 0xFF)) {
        return;
    }

    uint16_t button_value = 0;
    if (frame[3] == dwin_write_command) {
        button_value = static_cast<uint16_t>((frame[6] << 8) | frame[7]);
    } else if (frame[3] == dwin_touch_command) {
        if (frame.size() < 9 || frame[6] == 0 || frame.size() < 7 + frame[6] * 2) {
            return;
        }
        button_value = static_cast<uint16_t>((frame[7] << 8) | frame[8]);
    } else {
        return;
    }

    if (write_vp(registration_complete_vp, 0)) {
        registration_pending = true;
        EVLOG_info << "UserRegistration: Registration started from DWIN button VP1000, value=" << button_value
                   << " command=" << static_cast<unsigned int>(frame[3]);
    }
}

void emptyImpl::on_token(const types::authorization::ProvidedIdToken& token) {
    if (!registration_pending.exchange(false)) {
        return;
    }

    const std::string& uid = token.id_token.value;
    if (uid.empty()) {
        EVLOG_warning << "UserRegistration: Ignoring empty token UID";
        return;
    }

    const std::filesystem::path path(config.uid_file);
    std::error_code filesystem_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            EVLOG_error << "UserRegistration: Could not create UID directory: " << filesystem_error.message();
            return;
        }
    }

    std::ofstream uid_file(path, std::ios::app);
    if (!uid_file) {
        EVLOG_error << "UserRegistration: Failed to open UID file " << config.uid_file;
        return;
    }
    uid_file << uid << '\n';
    uid_file.flush();
    if (!uid_file) {
        EVLOG_error << "UserRegistration: Failed to store UID in " << config.uid_file;
        return;
    }

    EVLOG_info << "UserRegistration: Stored a user UID in " << config.uid_file;
    registration_complete();
}

void emptyImpl::registration_complete() {
    // Show registration successful
    if (!write_vp(registration_complete_vp, 1)) {
        EVLOG_error << "UserRegistration: Failed to set registration completion VP";
        return;
    }

    EVLOG_info << "UserRegistration: Registration completed successfully";

    // Keep success indication on the DWIN for 2 seconds
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Reset registration status
    if (!write_vp(registration_complete_vp, 0)) {
        EVLOG_error << "UserRegistration: Failed to reset registration completion VP";
    }

    // Switch from Screen 11 to Screen 0
    const std::array<uint8_t, 10> page_switch_packet = {
        0x5A, 0xA5,
        0x07,
        0x82,
        0x00, 0x84,
        0x5A, 0x01,
        0x00, 0x00
    };

    {
        std::lock_guard<std::mutex> lock(serial_write_mutex);

        if (serial_fd < 0) {
            EVLOG_error << "UserRegistration: DWIN serial port unavailable";
            return;
        }

        size_t written = 0;

        while (written < page_switch_packet.size()) {
            const ssize_t count =
                write(serial_fd,
                      page_switch_packet.data() + written,
                      page_switch_packet.size() - written);

            if (count > 0) {
                written += static_cast<size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                EVLOG_error << "UserRegistration: Failed to switch DWIN to screen 0: "
                            << strerror(errno);
                return;
            }
        }

        tcdrain(serial_fd);
    }

    EVLOG_info << "UserRegistration: Switched DWIN from screen 11 to screen 0";
}

bool emptyImpl::write_vp(uint16_t address, uint16_t value) {
    std::lock_guard<std::mutex> lock(serial_write_mutex);
    if (serial_fd < 0) {
        return false;
    }

    const std::array<uint8_t, 8> packet = {dwin_header[0],
                                           dwin_header[1],
                                           0x05,
                                           dwin_write_command,
                                           static_cast<uint8_t>(address >> 8),
                                           static_cast<uint8_t>(address & 0xFF),
                                           static_cast<uint8_t>(value >> 8),
                                           static_cast<uint8_t>(value & 0xFF)};
    size_t written = 0;
    while (written < packet.size()) {
        const ssize_t count = write(serial_fd, packet.data() + written, packet.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            EVLOG_error << "UserRegistration: Failed to write DWIN VP " << address << ": " << strerror(errno);
            return false;
        }
    }
    tcdrain(serial_fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return true;
}

} // namespace main
} // namespace module
