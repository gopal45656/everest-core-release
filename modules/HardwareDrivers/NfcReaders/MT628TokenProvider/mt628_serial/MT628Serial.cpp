// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest

#include "MT628Serial.hpp"

#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace mt628 {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static speed_t baudToSpeed(int baud) {
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return B115200;
    }
}

static std::string bytesToHex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    for (auto b : data)
        oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << ' ';
    return oss.str();
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

MT628Serial::~MT628Serial() {
    close();
}

bool MT628Serial::open(const std::string& port, int baud_rate) {
    fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        std::cerr << "[MT628] open(" << port << ") failed: " << std::strerror(errno) << "\n";
        return false;
    }

    struct termios tty {};
    if (tcgetattr(fd_, &tty) != 0) {
        std::cerr << "[MT628] tcgetattr failed: " << std::strerror(errno) << "\n";
        ::close(fd_); fd_ = -1;
        return false;
    }

    cfsetispeed(&tty, baudToSpeed(baud_rate));
    cfsetospeed(&tty, baudToSpeed(baud_rate));

    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |=  CS8 | CREAD | CLOCAL;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        std::cerr << "[MT628] tcsetattr failed: " << std::strerror(errno) << "\n";
        ::close(fd_); fd_ = -1;
        return false;
    }

    tcflush(fd_, TCIOFLUSH);
    return true;
}

void MT628Serial::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool MT628Serial::isOpen() const { return fd_ >= 0; }
void MT628Serial::enableDebug(bool en) { debug_ = en; }

// ---------------------------------------------------------------------------
// Frame building
// ---------------------------------------------------------------------------
//
// Host → Reader command frame:
//   STX | LEN_H | LEN_L | CMD_WORD | CMD_PARAM | [APP_DATA...] | ETX | BCC
//
// LEN = count of bytes from CMD_WORD through APP_DATA (not STX, LEN, ETX, BCC).
//   Bare command (no app_data): LEN = 2
//
// BCC = XOR of ALL bytes from STX through ETX inclusive.
//
// Verified against protocol spec examples:
//   Find card:  02 00 02 34 30 03 07
//     BCC = 02^00^02^34^30^03 = 07 ✓
//   Get UID:    02 00 02 34 31 03 06
//     BCC = 02^00^02^34^31^03 = 06 ✓

std::vector<uint8_t> MT628Serial::buildFrame(uint8_t cmd_word, uint8_t cmd_param,
                                              const std::vector<uint8_t>& app_data) const {
    uint16_t len = static_cast<uint16_t>(2 + app_data.size()); // cmd_word + cmd_param + app_data

    std::vector<uint8_t> frame;
    frame.push_back(STX);
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
    frame.push_back(cmd_word);
    frame.push_back(cmd_param);
    for (auto b : app_data)
        frame.push_back(b);
    frame.push_back(ETX);

    // BCC = XOR of every byte from STX through ETX
    uint8_t bcc = 0;
    for (auto b : frame)
        bcc ^= b;
    frame.push_back(bcc);

    return frame;
}

// ---------------------------------------------------------------------------
// Serial I/O
// ---------------------------------------------------------------------------

bool MT628Serial::writeBytes(const std::vector<uint8_t>& data) {
    if (fd_ < 0) return false;
    // Flush stale incoming bytes before each new command
    tcflush(fd_, TCIFLUSH);
    if (debug_)
        std::cerr << "[MT628] TX: " << bytesToHex(data) << "\n";
    return ::write(fd_, data.data(), data.size()) == static_cast<ssize_t>(data.size());
}

std::vector<uint8_t> MT628Serial::readBytes(size_t n, int timeout_ms) {
    std::vector<uint8_t> buf;
    buf.reserve(n);
    while (buf.size() < n) {
        struct pollfd pfd { fd_, POLLIN, 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) break;
        if (!(pfd.revents & POLLIN)) break;
        uint8_t b = 0;
        if (::read(fd_, &b, 1) == 1)
            buf.push_back(b);
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Response parsing
// ---------------------------------------------------------------------------
//
// Reader → Host response frame:
//   STX | LEN_H | LEN_L | CMD_WORD | CMD_PARAM | STATUS | [APP_DATA...] | ETX | BCC
//
// LEN = cmd_word(1) + cmd_param(1) + status(1) + app_data(N) → minimum = 3
// BCC = XOR of ALL bytes from STX through ETX inclusive.
//
// Example find card response:  02 00 03 34 30 59 03 BCC
// Example get UID response:    02 00 07 34 31 59 UID[4] 03 BCC

bool MT628Serial::readResponse(uint8_t expected_cmd_word, uint8_t expected_cmd_param,
                                char& status_out, std::vector<uint8_t>& app_data_out) {
    app_data_out.clear();

    // STX
    auto b = readBytes(1, READ_TIMEOUT_MS);
    if (b.empty() || b[0] != STX) {
        if (debug_)
            std::cerr << "[MT628] No STX" << (b.empty() ? "" : ", got: " + bytesToHex(b)) << "\n";
        return false;
    }

    // LEN (2 bytes)
    auto len_bytes = readBytes(2, READ_TIMEOUT_MS);
    if (len_bytes.size() < 2) {
        if (debug_) std::cerr << "[MT628] Timeout reading LEN\n";
        return false;
    }
    uint16_t len = (static_cast<uint16_t>(len_bytes[0]) << 8) | len_bytes[1];

    if (len < 3) {
        if (debug_) std::cerr << "[MT628] LEN=" << len << " too short (expected >=3)\n";
        return false;
    }

    // Payload (len bytes) + ETX + BCC
    auto body = readBytes(static_cast<size_t>(len) + 2, READ_TIMEOUT_MS);
    if (body.size() < static_cast<size_t>(len) + 2) {
        if (debug_)
            std::cerr << "[MT628] Short read: got " << body.size()
                      << ", expected " << (len + 2) << "\n";
        return false;
    }

    uint8_t rcv_cmd_word  = body[0];
    uint8_t rcv_cmd_param = body[1];
    uint8_t rcv_status    = body[2];
    uint8_t etx           = body[len];
    uint8_t rcv_bcc       = body[len + 1];

    if (debug_) {
        std::cerr << "[MT628] RX: STX=" << bytesToHex({STX})
                  << "LEN=" << bytesToHex(len_bytes)
                  << "BODY=" << bytesToHex(std::vector<uint8_t>(body.begin(), body.begin() + len))
                  << "ETX=" << bytesToHex({etx})
                  << "BCC=" << bytesToHex({rcv_bcc}) << "\n";
    }

    if (etx != ETX) {
        if (debug_) std::cerr << "[MT628] ETX mismatch: 0x" << std::hex << static_cast<int>(etx) << "\n";
        return false;
    }

    // BCC = XOR of STX + LEN_H + LEN_L + payload bytes + ETX
    uint8_t exp_bcc = STX;
    exp_bcc ^= len_bytes[0];
    exp_bcc ^= len_bytes[1];
    for (size_t i = 0; i < len; ++i)
        exp_bcc ^= body[i];
    exp_bcc ^= etx;

    if (rcv_bcc != exp_bcc) {
        if (debug_)
            std::cerr << "[MT628] BCC mismatch: got 0x" << std::hex << static_cast<int>(rcv_bcc)
                      << " expected 0x" << static_cast<int>(exp_bcc) << "\n";
        return false;
    }

    if (rcv_cmd_word != expected_cmd_word || rcv_cmd_param != expected_cmd_param) {
        if (debug_)
            std::cerr << "[MT628] CMD echo mismatch: got 0x"
                      << std::hex << static_cast<int>(rcv_cmd_word) << " 0x"
                      << static_cast<int>(rcv_cmd_param) << "\n";
        return false;
    }

    status_out = static_cast<char>(rcv_status);
    for (size_t i = 3; i < len; ++i)
        app_data_out.push_back(body[i]);

    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

bool MT628Serial::findCard() {
    auto frame = buildFrame(0x34, 0x30);
    if (!writeBytes(frame)) return false;

    char status = 0;
    std::vector<uint8_t> dummy;
    if (!readResponse(0x34, 0x30, status, dummy)) return false;

    if (debug_)
        std::cerr << "[MT628] findCard: status='" << status << "' (0x"
                  << std::hex << static_cast<int>(static_cast<uint8_t>(status)) << ")\n";
    return status == 'Y';
}

std::string MT628Serial::getCardUID() {
    auto frame = buildFrame(0x34, 0x31);
    if (!writeBytes(frame)) return {};

    char status = 0;
    std::vector<uint8_t> uid_bytes;
    if (!readResponse(0x34, 0x31, status, uid_bytes)) return {};

    if (status != 'Y' || uid_bytes.size() < 4) {
        if (debug_)
            std::cerr << "[MT628] getCardUID: status='" << status
                      << "' uid_bytes=" << uid_bytes.size() << "\n";
        return {};
    }

    std::ostringstream oss;
    for (int i = 0; i < 4; ++i)
        oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(uid_bytes[i]);
    return oss.str();
}

void MT628Serial::softReset() {
    // Reader responds immediately then resets after ~6s; ignore response
    writeBytes(buildFrame(0x31, 0x55));
}

void MT628Serial::buzz(uint8_t count, uint16_t duration_ms) {
    // Buzzer command: CMD_WORD=0x31, CMD_PARAM=0x13
    // App data: Times(1) | Duration_H(1) | Duration_L(1)
    // Host:   02 00 05 31 13 <count> <dur_H> <dur_L> 03 BCC
    // Reader: 02 00 03 31 3E 59 03 BCC  (responds first, beeps after)
    std::vector<uint8_t> app_data = {
        count,
        static_cast<uint8_t>((duration_ms >> 8) & 0xFF),
        static_cast<uint8_t>(duration_ms & 0xFF)
    };
    auto frame = buildFrame(0x31, 0x13, app_data);
    if (!writeBytes(frame)) return;

    // Read and discard the response — the reader beeps after responding
    char status = 0;
    std::vector<uint8_t> dummy;
    readResponse(0x31, 0x3E, status, dummy);
}

} // namespace mt628
