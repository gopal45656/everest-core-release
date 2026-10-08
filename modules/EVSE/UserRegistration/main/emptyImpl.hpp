// SPDX-License-Identifier: Apache-2.0

#ifndef MAIN_EMPTY_IMPL_HPP
#define MAIN_EMPTY_IMPL_HPP

#include <generated/interfaces/empty/Implementation.hpp>

#include "../UserRegistration.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace module {
namespace main {

struct Conf {
    std::string display_serial_port;
    int display_baud_rate;
    std::string uid_file;
};

class emptyImpl : public emptyImplBase {
public:
    emptyImpl() = delete;
    emptyImpl(Everest::ModuleAdapter* ev, const Everest::PtrContainer<UserRegistration>& mod, Conf& config);
    ~emptyImpl() override;

protected:
    void init() override;
    void ready() override;

private:
    const Everest::PtrContainer<UserRegistration>& mod;
    const Conf& config;
    int serial_fd{-1};
    std::atomic<bool> running{false};
    std::atomic<bool> registration_pending{false};
    std::mutex serial_write_mutex;
    std::thread input_thread;

    void open_display();
    void read_display();
    void process_frame(const std::vector<uint8_t>& frame);
    void on_token(const types::authorization::ProvidedIdToken& token);
    void registration_complete();
    bool write_vp(uint16_t address, uint16_t value);
};

} // namespace main
} // namespace module

#endif // MAIN_EMPTY_IMPL_HPP
