// SPDX-License-Identifier: Apache-2.0
#ifndef MAIN_AUTH_TOKEN_PROVIDER_IMPL_HPP
#define MAIN_AUTH_TOKEN_PROVIDER_IMPL_HPP

#include <generated/interfaces/auth_token_provider/Implementation.hpp>
#include "../RC522TokenProvider.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <regex>
#include <string>
#include <thread>

namespace module {
namespace main {

struct Conf {
    // existing fields from your manifest mapping
    std::string token;
    std::string type;
    double timeout;
    int connector_id;

    // serial-related fields you added
    std::string mode;            // "dummy" | "serial_push" | "serial_on_auth_required"
    std::string serial_device;
    int baudrate;
    int data_bits;
    std::string parity;          // "none" | "even" | "odd"
    int stop_bits;
    std::string line_delimiter;
    std::string token_regex;
};

class auth_token_providerImpl : public auth_token_providerImplBase {
public:
    auth_token_providerImpl() = delete;
    auth_token_providerImpl(Everest::ModuleAdapter* ev,
                            const Everest::PtrContainer<RC522TokenProvider>& mod,
                            Conf& config)
        : auth_token_providerImplBase(ev, "main"), mod(mod), config(config) {}

    ~auth_token_providerImpl() override;  // destructor

private:
    const Everest::PtrContainer<RC522TokenProvider>& mod;
    const Conf& config;

    // Worker/queues/regex
    std::thread serial_thread_;
    std::atomic<bool> running_{false};
    std::string pending_;
    std::regex token_re_;
    std::mutex q_mtx_;
    std::condition_variable q_cv_;
    std::queue<std::string> token_queue_;

    // lifecycle from framework
    void init() override;
    void ready() override;

    // ---- DECLARE ALL HELPERS YOU DEFINE IN THE .CPP ----
    void start_serial_if_needed();                      // matches cpp: void auth_token_providerImpl::start_serial_if_needed()
    bool open_and_configure_serial(int& fd);           // matches cpp: bool auth_token_providerImpl::open_and_configure_serial(int& fd)
    void close_serial(int fd);                         // matches cpp
    void serial_loop();                                // matches cpp
    void handle_line(const std::string& line);         // matches cpp
    void publish_token_string(const std::string& token_str); // matches cpp
};

} // namespace main
} // namespace module

#endif // MAIN_AUTH_TOKEN_PROVIDER_IMPL_HPP