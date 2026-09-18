// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
//
// CAN driver for Tonhe charging modules (THJS-TXXY-0060 V1.2).
// Controls up to 240 modules sharing a single 125 kbps CAN bus.
//
// Protocol highlights
//   • SAE J1939-style 29-bit extended frames
//   • Master address: 0xA0, module addresses: 0x01–0xF0 (auto-discovered)
//   • Little-endian signals
//   • Modules transmit status autonomously every ~500 ms (no polling required)
//   • Controller sends C_M_2 (broadcast setpoint) and C_M_3 (keepalive) every 500 ms
//   • Individual setpoints sent via C_M_24 per module for current sharing
//   • Unknown module addresses are auto-added on first reception (auto-discovery)

#ifndef INFY_CAN_DEVICE_HPP
#define INFY_CAN_DEVICE_HPP

#include "CanDevice.hpp"
#include "TonheCanPackets.hpp"
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <ostream>
#include <sigslot/signal.hpp>
#include <string>
#include <thread>
#include <vector>

class InfyCanDevice : public CanDevice {
public:
    InfyCanDevice();
    ~InfyCanDevice();

    // -----------------------------------------------------------------------
    // Configuration (call before open())
    // -----------------------------------------------------------------------

    void set_can_device(const std::string& dev);
    void set_config_values(const std::string& module_addresses,
                           int   heartbeat_timeout_ms,
                           float overtemperature_limit_C,
                           float per_module_rated_power_W,
                           float conversion_efficiency);

    /// Open SocketCAN interface and start TX thread.
    bool open();

    /// Legacy API: open_device(name) = set_can_device(name) + open()
    bool open_device(const char* dev);

    /// Close SocketCAN interface.
    bool close_device();

    // -----------------------------------------------------------------------
    // Commands
    // -----------------------------------------------------------------------

    /// Enable or disable output on all configured modules.
    bool switch_on_off(bool on);

    /// Set the aggregate voltage and total current demand.
    /// The driver divides current equally among active modules.
    bool set_voltage_current(float voltage_V, float total_current_A);

    // Legacy stubs kept for API compatibility (no-ops on Tonhe hardware)
    bool set_walkin_enabled(bool /*on*/) { return true; }
    bool set_inverter_mode(bool /*inv*/) { return true; }
    bool adjust_power_factor(float /*pf*/) { return true; }

    /// OutputMode enum kept for test-tool compat — Tonhe hardware uses
    /// parallel output mode by default; this setting has no effect.
    enum class OutputMode { Parallel = 0, Series = 1, Automatic = 2 };
    bool set_output_mode(OutputMode /*mode*/) { return true; }
    bool set_output_mode(int /*mode*/) { return true; }
    bool set_generic_setting(uint8_t, uint8_t, uint32_t) { return true; }

    /// request_rx kept for test-tool compat — Tonhe modules push status
    /// autonomously so there is no poll/request mechanism.
    bool request_rx(uint8_t /*addr*/, const std::vector<uint8_t>& /*payload*/) { return true; }

    // -----------------------------------------------------------------------
    // Per-module telemetry
    // -----------------------------------------------------------------------

    struct ModuleTelemetry {
        float   output_voltage_V{0.f};
        float   output_current_A{0.f};
        float   ambient_temp_C{0.f};
        float   a_phase_V{0.f};
        float   b_phase_V{0.f};
        float   c_phase_V{0.f};

        // Capability limits (rated per module — set from config, not from bus)
        float   dc_max_output_voltage{1000.f};
        float   dc_min_output_voltage{200.f};
        float   dc_max_output_current{30.f};
        float   dc_rated_output_power{30000.f};
        bool    valid_caps{true};           // always true — from config

        tonhe::MC1Frame::State module_state{tonhe::MC1Frame::State::NormalOff};
        uint8_t  fault_bits{0};    // fault_lo byte from MC1Frame
        uint16_t ext_fault_bits{0};

        std::chrono::steady_clock::time_point last_heartbeat;
        bool online{false};
    };

    using TelemetryMap = std::map<uint8_t, ModuleTelemetry>;

    TelemetryMap get_telemetry_snapshot() const;
    size_t       active_module_count() const;

    // -----------------------------------------------------------------------
    // Signals
    // -----------------------------------------------------------------------

    /// Emitted with aggregated voltage and total current (~2 Hz).
    sigslot::signal<float, float> signalVoltageCurrent;

    /// Emitted when any module's operational mode changes.
    /// Carries a synthetic PowerModuleStatus and InverterStatus for
    /// backward-compatibility with power_supply_DCImpl.
    sigslot::signal<can_packet_acdc::PowerModuleStatus,
                    can_packet_acdc::InverterStatus> signalModuleStatus;

    /// Emitted whenever active module count changes or on startup.
    sigslot::signal<TelemetryMap> signalCapabilitiesUpdate;

    /// Per-module error events: (node_id, error_string, is_active).
    sigslot::signal<uint8_t, std::string, bool> signalModuleError;

    // -----------------------------------------------------------------------
    // Legacy telemetry struct (kept for test-tool compatibility)
    // -----------------------------------------------------------------------
    struct LegacyTelemetry {
        float output_voltage_V{0.f};
        float output_current_A{0.f};
        float ambient_temperature{0.f};
        float ac_phase_a_voltage{0.f};
        float ac_phase_b_voltage{0.f};
        float ac_phase_c_voltage{0.f};
        float dc_max_output_voltage{0.f};
        float dc_min_output_voltage{0.f};
        float dc_max_output_current{0.f};
        float dc_rated_output_power{0.f};
        // Kept zero for compat
        float ac_ab_line_voltage{0.f};
        float ac_bc_line_voltage{0.f};
        float ac_ca_line_voltage{0.f};
        float dc_high_side_voltage{0.f};
        float dc_high_side_current{0.f};
        can_packet_acdc::SystemDCVoltage   voltage;
        can_packet_acdc::SystemDCCurrent   current;
        can_packet_acdc::PowerModuleStatus status;

        friend std::ostream& operator<<(std::ostream& out, const LegacyTelemetry& self);
    } telemetry;

protected:
    void rx_handler(uint32_t can_id, const std::vector<uint8_t>& payload) override;

private:
    // Config
    std::string          can_device_name;
    std::vector<uint8_t> configured_node_ids;
    int                  heartbeat_timeout_ms{60000};
    float                overtemperature_limit_C{85.f};
    float                per_module_rated_power_W{30000.f};
    float                conversion_efficiency{0.95f};

    // Runtime state
    mutable std::mutex   telemetry_mutex;
    TelemetryMap         module_telemetry;

    std::mutex setpoint_mutex;
    float      last_voltage{0.f};
    float      last_total_current{0.f};
    bool       output_enabled{false};

    // TX thread
    std::atomic_bool exit_tx_thread{false};
    std::thread      tx_thread_handle;
    void             tx_thread_func();

    // Helpers
    void check_heartbeats();
    void send_setpoints();
    void emit_capabilities_update();
    void update_module_online_state(uint8_t node_id, bool online);
    void handle_mc1(uint8_t mod_addr, const uint8_t* data);
    void handle_mc2(uint8_t mod_addr, const uint8_t* data);
    void handle_mc3(uint8_t mod_addr, const uint8_t* data);
    void handle_mc4(uint8_t mod_addr, const uint8_t* data);

    bool tx(uint32_t can_id, const std::vector<uint8_t>& payload);
    uint8_t throttle_cnt{0};
};

#endif // INFY_CAN_DEVICE_HPP
