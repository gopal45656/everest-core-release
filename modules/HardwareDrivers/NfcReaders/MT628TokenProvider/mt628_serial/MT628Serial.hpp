// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest

#ifndef MT628_SERIAL_HPP
#define MT628_SERIAL_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace mt628 {

/// Implements the serial communication protocol for the MT318-620V2.4 NFC card reader.
///
/// Frame format (host -> reader):
///   STX (0x02) | LEN_H | LEN_L | CMD_WORD | APP_DATA... | ETX (0x03) | BCC
/// Where LEN is the count of bytes from CMD_WORD up to (but not including) ETX,
/// and BCC is the XOR of all bytes from LEN_H through ETX (inclusive).
///
/// Frame format (reader -> host):
///   STX (0x02) | LEN_H | LEN_L | CMD_WORD | STATUS | APP_DATA... | ETX (0x03) | BCC
class MT628Serial {
public:
    MT628Serial() = default;
    ~MT628Serial();

    /// Open the serial port at the given path and baud rate.
    /// Returns true on success.
    bool open(const std::string& port, int baud_rate);

    /// Close the serial port.
    void close();

    /// Returns true if the port is open.
    bool isOpen() const;

    /// Send "Find RF card" command (0x34, 0x30).
    /// Returns true if the reader reports a card is present ('Y' = 0x59 status).
    bool findCard();

    /// Send "Get Mifare1 card serial number" command (0x34, 0x31).
    /// On success, returns the 4-byte UID as a hex string (e.g. "C6B272AE").
    /// Returns an empty string on failure.
    std::string getCardUID();

    /// Send "Reader Soft Reset" command (0x31, 0x55).
    void softReset();

    /// Trigger the buzzer.
    /// count     = number of beeps (1-255)
    /// duration_ms = length of each beep in milliseconds (0-65535)
    /// The reader sends its response first, then performs the beep.
    void buzz(uint8_t count = 1, uint16_t duration_ms = 100);

    /// Enable verbose debug output.
    void enableDebug(bool enable);

private:
    int fd_{-1};
    bool debug_{false};

    /// Build a command frame and return the full byte buffer to send.
    std::vector<uint8_t> buildFrame(uint8_t cmd_word, uint8_t cmd_param,
                                    const std::vector<uint8_t>& app_data = {}) const;

    /// Write raw bytes to the serial port.
    bool writeBytes(const std::vector<uint8_t>& data);

    /// Read exactly n bytes from the serial port within timeout_ms milliseconds.
    /// Returns the bytes read; may be shorter than n on timeout.
    std::vector<uint8_t> readBytes(size_t n, int timeout_ms);

    /// Read a complete response frame from the reader.
    /// Validates BCC. Returns the application data (without STX/LEN/CMD/STATUS/ETX/BCC).
    /// status_out is set to the status byte ('Y'=success, 'N'=failure, etc.).
    /// Returns false if the frame is malformed or a timeout occurs.
    bool readResponse(uint8_t expected_cmd_word, uint8_t expected_cmd_param,
                      char& status_out, std::vector<uint8_t>& app_data_out);

    /// Compute BCC (XOR of all bytes in the range).
    static uint8_t computeBCC(const std::vector<uint8_t>& data, size_t start, size_t end);

    static constexpr uint8_t STX = 0x02;
    static constexpr uint8_t ETX = 0x03;

    // Per-read timeout: 200 ms is generous for a local serial device.
    static constexpr int READ_TIMEOUT_MS = 200;
};

} // namespace mt628

#endif // MT628_SERIAL_HPP
