#ifndef ANALOG_READER_ANALOG_READER_IMPL_HPP
#define ANALOG_READER_ANALOG_READER_IMPL_HPP

#include <generated/interfaces/analog_reader/Implementation.hpp>
#include "../AnalogChannelReader.hpp"

#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <cstdint>

namespace module {
namespace analog_reader {

// ---------------- Configuration Structure ----------------
struct Conf {
    std::string serial_port{"/dev/ttyUSB0"};
    int baudrate{9600};
    std::string parity{"none"};
    int data_bits{8};
    int stop_bits{1};
    int polling_interval_ms{4000};

    double reference_voltage{5.0};   // REQUIRED by generated EVerest code

    uint8_t device_address{1};
};

// ---------------- Implementation Class ----------------
class analog_readerImpl : public analog_readerImplBase {
public:
    analog_readerImpl() = delete;

    analog_readerImpl(Everest::ModuleAdapter* ev,
                      const Everest::PtrContainer<AnalogChannelReader>& mod,
                      const Conf& config);

    ~analog_readerImpl();

protected:
    void init() override;
    void ready() override;

private:
    const Everest::PtrContainer<AnalogChannelReader>& mod;
    Conf config;

    std::thread poll_thread;
    std::atomic<bool> running{false};
    std::vector<float> channel_values;
    std::mutex data_mutex;
    int fd{-1};

    // Serial
    int open_serial();
    void close_serial();

    // Polling
    void polling_loop();

    // Modbus
    uint16_t modbus_crc16(const std::vector<uint8_t>& data);

    bool read_input_register(uint16_t start_reg,
                             uint16_t quantity,
                             uint16_t& value);
};

} // namespace analog_reader
} // namespace module

#endif
  
