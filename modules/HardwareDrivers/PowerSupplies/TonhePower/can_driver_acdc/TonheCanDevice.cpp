// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
//
// Tonhe charging module CAN driver implementation.
// Protocol: THJS-TXXY-0060 V1.2

#include "TonheCanDevice.hpp"
#include "CanPackets.hpp"   // for legacy PowerModuleStatus / InverterStatus types

#include <everest/logging.hpp>
#include <sstream>
#include <unistd.h>

// TX thread interval: 500 ms — matches the module's autonomous transmit rate.
// C_M_3 (timing/keepalive) is sent every 5000 ms per spec, i.e. every
// CM3_INTERVAL_CYCLES-th pass through the 500 ms loop.
static constexpr int TX_INTERVAL_US       = 500000;   // 500 ms
static constexpr int TX_INTERFRAME_US     =   5000;   //   5 ms between frames
static constexpr int CM3_INTERVAL_CYCLES  =      10;   // 10 * 500ms = 5000 ms

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TonheCanDevice::TonheCanDevice() = default;

TonheCanDevice::~TonheCanDevice() {
    exit_tx_thread = true;
    if (tx_thread_handle.joinable())
        tx_thread_handle.join();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TonheCanDevice::set_can_device(const std::string& dev) {
    can_device_name = dev;
}

void TonheCanDevice::set_config_values(const std::string& module_addresses,
                                       int   heartbeat_timeout_ms_cfg,
                                       float overtemperature_limit_C_cfg,
                                       float per_module_rated_power_W_cfg,
                                       float conversion_efficiency_cfg)
{
    heartbeat_timeout_ms     = heartbeat_timeout_ms_cfg;
    overtemperature_limit_C  = overtemperature_limit_C_cfg;
    per_module_rated_power_W = per_module_rated_power_W_cfg;
    conversion_efficiency    = conversion_efficiency_cfg;

    configured_node_ids.clear();
    std::istringstream ss(module_addresses);
    std::string token;
    while (std::getline(ss, token, ',')) {
        try {
            int id = std::stoi(token);
            if (id >= 1 && id <= 240)
                configured_node_ids.push_back(static_cast<uint8_t>(id));
            else
                EVLOG_warning << "TonheBEG: Ignoring invalid node ID " << id;
        } catch (...) {
            EVLOG_warning << "TonheBEG: Cannot parse node ID '" << token << "'";
        }
    }
    if (configured_node_ids.empty()) {
        EVLOG_info << "TonheBEG: No node IDs configured – using auto-discovery";
    }

    // Pre-populate telemetry map; capabilities come from config (not from bus)
    std::lock_guard<std::mutex> lock(telemetry_mutex);
    for (uint8_t id : configured_node_ids) {
        auto& t = module_telemetry[id];
        t.online                = false;
        t.last_heartbeat        = std::chrono::steady_clock::now();
        t.dc_max_output_voltage = 1000.f;
        t.dc_min_output_voltage =  200.f;
        t.dc_max_output_current = per_module_rated_power_W / 1000.f; // A at 1000 V
        t.dc_rated_output_power = per_module_rated_power_W;
        t.valid_caps            = true;
    }

    EVLOG_info << "TonheBEG: Configured " << configured_node_ids.size()
               << " module(s): [" << module_addresses << "]"
               << "  timeout=" << heartbeat_timeout_ms << "ms"
               << "  overtemp=" << overtemperature_limit_C << "°C"
               << "  poll_interval=500ms  keepalive_interval=5000ms  interframe_gap=5ms";
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

bool TonheCanDevice::open() {
    if (!CanDevice::open_device(can_device_name.c_str()))
        return false;

    // Configure modules for DC input operation before we start sending output setpoints.
    // The Tonhe modules only accept this mode change while powered off.
    tonhe::CM4Frame cm4;
    cm4.ac_input = false;
    EVLOG_info << "TonheBEG TX C_M_4 configure input mode=DC";
    if (!tx(tonhe::cm4_id(), cm4.to_bytes())) {
        EVLOG_warning << "TonheBEG: Failed to configure module input mode to DC";
    }

    exit_tx_thread   = false;
    tx_thread_handle = std::thread(&TonheCanDevice::tx_thread_func, this);

    EVLOG_info << "TonheBEG: CAN interface '" << can_device_name
               << "' opened. Controlling " << configured_node_ids.size() << " module(s).";
    return true;
}

bool TonheCanDevice::open_device(const char* dev) {
    can_device_name = dev;
    return open();
}

bool TonheCanDevice::close_device() {
    exit_tx_thread = true;
    if (tx_thread_handle.joinable())
        tx_thread_handle.join();
    return CanDevice::close_device();
}

// ---------------------------------------------------------------------------
// TX thread — 500 ms keepalive loop
// ---------------------------------------------------------------------------

void TonheCanDevice::tx_thread_func() {
    usleep(500000); // 500 ms startup delay

    uint32_t cycle = 0;
    while (!exit_tx_thread) {
        check_heartbeats();
        send_setpoints();

        // C_M_3 timing keepalive — every 10th cycle (5000 ms), per spec
        if (cycle % CM3_INTERVAL_CYCLES == 0) {
            // EVLOG_info << "TonheBEG TX C_M_3 keepalive";
            tx(tonhe::cm3_id(), tonhe::CM3Frame().to_bytes());
            usleep(TX_INTERFRAME_US);
        }

        // Every 5th cycle (~2.5 s) re-assert the on/off state so newly-recovered
        // modules are always in the correct state.
        if (cycle % 5 == 0) {
            std::lock_guard<std::mutex> sp(setpoint_mutex);
            uint32_t all_flags = 0;
            {
                std::lock_guard<std::mutex> t(telemetry_mutex);
                for (auto& [id, telem] : module_telemetry)
                    if (telem.online)
                        all_flags |= (1u << (id - 1));
            }
            if (all_flags) {
                tonhe::CM1Frame f;
                f.module_flag = all_flags;
                f.start       = output_enabled;
                f.group       = 1;
                // EVLOG_info << "TonheBEG TX C_M_1 reassert"
                //            << "  flags=0x" << std::hex << all_flags << std::dec
                //            << "  start=" << (output_enabled ? "on" : "off");
                tx(tonhe::cm1_id(), f.to_bytes());
                usleep(TX_INTERFRAME_US);
            }
        }
        ++cycle;

        usleep(TX_INTERVAL_US);
    }
}

// ---------------------------------------------------------------------------
// Heartbeat timeout check
// ---------------------------------------------------------------------------

void TonheCanDevice::check_heartbeats() {
    auto now     = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(heartbeat_timeout_ms);

    std::vector<uint8_t> gone_offline;
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        for (auto& [id, t] : module_telemetry) {
            if (t.online && (now - t.last_heartbeat) > timeout) {
                EVLOG_warning << "TonheBEG: Module " << static_cast<int>(id)
                              << " heartbeat timeout – marking offline.";
                t.online = false;
                gone_offline.push_back(id);
            }
        }
    }
    if (!gone_offline.empty()) {
        emit_capabilities_update();
        for (uint8_t id : gone_offline)
            signalModuleError(id, "CommunicationFault", true);
    }
}

// ---------------------------------------------------------------------------
// Send C_M_24 (unicast) and C_M_2 (broadcast) setpoints to active modules
// ---------------------------------------------------------------------------

void TonheCanDevice::send_setpoints() {
    std::lock_guard<std::mutex> sp(setpoint_mutex);

    std::vector<uint8_t> active;
    {
        std::lock_guard<std::mutex> t(telemetry_mutex);
        for (auto& [id, telem] : module_telemetry)
            if (telem.online)
                active.push_back(id);
    }
    if (active.empty()) return;

    float per_module_A = (active.size() > 0)
        ? last_total_current / static_cast<float>(active.size())
        : 0.f;

    // C_M_24: individual setpoint per active module
    for (uint8_t id : active) {
        tonhe::CM24Frame f;
        f.start      = output_enabled;
        f.voltage_V  = last_voltage;
        f.current_A  = per_module_A;
        // EVLOG_info << "TonheBEG[node" << static_cast<int>(id) << "] TX C_M_24"
        //            << "  cmd=" << (output_enabled ? "START" : "STOP")
        //            << "  V=" << last_voltage << "V"
        //            << "  I=" << per_module_A << "A";
        tx(tonhe::cm24_id(id), f.to_bytes());
        usleep(TX_INTERFRAME_US);
    }

    // C_M_2: broadcast setpoint (redundancy for C_M_24 / module watchdog)
    //
    // NOTE: C_M_2 applies ONE shared current value to every module selected
    // by the bitmask (it cannot carry a per-module value). It must therefore
    // carry the same per-module share as C_M_24, NOT the undivided aggregate
    // total — otherwise every active module would be told to individually
    // target the full total current right after C_M_24 correctly told it to
    // target only its share, defeating equal current sharing and creating an
    // overcurrent hazard.
    if (!active.empty()) {
        uint32_t flags = 0;
        for (uint8_t id : active)
            flags |= (1u << (id - 1));

        tonhe::CM2Frame f;
        f.module_flag = flags;
        f.voltage_V   = last_voltage;
        f.current_A   = per_module_A;
        f.group       = 1;
        // EVLOG_info << "TonheBEG TX C_M_2 broadcast"
        //            << "  flags=0x" << std::hex << flags << std::dec
        //            << "  V=" << last_voltage << "V"
        //            << "  I_per_module=" << per_module_A << "A";
        tx(tonhe::cm2_id(), f.to_bytes());
        usleep(TX_INTERFRAME_US);
    }
}

// ---------------------------------------------------------------------------
// Public commands
// ---------------------------------------------------------------------------

bool TonheCanDevice::switch_on_off(bool on) {
    {
        std::lock_guard<std::mutex> lock(setpoint_mutex);
        output_enabled = on;
    }
    // Immediate broadcast
    std::vector<uint8_t> active;
    {
        std::lock_guard<std::mutex> t(telemetry_mutex);
        for (auto& [id, telem] : module_telemetry)
            if (telem.online)
                active.push_back(id);
    }
    uint32_t flags = 0;
    for (uint8_t id : active)
        flags |= (1u << (id - 1));

    if (flags == 0) {
        flags = 0xFFFFFFu;
    }

    tonhe::CM1Frame f;
    f.module_flag = flags;
    f.start       = on;
    f.group       = 1;
    // EVLOG_info << "TonheBEG TX C_M_1 switch_on_off"
    //            << "  flags=0x" << std::hex << flags << std::dec
    //            << "  start=" << (on ? "on" : "off");
    return tx(tonhe::cm1_id(), f.to_bytes());
}

bool TonheCanDevice::set_voltage_current(float voltage_V, float total_current_A) {
    {
        std::lock_guard<std::mutex> lock(setpoint_mutex);
        last_voltage       = voltage_V;
        last_total_current = total_current_A;
    }
    send_setpoints(); // apply immediately
    return true;
}

// ---------------------------------------------------------------------------
// Capability aggregation
// ---------------------------------------------------------------------------

void TonheCanDevice::emit_capabilities_update() {
    TelemetryMap snap;
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        snap = module_telemetry;
    }
    signalCapabilitiesUpdate(snap);
}

// ---------------------------------------------------------------------------
// Online state management
// ---------------------------------------------------------------------------

void TonheCanDevice::update_module_online_state(uint8_t node_id, bool online) {
    bool changed = false;
    bool was_online_before = false;
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        auto it = module_telemetry.find(node_id);
        if (it != module_telemetry.end()) {
            was_online_before = it->second.online;
            if (it->second.online != online) {
                it->second.online = online;
                changed = true;
            }
            if (online)
                it->second.last_heartbeat = std::chrono::steady_clock::now();
        }
    }
    if (changed) {
        EVLOG_info << "TonheBEG: Module " << static_cast<int>(node_id)
                   << (online ? " came online." : " went offline.");

        if (online && !was_online_before) {
            // Re-send current setpoints to newly-recovered module
            try {
                std::lock_guard<std::mutex> sp(setpoint_mutex);
                std::vector<uint8_t> active;
                {
                    std::lock_guard<std::mutex> t(telemetry_mutex);
                    for (auto& [id, telem] : module_telemetry)
                        if (telem.online)
                            active.push_back(id);
                }
                float per_A = active.empty() ? last_total_current
                    : last_total_current / static_cast<float>(active.size());

                tonhe::CM24Frame uf;
                uf.start     = output_enabled;
                uf.voltage_V = last_voltage;
                uf.current_A = per_A;
                tx(tonhe::cm24_id(node_id), uf.to_bytes());

                // Also send stop/start broadcast so module picks up correct state
                uint32_t flags = (1u << (node_id - 1));
                tonhe::CM1Frame bf;
                bf.module_flag = flags;
                bf.start       = output_enabled;
                bf.group       = 1;
                // EVLOG_info << "TonheBEG[node" << static_cast<int>(node_id) << "] TX C_M_1 reconnect"
                //            << "  flags=0x" << std::hex << flags << std::dec
                //            << "  start=" << (output_enabled ? "on" : "off");
                tx(tonhe::cm1_id(), bf.to_bytes());
            } catch (const std::exception& e) {
                EVLOG_warning << "TonheBEG: Could not re-init node "
                              << static_cast<int>(node_id) << ": " << e.what();
            }
        }
        emit_capabilities_update();
    }
}

// ---------------------------------------------------------------------------
// RX handler
// ---------------------------------------------------------------------------

void TonheCanDevice::rx_handler(uint32_t can_id, const std::vector<uint8_t>& payload) {
    if (payload.size() < 8) return;

    // All Tonhe frames are extended
    if (!(can_id & CAN_EFF_FLAG)) return;

    uint32_t raw = can_id & ~CAN_EFF_FLAG;

    // Extract source and destination from J1939 CAN ID
    // CAN ID layout: (prio_pgnh << 24) | (pgnl << 16) | (dst << 8) | src
    uint8_t src_addr  = raw & 0xFF;
    uint8_t dst_addr  = (raw >> 8) & 0xFF;
    uint8_t pgn_low   = (raw >> 16) & 0xFF;
    uint8_t pgn_high  = (raw >> 24) & 0xFF;

    // Only handle frames addressed to us (master) or broadcast
    if (dst_addr != tonhe::MASTER_ADDR && dst_addr != tonhe::BROADCAST_ADDR)
        return;

    // Auto-discover: if this src_addr is not yet tracked, add it with default capabilities
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        if (module_telemetry.find(src_addr) == module_telemetry.end()) {
            EVLOG_info << "TonheBEG: Auto-discovered module at address "
                       << static_cast<int>(src_addr) << " — adding to telemetry map.";
            auto& t = module_telemetry[src_addr];
            t.online                = false;
            t.last_heartbeat        = std::chrono::steady_clock::now();
            t.dc_max_output_voltage = 1000.f;
            t.dc_min_output_voltage =  200.f;
            t.dc_max_output_current = per_module_rated_power_W / 1000.f;
            t.dc_rated_output_power = per_module_rated_power_W;
            t.valid_caps            = true;
        }
    }

    // Record heartbeat (any frame from a module keeps it alive)
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        auto it = module_telemetry.find(src_addr);
        if (it != module_telemetry.end()) {
            bool was_online = it->second.online;
            it->second.last_heartbeat = std::chrono::steady_clock::now();
            if (!was_online) {
                it->second.online = true;
                // Signal online change outside lock below
            }
        }
    }
    update_module_online_state(src_addr, true);

    const uint8_t* d = payload.data();

    // Dispatch by PGN (pgn_high | pgn_low)
    // M_C_1: pgn_high=0x18, pgn_low=0x01  → 0x1801
    // M_C_2: pgn_high=0x08, pgn_low=0x02  → 0x0802
    // M_C_3: pgn_high=0x18, pgn_low=0x0B  → 0x180B
    // M_C_4: pgn_high=0x1C, pgn_low=0x91  → 0x1C91
    uint16_t pgn = (static_cast<uint16_t>(pgn_high) << 8) | pgn_low;

    switch (pgn) {
    case 0x1801: handle_mc1(src_addr, d); break;
    case 0x0802: handle_mc2(src_addr, d); break;
    case 0x180B: handle_mc3(src_addr, d); break;
    case 0x1C91: handle_mc4(src_addr, d); break;
    default:
        EVLOG_debug << "TonheBEG: Unknown PGN 0x" << std::hex << pgn
                    << " from node " << static_cast<int>(src_addr);
        break;
    }
}

// ---------------------------------------------------------------------------
// M_C_1: Charging module state + output telemetry + fault bits
// ---------------------------------------------------------------------------

void TonheCanDevice::handle_mc1(uint8_t mod_addr, const uint8_t* d) {
    auto frame = tonhe::MC1Frame::from_bytes(d);

    // EVLOG_info << "TonheBEG[node" << static_cast<int>(mod_addr) << "] M_C_1"
    //            << "  state=0x" << std::hex << static_cast<int>(static_cast<uint8_t>(frame.state))
    //            << std::dec
    //            << "  V=" << frame.voltage_V << "V"
    //            << "  I=" << frame.current_A << "A"
    //            << "  faults=0x" << std::hex << (frame.fault_lo | (frame.fault_hi << 8)) << std::dec;

    float agg_voltage  = 0.f;
    float agg_current  = 0.f;
    bool  mode_changed = false;
    tonhe::MC1Frame::State old_state;

    {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        auto it = module_telemetry.find(mod_addr);
        if (it == module_telemetry.end()) return;

        auto& t = it->second;
        old_state = t.module_state;

        // Check fault bit transitions and emit error signals
        auto check_fault = [&](bool new_bit, bool old_bit, const std::string& name) {
            if (new_bit && !old_bit)
                signalModuleError(mod_addr, name, true);
            else if (!new_bit && old_bit)
                signalModuleError(mod_addr, name, false);
        };
        uint8_t old_faults = t.fault_bits;
        uint8_t new_faults = frame.fault_lo;

        check_fault((new_faults >> 3) & 1, (old_faults >> 3) & 1, "OverVoltageDC");
        check_fault((new_faults >> 4) & 1, (old_faults >> 4) & 1, "OverCurrentDC");
        check_fault((new_faults >> 5) & 1, (old_faults >> 5) & 1, "OverTemperature");
        check_fault((new_faults >> 6) & 1, (old_faults >> 6) & 1, "FanFault");
        check_fault((new_faults >> 7) & 1, (old_faults >> 7) & 1, "HardwareFault");
        check_fault((new_faults >> 0) & 1, (old_faults >> 0) & 1, "UnderVoltageAC");
        check_fault((new_faults >> 1) & 1, (old_faults >> 1) & 1, "InputPhaseLoss");

        t.output_voltage_V = frame.voltage_V;
        t.output_current_A = frame.current_A;
        t.fault_bits       = frame.fault_lo;

        if (t.module_state != frame.state) {
            t.module_state = frame.state;
            mode_changed   = true;
        }

        // Update legacy telemetry
        telemetry.output_voltage_V = frame.voltage_V;
        telemetry.output_current_A = frame.current_A;

        // Aggregate for voltage/current signal
        for (const auto& [id, tt] : module_telemetry) {
            if (tt.online) {
                agg_voltage  = tt.output_voltage_V; // parallel bus → same voltage
                agg_current += tt.output_current_A;
            }
        }
    }

    // Emit aggregated telemetry (throttle to ~2 Hz at 500 ms module rate)
    if (throttle_cnt++ % 2 == 0)
        signalVoltageCurrent(agg_voltage, agg_current);

    // Emit mode change
    if (mode_changed) {
        can_packet_acdc::PowerModuleStatus status;
        can_packet_acdc::InverterStatus    inv_status;

        switch (frame.state) {
        case tonhe::MC1Frame::State::On:
            status.dc_side_off  = false;
            status.fault_alarm  = false;
            break;
        case tonhe::MC1Frame::State::NormalOff:
            status.dc_side_off  = true;
            status.fault_alarm  = false;
            break;
        case tonhe::MC1Frame::State::FaultOff:
            status.dc_side_off  = true;
            status.fault_alarm  = true;
            break;
        }
        // Map specific fault bits to status fields
        if (frame.flt_temperature_high())   status.over_temperature_alarm   = true;
        if (frame.flt_output_overvoltage()) status.output_over_voltage_alarm = true;
        if (frame.flt_input_phase_loss())   status.input_phase_lost_alarm    = true;
        if (frame.flt_input_undervoltage()) status.input_low_voltage_alarm   = true;
        if (frame.flt_fan_fault())          status.fan_fault_alarm           = true;

        signalModuleStatus(status, inv_status);
    }

    // Overtemperature check
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        auto it = module_telemetry.find(mod_addr);
        if (it != module_telemetry.end() && overtemperature_limit_C > 0.f
            && it->second.ambient_temp_C > overtemperature_limit_C) {
            EVLOG_warning << "TonheBEG: Module " << static_cast<int>(mod_addr)
                          << " temperature " << it->second.ambient_temp_C
                          << "°C exceeds limit " << overtemperature_limit_C << "°C";
            signalModuleError(mod_addr, "OverTemperature", true);
        }
    }
}

// ---------------------------------------------------------------------------
// M_C_2: Start/stop acknowledgement
// ---------------------------------------------------------------------------

void TonheCanDevice::handle_mc2(uint8_t mod_addr, const uint8_t* d) {
    auto frame = tonhe::MC2Frame::from_bytes(d);
    EVLOG_debug << "TonheBEG: Module " << static_cast<int>(mod_addr)
                << " ack: received=" << frame.received;
}

// ---------------------------------------------------------------------------
// M_C_3: AC input voltages + ambient temperature
// ---------------------------------------------------------------------------

void TonheCanDevice::handle_mc3(uint8_t mod_addr, const uint8_t* d) {
    auto frame = tonhe::MC3Frame::from_bytes(d);

    // EVLOG_info << "TonheBEG[node" << static_cast<int>(mod_addr) << "] M_C_3"
    //            << "  VA=" << frame.a_phase_V << "V"
    //            << "  VB=" << frame.b_phase_V << "V"
    //            << "  VC=" << frame.c_phase_V << "V"
    //            << "  temp=" << frame.ambient_temp_C << "°C";
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        auto it = module_telemetry.find(mod_addr);
        if (it == module_telemetry.end()) return;
        it->second.a_phase_V      = frame.a_phase_V;
        it->second.b_phase_V      = frame.b_phase_V;
        it->second.c_phase_V      = frame.c_phase_V;
        it->second.ambient_temp_C = frame.ambient_temp_C;
        telemetry.ambient_temperature = frame.ambient_temp_C;
        telemetry.ac_phase_a_voltage  = frame.a_phase_V;
        telemetry.ac_phase_b_voltage  = frame.b_phase_V;
        telemetry.ac_phase_c_voltage  = frame.c_phase_V;
    }
    // Overtemperature check on ambient temperature from M_C_3
    if (overtemperature_limit_C > 0.f && frame.ambient_temp_C > overtemperature_limit_C) {
        EVLOG_warning << "TonheBEG: Module " << static_cast<int>(mod_addr)
                      << " ambient " << frame.ambient_temp_C << "°C > limit";
        signalModuleError(mod_addr, "OverTemperature", true);
    }
}

// ---------------------------------------------------------------------------
// M_C_4: Extended module state / fault-warning
// ---------------------------------------------------------------------------

void TonheCanDevice::handle_mc4(uint8_t mod_addr, const uint8_t* d) {
    auto frame = tonhe::MC4Frame::from_bytes(d);

    // EVLOG_info << "TonheBEG[node" << static_cast<int>(mod_addr) << "] M_C_4"
    //            << "  module_state=0x" << std::hex << frame.module_state
    //            << "  fault_warning=0x" << frame.fault_warning << std::dec;
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        auto it = module_telemetry.find(mod_addr);
        if (it == module_telemetry.end()) return;
        uint16_t old_ext = it->second.ext_fault_bits;
        it->second.ext_fault_bits = frame.fault_warning;

        auto check = [&](bool new_b, bool old_b, const std::string& name) {
            if (new_b && !old_b)       signalModuleError(mod_addr, name, true);
            else if (!new_b && old_b)  signalModuleError(mod_addr, name, false);
        };
        check(frame.fw_internal_overtemp(),  (old_ext >> 6) & 1, "OverTemperature");
        check(frame.fw_air_inlet_overtemp(), (old_ext >> 7) & 1, "OverTemperature");
        check(frame.fw_can_timeout(),        (old_ext >> 2) & 1, "CommunicationFault");
    }
}

// ---------------------------------------------------------------------------
// State accessors
// ---------------------------------------------------------------------------

TonheCanDevice::TelemetryMap TonheCanDevice::get_telemetry_snapshot() const {
    std::lock_guard<std::mutex> lock(telemetry_mutex);
    return module_telemetry;
}

size_t TonheCanDevice::active_module_count() const {
    std::lock_guard<std::mutex> lock(telemetry_mutex);
    size_t n = 0;
    for (const auto& [id, t] : module_telemetry)
        if (t.online) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// TX helper
// ---------------------------------------------------------------------------

bool TonheCanDevice::tx(uint32_t can_id, const std::vector<uint8_t>& payload) {
    uint32_t frame_id = can_id | CAN_EFF_FLAG;
    try {
        return _tx(frame_id, payload);
    } catch (const std::exception& e) {
        EVLOG_warning << "TonheBEG: tx failed: " << e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// Legacy telemetry ostream
// ---------------------------------------------------------------------------

std::ostream& operator<<(std::ostream& out, const TonheCanDevice::LegacyTelemetry& self) {
    out << "\n--- TonheBEG Telemetry ---\n";
    out << "Output: V=" << self.output_voltage_V << "V  I=" << self.output_current_A << "A\n";
    out << "AC input: A=" << self.ac_phase_a_voltage
        << " B=" << self.ac_phase_b_voltage
        << " C=" << self.ac_phase_c_voltage << "V\n";
    out << "Ambient temp: " << self.ambient_temperature << "°C\n";
    out << "----------------------------\n";
    return out;
}