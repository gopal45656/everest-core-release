#include "analog_readerImpl.hpp"
#include <everest/logging.hpp>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <iomanip>
#include <vector>
#include <mutex>

// ---------------- Namespace ----------------
namespace module {
namespace analog_reader {

// ---------------- Constructor ----------------
analog_readerImpl::analog_readerImpl(
        Everest::ModuleAdapter* ev,
        const Everest::PtrContainer<AnalogChannelReader>& mod,
        const Conf& config)
    : analog_readerImplBase(ev, "analog_reader"),
      mod(mod),
      config(config),
      fd(-1),
      running(false)
{
}

// ---------------- Destructor ----------------
analog_readerImpl::~analog_readerImpl()
{
    running = false;

    if (poll_thread.joinable())
        poll_thread.join();

    close_serial();
}

// ---------------- Init ----------------
void analog_readerImpl::init()
{
    channel_values.resize(8, 0.0f);
}

// ---------------- Ready ----------------
void analog_readerImpl::ready()
{
    fd = open_serial();
    if (fd < 0) {
        EVLOG_error << "Failed to open serial port";
        return;
    }

    running = true;
    poll_thread = std::thread(&analog_readerImpl::polling_loop, this);
}

// ---------------- Open Serial ----------------
int analog_readerImpl::open_serial()
{
    int serial_fd = open(config.serial_port.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (serial_fd < 0) {
        perror("open");
        return -1;
    }

    struct termios tty{};
    if (tcgetattr(serial_fd, &tty) != 0) {
        perror("tcgetattr");
        close(serial_fd);
        return -1;
    }

    // Set baudrate
    speed_t speed = B9600;
    if (config.baudrate == 115200) speed = B115200;
    else if (config.baudrate == 19200) speed = B19200;
    else if (config.baudrate == 38400) speed = B38400;

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    // Data bits
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= (config.data_bits == 7) ? CS7 : CS8;

    // Parity
    if (config.parity == "even") {
        tty.c_cflag |= PARENB;
        tty.c_cflag &= ~PARODD;
    } else if (config.parity == "odd") {
        tty.c_cflag |= PARENB;
        tty.c_cflag |= PARODD;
    } else {
        tty.c_cflag &= ~PARENB;
    }

    // Stop bits
    if (config.stop_bits == 2)
        tty.c_cflag |= CSTOPB;
    else
        tty.c_cflag &= ~CSTOPB;

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;

    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_iflag = 0;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 10;

    if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(serial_fd);
        return -1;
    }

    EVLOG_info << "Serial port opened: " << config.serial_port;
    return serial_fd;
}

// ---------------- Close Serial ----------------
void analog_readerImpl::close_serial()
{
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

// ---------------- CRC16 ----------------
uint16_t analog_readerImpl::modbus_crc16(const std::vector<uint8_t>& data)
{
    uint16_t crc = 0xFFFF;

    for (auto byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

// ---------------- Read Input Register (Function 0x04) ----------------
bool analog_readerImpl::read_input_register(uint16_t start_reg,
                                            uint16_t quantity,
                                            uint16_t& value)
{
    std::vector<uint8_t> frame;

    frame.push_back(config.device_address);
    frame.push_back(0x04);                  //0x04 Read input register and 0x03 for Read holding register
    frame.push_back(start_reg >> 8);
    frame.push_back(start_reg & 0xFF);
    frame.push_back(quantity >> 8);
    frame.push_back(quantity & 0xFF);

    uint16_t crc = modbus_crc16(frame);
    frame.push_back(crc & 0xFF);
    frame.push_back((crc >> 8) & 0xFF);

    ssize_t written = write(fd, frame.data(), frame.size());
    if (written != static_cast<ssize_t>(frame.size())) {
        EVLOG_error << "Failed to write complete Modbus frame";
        return false;
    }

    tcdrain(fd);

    uint8_t buffer[256];
    int n = read(fd, buffer, sizeof(buffer));

    if (n < 7) {
        EVLOG_error << "Invalid Modbus response length";
        return false;
    }

    if (buffer[0] != config.device_address || buffer[1] != 0x04) {
        EVLOG_error << "Invalid Modbus response header";
        return false;
    }

    value = (buffer[3] << 8) | buffer[4];
    return true;
}

// ---------------- Polling Loop ----------------
void analog_readerImpl::polling_loop()
{
    const int ADC_RESOLUTION = 65535;  //16-bit ADC
    const float V_REF = config.reference_voltage;

    while (running) {

        for (int ch = 0; ch < 8; ++ch) {

            uint16_t raw_value = 0;

            if (read_input_register(static_cast<uint16_t>(ch), 0x0001, raw_value)) {

                float voltage = (raw_value / static_cast<float>(ADC_RESOLUTION)) * V_REF;
                float current_mA = voltage * (20.0f / V_REF);

                {
                    std::lock_guard<std::mutex> lock(data_mutex);
                    channel_values[ch] = voltage;
                }

                EVLOG_info << "Channel " << (ch + 1)
                           << " Voltage: " << std::fixed << std::setprecision(2)
                           << voltage << " V, Current: "
                           << std::fixed << std::setprecision(2)
                           << current_mA << " mA";
            } else {
                EVLOG_error << "Failed to read channel " << (ch + 1);
            }
        }

        // ✅ Delay AFTER all 8 channels are read
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}  // namespace analog_reader

} // namespace module

