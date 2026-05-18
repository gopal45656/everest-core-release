// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "CanBusToEvseManager.hpp"
#include "GbtProtocol.hpp"

#include <iomanip>
#include <memory>
#include <sstream>

namespace module {

void CanBusToEvseManager::init() {
    invoke_init(*p_can_signals);
}

void CanBusToEvseManager::ready() {
    invoke_ready(*p_can_signals);

    // Build the GB/T 27930 protocol handler.
    auto protocol = std::make_shared<GbtProtocol>(
        [this](uint32_t id, bool extended, int dlc,
               const std::array<uint8_t, 8>& data) {
            Array payload;
            for (int i = 0; i < dlc; ++i) {
                payload.push_back(static_cast<int>(data[i]));
            }
            int can_id = static_cast<int>(id);
            r_can_bus->call_send(can_id, extended, dlc, payload);
        });

    // Configure charger parameters
    protocol->charger_max_voltage_V = 1000.0;
    protocol->charger_max_current_A = 250.0;
    protocol->charger_type = 1;
    protocol->charger_temp_C = 25.0;

    // Subscribe to incoming CAN frames.
    r_can_bus->subscribe_last_frame([this, protocol](const Object& frame) {
        uint32_t id  = static_cast<uint32_t>(static_cast<int>(frame.at("id")));
        bool extended = frame.at("extended");
        int dlc       = frame.at("dlc");
        const Array& json_data = frame.at("data");

        uint8_t raw[8] = {};
        int len = std::min(dlc, 8);
        for (int i = 0; i < len; ++i) {
            raw[i] = static_cast<uint8_t>(static_cast<int>(json_data[i]));
        }

        if (config.log_frames) {
            std::stringstream ss;
            for (int i = 0; i < len; ++i) {
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << std::uppercase << static_cast<int>(raw[i]) << " ";
            }
            EVLOG_info << "[CanBusToEvseManager] RX id=0x"
                       << std::hex << std::uppercase << id
                       << " dlc=" << std::dec << dlc
                       << " data=[" << ss.str() << "]"
                       << " state=" << protocol->state_name();
        }

        // Run the GB/T 27930 state machine
        protocol->on_frame(id, extended, dlc, raw);

        // ----------------------------------------------------------------
        // When BCL arrives during Charging, publish a power_request var.
        // EvseManager subscribes to this and calls update_local_energy_limit()
        // which flows through EnergyManager → energyImpl → powersupply_DC_set().
        // The bridge never touches the power supply directly.
        // ----------------------------------------------------------------
        if (protocol->state() == GbtProtocol::State::Charging &&
            id == GBT27930_BCL_FRAME_ID) {

            EVLOG_info << "[CanBusToEvseManager] Publishing power request: "
                       << protocol->bms.requested_voltage_V << "V / "
                       << protocol->bms.requested_current_A << "A";

            Object power_req;
            power_req["voltage_V"] = protocol->bms.requested_voltage_V;
            power_req["current_A"] = protocol->bms.requested_current_A;
            power_req["charge_mode"] = static_cast<int>(protocol->bms.charge_mode);
            p_can_signals->publish_power_request(power_req);
        }

        // Forward the raw frame to EvseManager via can_signal_receiver
        p_can_signals->publish_can_frame(frame);
    });
}

} // namespace module
