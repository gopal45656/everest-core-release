#ifndef MAIN_CHADEMO_IMPL_HPP
#define MAIN_CHADEMO_IMPL_HPP

#include <thread>
#include <atomic>
#include <chrono>

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 3
//

#include <generated/interfaces/chademo/Implementation.hpp>

#include "../Chademo.hpp"

// ev@75ac1216-19eb-4182-a85c-820f1fc2c091:v1
// insert your custom include headers here
// ev@75ac1216-19eb-4182-a85c-820f1fc2c091:v1

namespace module {
namespace main {

struct Conf {};

class chademoImpl : public chademoImplBase {
public:
    chademoImpl() = delete;
    chademoImpl(Everest::ModuleAdapter* ev, const Everest::PtrContainer<Chademo>& mod, Conf& config) :
        chademoImplBase(ev, "main"), mod(mod), config(config){};

    // ev@8ea32d28-373f-4c90-ae5e-b4fcc74e2a61:v1
    // insert your public definitions here
    // ev@8ea32d28-373f-4c90-ae5e-b4fcc74e2a61:v1

protected:
    // command handler functions (virtual)
    virtual void handle_start_charging() override;
    virtual void handle_stop_charging() override;

    // ev@d2d1847a-7b88-41dd-ad07-92785f06f5c4:v1
    // insert your protected definitions here
    // ev@d2d1847a-7b88-41dd-ad07-92785f06f5c4:v1

private:
    const Everest::PtrContainer<Chademo>& mod;
    const Conf& config;

    bool bsp_signal_sent{false};
    std::thread worker_thread;
    bool running{false};

    enum class EVSE_STATE {
        WAIT_FOR_PLUG,
        WAIT_FOR_EV_100,
        SEND_EVSE_PARAMS_108,
        PRECHARGE,
        WAIT_FOR_CONFIRMATION,
        SEND_IN2_109,
        WAIT_FOR_102,
        CHARGING_OPERATIONAL,
        HANDSHAKE,
        CHG_PARAMETERS,
        CHARGING,
        FINISHED,
        FAULT
    };
    EVSE_STATE currentState{EVSE_STATE::WAIT_FOR_PLUG};

    // CHAdeMO signal variables — updated by subscribe_last_frame callback
    std::atomic<float> current_ev_req_v{0};
    std::atomic<float> current_ev_req_i{0};
    std::atomic<int>   current_ev_soc{0};

    // Watchdog: updated on every received CAN frame
    std::atomic<std::chrono::steady_clock::time_point> last_can_rx_{
        std::chrono::steady_clock::now()};

    void run_state_machine();

    // Dispatch a received CAN frame into the state machine
    void handle_incoming_can(uint32_t id, const uint8_t* data);

    // Send a CAN frame via the CanBus module
    void send_via_canbus(uint32_t id, uint8_t dlc, const uint8_t* data);

    void send_heartbeat_108(bool in1_high, uint16_t v, uint8_t i, uint8_t temp);
    void send_params_109(bool permission, bool fault);

    virtual void init() override;
    virtual void ready() override;

    // ev@3370e4dd-95f4-47a9-aaec-ea76f34a66c9:v1
    // insert your private definitions here
    // ev@3370e4dd-95f4-47a9-aaec-ea76f34a66c9:v1
};

// ev@3d7da0ad-02c2-493d-9920-0bbbd56b9876:v1
// insert other definitions here
// ev@3d7da0ad-02c2-493d-9920-0bbbd56b9876:v1

} // namespace main
} // namespace module

#endif // MAIN_CHADEMO_IMPL_HPP
