// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "chademoImpl.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace module {
namespace main {

void chademoImpl::init() {
    // Open the CAN interface via the CanBus module.
    std::string ifname = "main_mcan0";
    mod->r_can_bus->call_open(ifname);

    // Subscribe to incoming CAN frames from the CanBus module.
    mod->r_can_bus->subscribe_last_frame([this](const Object& frame) {
        uint32_t id  = static_cast<uint32_t>(static_cast<int>(frame.at("id")));
        int dlc      = frame.at("dlc");
        const Array& json_data = frame.at("data");

        uint8_t raw[8] = {};
        int len = std::min(dlc, 8);
        for (int i = 0; i < len; ++i) {
            raw[i] = static_cast<uint8_t>(static_cast<int>(json_data[i]));
        }

        // Reset watchdog
        last_can_rx_ = std::chrono::steady_clock::now();

        // Forward raw frame to EvseManager via can_signal_receiver
        mod->p_can_signals->publish_can_frame(frame);

        // Dispatch into state machine
        handle_incoming_can(id, raw);
    });

    currentState = EVSE_STATE::WAIT_FOR_PLUG;
    running = true;
    EVLOG_info << "CHAdeMO Module Initialized — using CanBus module for CAN";
}

void chademoImpl::ready() {
    worker_thread = std::thread(&chademoImpl::run_state_machine, this);
}

void chademoImpl::run_state_machine() {
    uint16_t cfg_v = (uint16_t)mod->config.max_voltage_limit;
    uint8_t  cfg_i = (uint8_t)mod->config.max_current_limit;
    uint8_t  charger_temp = 35;
    bool     bsp_signal_sent = false;

    while (running) {
        // Safety watchdog — if no CAN frame received for 1s during active session.
        // Only active from WAIT_FOR_CONFIRMATION onwards.
        if (currentState != EVSE_STATE::WAIT_FOR_PLUG &&
            currentState != EVSE_STATE::WAIT_FOR_EV_100 &&
            currentState != EVSE_STATE::SEND_EVSE_PARAMS_108 &&
            currentState != EVSE_STATE::FAULT) {
            auto elapsed = std::chrono::steady_clock::now() - last_can_rx_.load();
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 1) {
                EVLOG_error << "Emergency: CAN Timeout (1s). Car disconnected or crashed.";
                handle_stop_charging();
                currentState = EVSE_STATE::FAULT;
            }
        }

        switch (currentState) {

            case EVSE_STATE::WAIT_FOR_PLUG: {
                std::string cmd = "gpioget /dev/gpiochip" +
                                  std::to_string(mod->config.presence_gpio_chip) +
                                  " " + std::to_string(mod->config.presence_gpio_line);
                int pin_value = 1;
                FILE* pipe = popen(cmd.c_str(), "r");
                if (pipe) {
                    char buffer[16];
                    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                        try { pin_value = std::stoi(buffer); } catch (...) { pin_value = 1; }
                    }
                    pclose(pipe);
                }
                if (pin_value == 0) {
                    EVLOG_info << "Vehicle Detected via /dev/gpiochip" << mod->config.presence_gpio_chip;
                    mod->r_bsp->call_enable(true);
                    last_can_rx_ = std::chrono::steady_clock::now();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    send_heartbeat_108(true, cfg_v, cfg_i, charger_temp);
                    currentState = EVSE_STATE::WAIT_FOR_EV_100;
                }
                break;
            }

            case EVSE_STATE::WAIT_FOR_EV_100: {
                send_heartbeat_108(true, cfg_v, cfg_i, charger_temp);
                // Check for unplug
                std::string cmd = "gpioget gpiochip" +
                                  std::to_string(mod->config.presence_gpio_chip) +
                                  " " + std::to_string(mod->config.presence_gpio_line);
                int pin_check = 0;
                FILE* pipe = popen(cmd.c_str(), "r");
                if (pipe) {
                    char buffer[16];
                    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                        try { pin_check = std::stoi(buffer); } catch (...) { pin_check = 0; }
                    }
                    pclose(pipe);
                }
                if (pin_check == 1) {
                    EVLOG_error << "Plug removed! Resetting.";
                    mod->r_bsp->call_enable(false);
                    currentState = EVSE_STATE::WAIT_FOR_PLUG;
                }
                break;
            }

            case EVSE_STATE::SEND_EVSE_PARAMS_108:
                send_heartbeat_108(true, cfg_v, cfg_i, charger_temp);
                EVLOG_info << "Scaling Power Supply to match EV Battery Voltage...";
                currentState = EVSE_STATE::PRECHARGE;
                break;

            case EVSE_STATE::PRECHARGE:
                send_heartbeat_108(true, cfg_v, cfg_i, charger_temp);
                currentState = EVSE_STATE::WAIT_FOR_CONFIRMATION;
                last_can_rx_ = std::chrono::steady_clock::now();
                break;

            case EVSE_STATE::WAIT_FOR_CONFIRMATION:
                send_heartbeat_108(true, cfg_v, cfg_i, charger_temp);
                break;

            case EVSE_STATE::SEND_IN2_109: {
                send_params_109(true, false);
                types::evse_board_support::PowerOnOff p_on{};
                p_on.allow_power_on = true;
                mod->r_bsp->call_allow_power_on(p_on);
                currentState = EVSE_STATE::WAIT_FOR_102;
                EVLOG_info << "Signal d1 (Charge Permission) HIGH via BSP";
                break;
            }

            case EVSE_STATE::WAIT_FOR_102:
                send_heartbeat_108(true, cfg_v, cfg_i, charger_temp);
                if (!bsp_signal_sent) {
                    send_params_109(true, false);
                    types::evse_board_support::PowerOnOff p_on{};
                    p_on.allow_power_on = true;
                    mod->r_bsp->call_allow_power_on(p_on);
                    bsp_signal_sent = true;
                    EVLOG_info << "BSP Power-On command sent once.";
                }
                send_params_109(true, false);
                break;

            case EVSE_STATE::CHARGING_OPERATIONAL:
                handle_start_charging();
                break;

            case EVSE_STATE::FAULT:
                send_params_109(false, true);
                handle_stop_charging();
                running = false;
                break;

            case EVSE_STATE::FINISHED:
                handle_stop_charging();
                running = false;
                break;

            default:
                break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(mod->config.publish_interval_ms));
    }
}

void chademoImpl::handle_incoming_can(uint32_t id, const uint8_t* data) {
    switch (id) {
        case 0x100: // EV Parameters — target voltage/current
            if (currentState == EVSE_STATE::WAIT_FOR_EV_100) {
                current_ev_req_v = static_cast<float>((data[1] << 8) | data[0]);
                current_ev_req_i = static_cast<float>(data[2]);
                currentState = EVSE_STATE::SEND_EVSE_PARAMS_108;
                EVLOG_info << "RX 0x100: EV Req " << current_ev_req_v.load()
                           << "V, " << (int)current_ev_req_i.load() << "A";
            }
            break;

        case 0x101: // EV Status / SOC
            if (currentState == EVSE_STATE::WAIT_FOR_CONFIRMATION) {
                current_ev_soc = data[2];

                std::stringstream ss;
                ss << std::hex << std::uppercase << std::setfill('0');
                for (int i = 0; i < 8; ++i) {
                    ss << std::setw(2) << static_cast<int>(data[i]) << (i < 7 ? " " : "");
                }
                EVLOG_info << "RX 0x101 Data: [" << ss.str() << "] | SOC: " << current_ev_soc.load() << "%";
                currentState = EVSE_STATE::SEND_IN2_109;
                EVLOG_info << "RX 0x101: EV SOC " << current_ev_soc.load() << "%";
            }
            break;

        case 0x102: { // EV Control — contactor status and faults
            bool ev_contactors_closed = (data[0] & 0x01);
            bool ev_fault = (data[4] == 0x01);

            if (ev_fault) {
                EVLOG_error << "EV reported Overtemp/Fault!";
                currentState = EVSE_STATE::FAULT;
            } else if (ev_contactors_closed && currentState == EVSE_STATE::WAIT_FOR_102) {
                types::evse_board_support::PowerOnOff p_on{};
                p_on.allow_power_on = true;
                mod->r_bsp->call_allow_power_on(p_on);
                currentState = EVSE_STATE::CHARGING_OPERATIONAL;
                EVLOG_info << "Relays Closed. Commencing Power Delivery.";
            }
            break;
        }

        default:
            break;
    }
}

void chademoImpl::send_via_canbus(uint32_t id, uint8_t dlc, const uint8_t* data) {
    Array payload;
    for (int i = 0; i < dlc; ++i) {
        payload.push_back(static_cast<int>(data[i]));
    }
    int can_id = static_cast<int>(id);
    bool extended = false; // CHAdeMO uses 11-bit standard IDs
    int len = static_cast<int>(dlc);
    mod->r_can_bus->call_send(can_id, extended, len, payload);
}

void chademoImpl::send_heartbeat_108(bool in1_high, uint16_t v, uint8_t i, uint8_t temp) {
    uint8_t data[8] = {0};
    data[0] = 0x01;
    data[1] = static_cast<uint8_t>(v & 0xFF);
    data[2] = static_cast<uint8_t>(v >> 8);
    data[3] = i;
    data[4] = in1_high ? 0x01 : 0x00;
    data[5] = temp;
    send_via_canbus(0x108, 8, data);
}

void chademoImpl::send_params_109(bool permission, bool fault) {
    uint8_t data[8] = {0};
    if (fault)      data[0] = 0x01;
    if (permission) data[1] = 0x01;
    uint16_t maxV = static_cast<uint16_t>(mod->config.max_voltage_limit);
    data[2] = maxV & 0xFF;
    data[3] = (maxV >> 8) & 0xFF;
    data[4] = static_cast<uint8_t>(mod->config.max_current_limit);
    data[6] = 0x02;
    send_via_canbus(0x109, 8, data);
}

void chademoImpl::handle_start_charging() {
    static float simulated_actual_v = 0.0f;

    float target_v  = current_ev_req_v.load();
    float target_i  = current_ev_req_i.load();
    float target_kw = (target_v * target_i) / 1000.0f;
    float current_kw = (simulated_actual_v * target_i) / 1000.0f;

    // Allow power on via BSP
    types::evse_board_support::PowerOnOff p_on{};
    p_on.allow_power_on = true;
    mod->r_bsp->call_allow_power_on(p_on);

    // Publish power request — EvseManager subscribes and drives the PSU
    Object power_req;
    power_req["voltage_V"] = static_cast<double>(simulated_actual_v);
    power_req["current_A"] = static_cast<double>(target_i);
    power_req["charge_mode"] = 0; // CHAdeMO uses constant current
    mod->p_can_signals->publish_power_request(power_req);

    EVLOG_info << "[CHAdeMO] Published power_request: "
               << simulated_actual_v << "V / " << target_i << "A";

    if (simulated_actual_v >= target_v || current_ev_soc.load() >= 100) {
        EVLOG_info << "!!! TARGET REACHED !!!";
        EVLOG_info << "Final Power: " << current_kw << " kW at " << simulated_actual_v << "V";
        currentState = EVSE_STATE::FINISHED;
        return;
    }

    if (simulated_actual_v < mod->config.max_voltage_limit) {
        simulated_actual_v += 5.0f;
    }

    static auto last_log = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - last_log).count() >= 2) {
        EVLOG_info << ">>> [POWER UPDATE] <<<";
        EVLOG_info << "Target: " << target_kw << " kW (" << target_v << "V / " << target_i << "A)";
        EVLOG_info << "Actual: " << current_kw << " kW (" << simulated_actual_v << "V / " << target_i << "A)";
        last_log = std::chrono::steady_clock::now();
    }

    uint8_t charger_temp = 35;
    send_heartbeat_108(true, static_cast<uint16_t>(simulated_actual_v),
                       static_cast<uint8_t>(target_i), charger_temp);

    if (current_ev_soc.load() >= 100) {
        currentState = EVSE_STATE::FINISHED;
    }
}

void chademoImpl::handle_stop_charging() {
    EVLOG_info << "Shutting down DC Power Supply.";
    bsp_signal_sent = false;
    send_params_109(false, false); // IN2 Low — EV opens contactors

    // Publish a zero power_request so EvseManager turns the PSU off
    Object stop_req;
    stop_req["voltage_V"] = 0.0;
    stop_req["current_A"] = 0.0;
    stop_req["charge_mode"] = 0;
    mod->p_can_signals->publish_power_request(stop_req);

    types::evse_board_support::PowerOnOff p_off{};
    p_off.allow_power_on = false;
    mod->r_bsp->call_allow_power_on(p_off);
    EVLOG_info << "Physical Contactors OPENED via BSP.";
}

} // namespace main
} // namespace module
