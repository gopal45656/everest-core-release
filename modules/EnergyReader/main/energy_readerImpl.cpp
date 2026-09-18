// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "energy_readerImpl.hpp"
#include "EnergyReader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fmt/core.h>
#include <stdexcept>
#include <utils/date.hpp>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <cstring>
#include <fstream>   // ← JSON file writing
#include <filesystem>

namespace module {
namespace main {

// ─────────────────────────────────────────────────────────────────────────────
// JSON status file path
// ─────────────────────────────────────────────────────────────────────────────
static constexpr const char* JSON_STATUS_FILE =
    "/usr/share/everest/modules/EnergyReader/reader_data.json";

// ─────────────────────────────────────────────────────────────────────────────
// DWIN packet constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint8_t PKT_HEADER_0 = 0x5A;
static constexpr uint8_t PKT_HEADER_1 = 0xA5;
static constexpr uint8_t PKT_LEN      = 0x05;
static constexpr uint8_t PKT_CMD      = 0x82;

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::init() {
    EVLOG_info << "EnergyReader: Initializing DWIN serial display on "
               << config.serial_port;

    {
        std::lock_guard<std::mutex> lock(display_mutex);
        display_values.gun_plugged = false;
        display_values.gun_unplugged = true;
        display_values.charging_active_icon = false;
        display_values.charging_complete_icon = false;
        display_values.fault = false;
        display_values.charging_active = false;
        display_values.charge_duration_seconds = 0;
        display_values.accumulated_duration_seconds = 0;
    }

    {
        std::lock_guard<std::mutex> lock(display_mutex_gun2);
        display_values_gun2.gun_plugged = false;
        display_values_gun2.gun_unplugged = true;
        display_values_gun2.charging_active_icon = false;
        display_values_gun2.charging_complete_icon = false;
        display_values_gun2.fault = false;
        display_values_gun2.charging_active = false;
        display_values_gun2.charge_duration_seconds = 0;
        display_values_gun2.accumulated_duration_seconds = 0;
    }

    // ── Subscribe to powermeter (Gun 1) ───────────────────────────────────────
    if (!mod->r_powermeter.empty() && mod->r_powermeter[0]) {
        mod->r_powermeter[0]->subscribe_powermeter(
            [this](const types::powermeter::Powermeter& pm) {
                this->on_powermeter_update(pm);
            });
    }

    // ── Subscribe to evse_manager (Gun 1) ─────────────────────────────────────
    if (!mod->r_evse_manager.empty() && mod->r_evse_manager[0]) {
        mod->r_evse_manager[0]->subscribe_session_event(
            [this](const types::evse_manager::SessionEvent& event) {
                this->on_session_event(event);
            });

        mod->r_evse_manager[0]->subscribe_ev_info(
            [this](const types::evse_manager::EVInfo& ev_info) {
                this->on_ev_info_update(ev_info);
            });
    }

    // ── Subscribe to powermeter (Gun 2) ───────────────────────────────────────
    if (mod->r_powermeter.size() > 1 && mod->r_powermeter[1]) {
        mod->r_powermeter[1]->subscribe_powermeter(
            [this](const types::powermeter::Powermeter& pm) {
                this->on_powermeter_update_gun2(pm);
            });
    }

    // ── Subscribe to evse_manager (Gun 2) ─────────────────────────────────────
    if (mod->r_evse_manager.size() > 1 && mod->r_evse_manager[1]) {
        mod->r_evse_manager[1]->subscribe_session_event(
            [this](const types::evse_manager::SessionEvent& event) {
                this->on_session_event_gun2(event);
            });

        mod->r_evse_manager[1]->subscribe_ev_info(
            [this](const types::evse_manager::EVInfo& ev_info) {
                this->on_ev_info_update_gun2(ev_info);
            });
    }

    open_serial_connection();
}

void energy_readerImpl::ready() {
    EVLOG_info << "EnergyReader: Starting write loop (interval "
               << config.publish_interval_ms << " ms)";
    running = true;
    write_thread = std::thread([this]() { write_loop(); });
}

// ─────────────────────────────────────────────────────────────────────────────
// Command handler
// ─────────────────────────────────────────────────────────────────────────────

Object energy_readerImpl::handle_get_last_values() {
    json snap;
    {
        std::lock_guard<std::mutex> lock(display_mutex);
        snap["voltage_dc"]           = display_values.voltage_dc;
        snap["current_dc"]           = display_values.current_dc;
        snap["power_kw"]             = display_values.power_kw;
        snap["energy_import_kwh"]    = display_values.energy_import_kwh;
        snap["energy_export_kwh"]    = display_values.energy_export_kwh;
        snap["max_voltage"]          = display_values.max_voltage;
        snap["min_voltage"]          = display_values.min_voltage;
        snap["max_current"]          = display_values.max_current;
        snap["min_current"]          = display_values.min_current;
        snap["timestamp"]            = display_values.timestamp;

        snap["demand_voltage"]       = display_values.demand_voltage;
        snap["demand_current"]       = display_values.demand_current;
        snap["charging_voltage"]     = display_values.charging_voltage;
        snap["charging_current"]     = display_values.charging_current;

        snap["gun_plugged"]          = display_values.gun_plugged;
        snap["gun_unplugged"]        = display_values.gun_unplugged;
        snap["charging_active"]      = display_values.charging_active_icon;
        snap["charging_complete"]    = display_values.charging_complete_icon;
        snap["fault"]                = display_values.fault;
        snap["charge_duration_min"]  = display_values.charge_duration_seconds / 60;
    }

    {
        std::lock_guard<std::mutex> lock(display_mutex_gun2);
        json snap_g2;
        snap_g2["voltage_dc"]           = display_values_gun2.voltage_dc;
        snap_g2["current_dc"]           = display_values_gun2.current_dc;
        snap_g2["power_kw"]             = display_values_gun2.power_kw;
        snap_g2["energy_import_kwh"]    = display_values_gun2.energy_import_kwh;
        snap_g2["energy_export_kwh"]    = display_values_gun2.energy_export_kwh;
        snap_g2["max_voltage"]          = display_values_gun2.max_voltage;
        snap_g2["min_voltage"]          = display_values_gun2.min_voltage;
        snap_g2["max_current"]          = display_values_gun2.max_current;
        snap_g2["min_current"]          = display_values_gun2.min_current;
        snap_g2["timestamp"]            = display_values_gun2.timestamp;

        snap_g2["demand_voltage"]       = display_values_gun2.demand_voltage;
        snap_g2["demand_current"]       = display_values_gun2.demand_current;
        snap_g2["charging_voltage"]     = display_values_gun2.charging_voltage;
        snap_g2["charging_current"]     = display_values_gun2.charging_current;

        snap_g2["gun_plugged"]          = display_values_gun2.gun_plugged;
        snap_g2["gun_unplugged"]        = display_values_gun2.gun_unplugged;
        snap_g2["charging_active"]      = display_values_gun2.charging_active_icon;
        snap_g2["charging_complete"]    = display_values_gun2.charging_complete_icon;
        snap_g2["fault"]                = display_values_gun2.fault;
        snap_g2["charge_duration_min"]  = display_values_gun2.charge_duration_seconds / 60;

        snap["gun2"] = snap_g2;
    }
    return snap;
}

// ─────────────────────────────────────────────────────────────────────────────
// Serial connection management
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::open_serial_connection() {
    const std::string& port = config.serial_port;

    serial_fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (serial_fd < 0) {
        EVLOG_error << "EnergyReader: Failed to open serial port: " << port;
        throw std::runtime_error("EnergyReader: Unable to open serial port: " + port);
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(serial_fd, &tty) != 0) {
        close(serial_fd);
        serial_fd = -1;
        throw std::runtime_error("EnergyReader: tcgetattr failed on " + port);
    }

    speed_t baud = B115200;
    switch (config.baud_rate) {
        case 9600:   baud = B9600;   break;
        case 19200:  baud = B19200;  break;
        case 38400:  baud = B38400;  break;
        case 57600:  baud = B57600;  break;
        case 115200: baud = B115200; break;
        default:
            EVLOG_warning << "EnergyReader: Unsupported baud rate "
                          << config.baud_rate << ", defaulting to 115200";
            baud = B115200;
    }
    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);

    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag  = 0;
    tty.c_oflag  = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |=  (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
        close(serial_fd);
        serial_fd = -1;
        throw std::runtime_error("EnergyReader: tcsetattr failed on " + port);
    }

    EVLOG_info << fmt::format("EnergyReader: Serial port opened — port={} baud={}",
                               port, config.baud_rate);
}

void energy_readerImpl::close_serial_connection() {
    completion_reset_active.store(false);
    completion_reset_generation.fetch_add(1);
    if (completion_reset_thread.joinable()) {
        completion_reset_thread.detach();
    }

    completion_reset_active_gun2.store(false);
    completion_reset_generation_gun2.fetch_add(1);
    if (completion_reset_thread_gun2.joinable()) {
        completion_reset_thread_gun2.detach();
    }

    if (serial_fd >= 0) {
        close(serial_fd);
        serial_fd = -1;
        EVLOG_info << "EnergyReader: Serial port closed.";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Powermeter subscription callback  (actual measured V / I from powermeter)
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::on_powermeter_update(const types::powermeter::Powermeter& pm) {
    {
        std::lock_guard<std::mutex> lock(pm_mutex);
        pm_last_reading = pm;
    }
    update_display_values(pm);
}

// ─────────────────────────────────────────────────────────────────────────────
// Display smoothing
//   Real meters report small sample-to-sample noise on V/I/P even when the
//   actual load is steady. Writing every raw sample straight to the DWIN
//   display makes the digits flicker/jitter even though each reading is
//   individually "correct". An exponential moving average (EMA) smooths
//   that out: EMA_SMOOTHING_ALPHA closer to 1.0 = more responsive/less
//   smoothing, closer to 0.0 = smoother/slower to react. 0.25 is a
//   reasonable starting point — tune to taste, or promote to a config
//   option (e.g. cfg.display_smoothing_alpha) if you want it adjustable
//   without a rebuild.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr float EMA_SMOOTHING_ALPHA = 0.25f;

static inline float ema_blend(float previous, float sample, bool have_previous) {
    return have_previous ? (EMA_SMOOTHING_ALPHA * sample + (1.0f - EMA_SMOOTHING_ALPHA) * previous)
                          : sample;
}

void energy_readerImpl::update_display_values(const types::powermeter::Powermeter& pm) {
    std::lock_guard<std::mutex> lock(display_mutex);

    // ── Raw PM diagnostic logging ────────────────────────────────────────────
    // Logged before any values are copied into display_values, so if these
    // already look wrong, the problem is upstream in the module publishing
    // Powermeter data, not in EnergyReader or the DWIN write path.
    EVLOG_debug << "EnergyReader: PM Voltage : "
                << (pm.voltage_V.has_value() && pm.voltage_V->DC.has_value()
                        ? pm.voltage_V->DC.value() : -1.0f);
    EVLOG_debug << "EnergyReader: PM Current : "
                << (pm.current_A.has_value() && pm.current_A->DC.has_value()
                        ? pm.current_A->DC.value() : -1.0f);
    if (pm.power_W.has_value()) {
        EVLOG_debug << "EnergyReader: PM Power Total = " << pm.power_W.value().total;
    }

    if (pm.voltage_V.has_value()) {
        float v = 0.0f;
        bool has_voltage = false;
        if (pm.voltage_V.value().DC.has_value()) {
            v = pm.voltage_V.value().DC.value();
            has_voltage = true;
        } else if (pm.voltage_V.value().L1.has_value()) {
            v = pm.voltage_V.value().L1.value();
            has_voltage = true;
        }

        if (has_voltage && !std::isfinite(v)) {
            EVLOG_warning << "EnergyReader: Discarding non-finite voltage_V reading: " << v;
            has_voltage = false;
        }

        if (has_voltage) {
            display_values.voltage_dc =
                ema_blend(display_values.voltage_dc, v, display_values.initialized);
            const float smoothed_v = display_values.voltage_dc;
            if (!display_values.initialized) {
                display_values.max_voltage = smoothed_v;
                display_values.min_voltage = smoothed_v;
            } else {
                display_values.max_voltage = std::max(display_values.max_voltage, smoothed_v);
                display_values.min_voltage = std::min(display_values.min_voltage, smoothed_v);
            }
        }
    }

    // ── Current from powermeter ────────────────────────────────────────────
    // If the meter doesn't report current this cycle (or reports NaN/Inf),
    // freeze display_values.current_dc at its last valid reading rather than
    // resetting it — matches the behavior of voltage/power/energy below.
    {
        float i = 0.0f;
        bool has_current = false;
        if (pm.current_A.has_value()) {
            if (pm.current_A.value().DC.has_value()) {
                i = pm.current_A.value().DC.value();
                has_current = true;
            } else if (pm.current_A.value().L1.has_value()) {
                i = pm.current_A.value().L1.value();
                has_current = true;
            }
        }

        if (has_current && !std::isfinite(i)) {
            EVLOG_warning << "EnergyReader: Discarding non-finite current_A reading: " << i;
            has_current = false;
        }

        if (has_current) {
            display_values.current_dc =
                ema_blend(display_values.current_dc, i, display_values.initialized);
            const float smoothed_i = display_values.current_dc;
            if (!display_values.initialized) {
                display_values.max_current = smoothed_i;
                display_values.min_current = smoothed_i;
            } else {
                display_values.max_current = std::max(display_values.max_current, smoothed_i);
                display_values.min_current = std::min(display_values.min_current, smoothed_i);
            }
        }
    }

    if (pm.power_W.has_value()) {
        const float total = pm.power_W.value().total;
        if (std::isfinite(total)) {
            const float sample_kw = total / 1000.0f;
            display_values.power_kw =
                ema_blend(display_values.power_kw, sample_kw, display_values.initialized);
        } else {
            EVLOG_warning << "EnergyReader: Discarding non-finite power_W.total reading: " << total;
        }
    }

    // NOTE: In the current EVerest Powermeter type, energy_Wh_import is a
    // required (non-optional) field — unlike power_W and energy_Wh_export,
    // which are optional. That is why there is no has_value() check here.
    // If the generated type for energy_Wh_import is ever changed to
    // optional<...>, this must be updated to check has_value() first.
    // Regardless of optionality, we still guard against NaN/Infinity, since
    // a malformed upstream publisher could still send a garbage total.
    //
    // total is the meter's LIFETIME import energy. Subtract the session
    // baseline (captured at SessionStarted) so the display shows energy
    // consumed THIS session, starting at 0. Clamp to 0 in case of tiny
    // negative rounding right at session start.
    {
        const float total = pm.energy_Wh_import.total;
        if (std::isfinite(total)) {
            const float session_kwh = (total / 1000.0f) - session_energy_import_baseline_kWh;
            display_values.energy_import_kwh = std::max(0.0f, session_kwh);
        } else {
            EVLOG_warning << "EnergyReader: Discarding non-finite energy_Wh_import.total reading: " << total;
        }
    }

    if (pm.energy_Wh_export.has_value()) {
        const float total = pm.energy_Wh_export.value().total;
        if (std::isfinite(total)) {
            const float session_kwh = (total / 1000.0f) - session_energy_export_baseline_kWh;
            display_values.energy_export_kwh = std::max(0.0f, session_kwh);
        } else {
            EVLOG_warning << "EnergyReader: Discarding non-finite energy_Wh_export.total reading: " << total;
        }
    }

    display_values.timestamp   = pm.timestamp;
    display_values.initialized = true;

    // ── AC mode: map meter V/I to charging_voltage/current for DWIN display ──
    // In DC mode these are overwritten by on_ev_info_update() from PSU feedback.
    // In AC mode present_voltage/current are never published by EVSEManager,
    // so we use the live meter reading instead.
    display_values.charging_voltage = display_values.voltage_dc;
    display_values.charging_current = display_values.current_dc;
}

// ─────────────────────────────────────────────────────────────────────────────
// ev_info callback  →  Demand Voltage, Demand Current, Charging Voltage,
//                       Charging Current, SOC
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::on_ev_info_update(const types::evse_manager::EVInfo& ev_info) {
    std::lock_guard<std::mutex> lock(display_mutex);

    // ── Demand values (what the EV is requesting) ─────────────────────────────
    if (ev_info.target_voltage.has_value()) {
        display_values.demand_voltage = ev_info.target_voltage.value();
        EVLOG_debug << fmt::format("EnergyReader: Demand voltage = {:.2f} V",
                                   display_values.demand_voltage);
    }

    if (ev_info.target_current.has_value()) {
        display_values.demand_current = ev_info.target_current.value();
        EVLOG_debug << fmt::format("EnergyReader: Demand current = {:.2f} A",
                                   display_values.demand_current);
    }

    // ── Charging values (what the PSU is actually outputting) ─────────────────
    if (ev_info.present_voltage.has_value()) {
        display_values.charging_voltage = ev_info.present_voltage.value();
        EVLOG_debug << fmt::format("EnergyReader: Charging voltage = {:.2f} V",
                                   display_values.charging_voltage);
    }

    if (ev_info.present_current.has_value()) {
        display_values.charging_current = ev_info.present_current.value();
        EVLOG_debug << fmt::format("EnergyReader: Charging current = {:.2f} A",
                                   display_values.charging_current);
    }

    // ── SOC (State of Charge from EV) ─────────────────────────────────────────
    if (ev_info.soc.has_value()) {
        display_values.current_soc_percent = ev_info.soc.value();
        // Capture initial SOC once per session at first valid reading
        if (!display_values.initial_soc_captured && ev_info.soc.value() > 0.0f) {
            display_values.initial_soc_percent  = ev_info.soc.value();
            display_values.initial_soc_captured = true;
            EVLOG_info << fmt::format("EnergyReader: Initial SOC captured = {:.1f}%",
                                      display_values.initial_soc_percent);
        }
        EVLOG_debug << fmt::format("EnergyReader: Current SOC = {:.1f}%",
                                   display_values.current_soc_percent);
    }

    // EVLOG_info << fmt::format(
    //     "EnergyReader: EV info — Demand={:.2f}V/{:.2f}A  Charging={:.2f}V/{:.2f}A",
    //     display_values.demand_voltage,  display_values.demand_current,
    //     display_values.charging_voltage, display_values.charging_current);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper — freeze duration into accumulator on pause
//   Call this whenever charging stops mid-session (pause, replug, etc.)
//   Must be called with display_mutex already held.
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::freeze_duration() {
    if (display_values.charging_active) {
        auto elapsed = std::chrono::steady_clock::now()
                       - display_values.charge_start_time;
        display_values.accumulated_duration_seconds +=
            static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        // EVLOG_info << fmt::format("EnergyReader: Duration frozen — accumulated={}s",
        //                            display_values.accumulated_duration_seconds);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Post-completion reset timer
//   5s  → clear charging_complete_icon, show gun unplugged/idle
//   10s → zero all session data
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::completion_reset_run(uint64_t generation) {
    // EVLOG_info << "EnergyReader: Post-completion reset timer started";

    // ── Wait 5 seconds → clear charging_complete_icon ────────────────────────
    for (int i = 0; i < 50; ++i) {          // 50 × 100 ms = 5 s
        if (!completion_reset_active.load() || completion_reset_generation.load() != generation) {
            // EVLOG_info << "EnergyReader: Post-completion reset cancelled (pre-5s).";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!completion_reset_active.load() || completion_reset_generation.load() != generation) return;

    {
        std::lock_guard<std::mutex> lock(display_mutex);
        display_values.charging_complete_icon = false;
        display_values.gun_plugged            = false;
        display_values.gun_unplugged          = true;   // → gun unplugged after completion
        EVLOG_info << "[ICON] charging_complete_icon cleared after 5s -> gun_unplugged=1 (waiting for next vehicle)";
    }

    // ── Wait another 5 seconds → zero all session data (total 10s) ───────────
    for (int i = 0; i < 50; ++i) {          // 50 × 100 ms = 5 s
        if (!completion_reset_active.load() || completion_reset_generation.load() != generation) {
            // EVLOG_info << "EnergyReader: Post-completion data reset cancelled (pre-10s).";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!completion_reset_active.load() || completion_reset_generation.load() != generation) return;

    {
        std::lock_guard<std::mutex> lock(display_mutex);

        display_values.demand_voltage               = 0.0f;
        display_values.demand_current               = 0.0f;
        display_values.charging_voltage             = 0.0f;
        display_values.charging_current             = 0.0f;
        display_values.current_soc_percent          = 0.0f;
        display_values.initial_soc_percent          = 0.0f;
        display_values.initial_soc_captured         = false;
        display_values.charge_duration_seconds      = 0;
        display_values.accumulated_duration_seconds = 0;
        display_values.voltage_dc                   = 0.0f;
        display_values.current_dc                   = 0.0f;
        display_values.power_kw                     = 0.0f;
        display_values.energy_import_kwh            = 0.0f;
        display_values.energy_export_kwh            = 0.0f;
        // display_values.max_voltage                  = 0.0f;
        // display_values.min_voltage                  = 0.0f;
        display_values.max_current                  = 0.0f;
        display_values.min_current                  = 0.0f;
        display_values.gun_unplugged                = true;  // → back to idle state
    }

    completion_reset_active.store(false);
    EVLOG_info << "EnergyReader: [RESET] All session data zeroed at 10s after charge complete -> idle state.";
}

// ─────────────────────────────────────────────────────────────────────────────
// EVSE Manager session event callback
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::on_session_event(const types::evse_manager::SessionEvent& event) {
    std::lock_guard<std::mutex> lock(display_mutex);

    switch (event.event) {

        // ── Gun plugged in ────────────────────────────────────────────────────
        case types::evse_manager::SessionEventEnum::SessionStarted:
            // Cancel any in-flight post-completion reset so it cannot zero
            // data that belongs to the new session.
            completion_reset_active.store(false);
            completion_reset_generation.fetch_add(1);

            display_values.gun_plugged                  = true;
            display_values.gun_unplugged                = false;
            display_values.charging_active_icon         = false;
            display_values.charging_complete_icon       = false;
            display_values.fault                        = false;
            display_values.charging_active              = false;
            // Reset demand/supply values for the new session
            display_values.demand_voltage               = 0.0f;
            display_values.demand_current               = 0.0f;
            display_values.charging_voltage             = 0.0f;
            display_values.charging_current             = 0.0f;
            // Reset SOC for new session
            display_values.current_soc_percent          = 0.0f;
            display_values.initial_soc_percent          = 0.0f;
            display_values.initial_soc_captured         = false;
            // Reset duration accumulators
            display_values.charge_duration_seconds      = 0;
            display_values.accumulated_duration_seconds = 0;
            // Reset session meter min/max
            display_values.max_voltage                  = 0.0f;
            display_values.min_voltage                  = 0.0f;
            display_values.max_current                  = 0.0f;
            display_values.min_current                  = 0.0f;
            display_values.initialized                  = false;

            // ── Snapshot lifetime energy totals as this session's baseline ────
            // display_values.energy_import_kwh / energy_export_kwh still hold
            // the last raw (pre-reset) meter readings at this point, i.e. the
            // meter's lifetime total in kWh. Save that as the baseline, then
            // zero the session-facing values so the display reads 0 kWh at
            // the start of the new session. Subsequent update_display_values
            // calls will report (lifetime_total - baseline).
            session_energy_import_baseline_kWh = display_values.energy_import_kwh;
            session_energy_export_baseline_kWh = display_values.energy_export_kwh;
            display_values.energy_import_kwh             = 0.0f;
            display_values.energy_export_kwh             = 0.0f;

            EVLOG_info << "[ICON] SessionStarted → gun_plugged=1";
            break;

        // ── Charging begins ───────────────────────────────────────────────────
        case types::evse_manager::SessionEventEnum::ChargingStarted:
            display_values.gun_plugged                  = true;
            display_values.gun_unplugged                = false;
            display_values.charging_active_icon         = true;
            display_values.charging_complete_icon       = false;
            display_values.fault                        = false;
            display_values.charging_active              = true;
            // Reset duration for fresh session start
            display_values.charge_start_time            = std::chrono::steady_clock::now();
            display_values.charge_duration_seconds      = 0;
            display_values.accumulated_duration_seconds = 0;

            EVLOG_info << "[ICON] ChargingStarted → charging_active=1, duration reset";
            break;

        // ── Charging paused — freeze the duration counter ─────────────────────
        case types::evse_manager::SessionEventEnum::ChargingPausedEV:
        case types::evse_manager::SessionEventEnum::ChargingPausedEVSE:
        case types::evse_manager::SessionEventEnum::WaitingForEnergy:
        case types::evse_manager::SessionEventEnum::SwitchingPhases:
        case types::evse_manager::SessionEventEnum::ReplugStarted:
            freeze_duration();   // ← save elapsed into accumulator before stopping
            display_values.charging_active_icon = false;
            display_values.charging_active      = false;
            EVLOG_info << fmt::format(
                "[ICON] Paused → charging_active=0, accumulated={}s",
                display_values.accumulated_duration_seconds);
            break;

        // ── Charging resumed — restart the start_time from now ────────────────
        case types::evse_manager::SessionEventEnum::ChargingResumed:
        case types::evse_manager::SessionEventEnum::SessionResumed:
        case types::evse_manager::SessionEventEnum::ReplugFinished:
            display_values.charge_start_time   = std::chrono::steady_clock::now();
            display_values.charging_active_icon = true;
            display_values.charging_active      = true;
            EVLOG_info << fmt::format(
                "[ICON] Resumed → charging_active=1, accumulated={}s",
                display_values.accumulated_duration_seconds);
            break;

        // ── Charging complete — show complete icon for 5s then go idle ────────
        case types::evse_manager::SessionEventEnum::ChargingFinished:
        case types::evse_manager::SessionEventEnum::StoppingCharging:
        case types::evse_manager::SessionEventEnum::TransactionFinished:
            freeze_duration();   // ← capture final duration before marking inactive
            display_values.gun_plugged            = false;
            display_values.gun_unplugged          = false;
            display_values.charging_active_icon   = false;
            display_values.charging_complete_icon = true;
            display_values.charging_active        = false;
            display_values.fault                  = false;

            // Start the post-completion timer only once. EVerest can emit
            // StoppingCharging, ChargingFinished, and TransactionFinished in
            // quick succession for the same session.
            if (!completion_reset_active.exchange(true)) {
                const uint64_t generation = completion_reset_generation.fetch_add(1) + 1;
                if (completion_reset_thread.joinable()) {
                    completion_reset_thread.detach();
                }
                completion_reset_thread = std::thread([this, generation]() { completion_reset_run(generation); });
                EVLOG_info << "[ICON] ChargingFinished -> charging_complete=1, 5s/10s reset timer started";
            } else {
                EVLOG_info << "[ICON] ChargingFinished -> charging_complete=1, reset timer already running";
            }
            break;

        // ── Gun unplugged before timer fires ─────────────────────────────────
        case types::evse_manager::SessionEventEnum::SessionFinished:
            // ── If charging was already complete, let the 5s/10s timer keep running ──
            // ── If gun was pulled mid-session (no completion), cancel and go idle ─────
            if (display_values.charging_complete_icon && completion_reset_active.load()) {
                display_values.gun_plugged            = false;
                display_values.gun_unplugged          = false;
                display_values.charging_active_icon   = false;
                display_values.charging_active        = false;
                display_values.charging_complete_icon = true;
                display_values.fault                  = false;

                EVLOG_info << "[ICON] SessionFinished -> complete_icon kept=true (timer running)";
                break;
            } else {
                // Gun pulled before charging finished — cancel timer, go straight to idle
                completion_reset_active.store(false);
                completion_reset_generation.fetch_add(1);
                display_values.charging_complete_icon = false;
            }

            display_values.gun_plugged            = false;
            display_values.gun_unplugged          = true;   // idle
            display_values.charging_active_icon   = false;
            display_values.charging_active        = false;
            display_values.charging_complete_icon = false;
            display_values.fault                  = false;
            display_values.demand_voltage         = 0.0f;
            display_values.demand_current         = 0.0f;
            display_values.charging_voltage       = 0.0f;
            display_values.charging_current       = 0.0f;

            EVLOG_info << "[ICON] SessionFinished -> complete_icon kept=false (idle)";
            break;

        // ── Fault: gun plugged but EV did not communicate in time ─────────────
        case types::evse_manager::SessionEventEnum::PluginTimeout:
            display_values.fault = true;
            EVLOG_warning << "[FAULT] PluginTimeout received → fault=1";
            break;

        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Powermeter subscription callback (Gun 2)
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::on_powermeter_update_gun2(const types::powermeter::Powermeter& pm) {
    {
        std::lock_guard<std::mutex> lock(pm_mutex_gun2);
        pm_last_reading_gun2 = pm;
    }
    update_display_values_gun2(pm);
}

void energy_readerImpl::update_display_values_gun2(const types::powermeter::Powermeter& pm) {
    std::lock_guard<std::mutex> lock(display_mutex_gun2);

    EVLOG_debug << "EnergyReader Gun2: PM Voltage : "
                << (pm.voltage_V.has_value() && pm.voltage_V->DC.has_value()
                        ? pm.voltage_V->DC.value() : -1.0f);
    EVLOG_debug << "EnergyReader Gun2: PM Current : "
                << (pm.current_A.has_value() && pm.current_A->DC.has_value()
                        ? pm.current_A->DC.value() : -1.0f);
    if (pm.power_W.has_value()) {
        EVLOG_debug << "EnergyReader Gun2: PM Power Total = " << pm.power_W.value().total;
    }

    if (pm.voltage_V.has_value()) {
        float v = 0.0f;
        bool has_voltage = false;
        if (pm.voltage_V.value().DC.has_value()) {
            v = pm.voltage_V.value().DC.value();
            has_voltage = true;
        } else if (pm.voltage_V.value().L1.has_value()) {
            v = pm.voltage_V.value().L1.value();
            has_voltage = true;
        }

        if (has_voltage && !std::isfinite(v)) {
            EVLOG_warning << "EnergyReader Gun2: Discarding non-finite voltage_V reading: " << v;
            has_voltage = false;
        }

        if (has_voltage) {
            display_values_gun2.voltage_dc =
                ema_blend(display_values_gun2.voltage_dc, v, display_values_gun2.initialized);
            const float smoothed_v = display_values_gun2.voltage_dc;
            if (!display_values_gun2.initialized) {
                display_values_gun2.max_voltage = smoothed_v;
                display_values_gun2.min_voltage = smoothed_v;
            } else {
                display_values_gun2.max_voltage = std::max(display_values_gun2.max_voltage, smoothed_v);
                display_values_gun2.min_voltage = std::min(display_values_gun2.min_voltage, smoothed_v);
            }
        }
    }

    {
        float i = 0.0f;
        bool has_current = false;
        if (pm.current_A.has_value()) {
            if (pm.current_A.value().DC.has_value()) {
                i = pm.current_A.value().DC.value();
                has_current = true;
            } else if (pm.current_A.value().L1.has_value()) {
                i = pm.current_A.value().L1.value();
                has_current = true;
            }
        }

        if (has_current && !std::isfinite(i)) {
            EVLOG_warning << "EnergyReader Gun2: Discarding non-finite current_A reading: " << i;
            has_current = false;
        }

        if (has_current) {
            display_values_gun2.current_dc =
                ema_blend(display_values_gun2.current_dc, i, display_values_gun2.initialized);
            const float smoothed_i = display_values_gun2.current_dc;
            if (!display_values_gun2.initialized) {
                display_values_gun2.max_current = smoothed_i;
                display_values_gun2.min_current = smoothed_i;
            } else {
                display_values_gun2.max_current = std::max(display_values_gun2.max_current, smoothed_i);
                display_values_gun2.min_current = std::min(display_values_gun2.min_current, smoothed_i);
            }
        }
    }

    if (pm.power_W.has_value()) {
        const float total = pm.power_W.value().total;
        if (std::isfinite(total)) {
            const float sample_kw = total / 1000.0f;
            display_values_gun2.power_kw =
                ema_blend(display_values_gun2.power_kw, sample_kw, display_values_gun2.initialized);
        } else {
            EVLOG_warning << "EnergyReader Gun2: Discarding non-finite power_W.total reading: " << total;
        }
    }

    {
        const float total = pm.energy_Wh_import.total;
        if (std::isfinite(total)) {
            const float session_kwh = (total / 1000.0f) - session_energy_import_baseline_kWh_gun2;
            display_values_gun2.energy_import_kwh = std::max(0.0f, session_kwh);
        } else {
            EVLOG_warning << "EnergyReader Gun2: Discarding non-finite energy_Wh_import.total reading: " << total;
        }
    }

    if (pm.energy_Wh_export.has_value()) {
        const float total = pm.energy_Wh_export.value().total;
        if (std::isfinite(total)) {
            const float session_kwh = (total / 1000.0f) - session_energy_export_baseline_kWh_gun2;
            display_values_gun2.energy_export_kwh = std::max(0.0f, session_kwh);
        } else {
            EVLOG_warning << "EnergyReader Gun2: Discarding non-finite energy_Wh_export.total reading: " << total;
        }
    }

    display_values_gun2.timestamp   = pm.timestamp;
    display_values_gun2.initialized = true;

    // ── AC mode: map meter V/I to charging_voltage/current for DWIN display ──
    display_values_gun2.charging_voltage = display_values_gun2.voltage_dc;
    display_values_gun2.charging_current = display_values_gun2.current_dc;
}

// ─────────────────────────────────────────────────────────────────────────────
// ev_info callback (Gun 2)
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::on_ev_info_update_gun2(const types::evse_manager::EVInfo& ev_info) {
    std::lock_guard<std::mutex> lock(display_mutex_gun2);

    if (ev_info.target_voltage.has_value()) {
        display_values_gun2.demand_voltage = ev_info.target_voltage.value();
        EVLOG_debug << fmt::format("EnergyReader Gun2: Demand voltage = {:.2f} V",
                                   display_values_gun2.demand_voltage);
    }

    if (ev_info.target_current.has_value()) {
        display_values_gun2.demand_current = ev_info.target_current.value();
        EVLOG_debug << fmt::format("EnergyReader Gun2: Demand current = {:.2f} A",
                                   display_values_gun2.demand_current);
    }

    if (ev_info.present_voltage.has_value()) {
        display_values_gun2.charging_voltage = ev_info.present_voltage.value();
        EVLOG_debug << fmt::format("EnergyReader Gun2: Charging voltage = {:.2f} V",
                                   display_values_gun2.charging_voltage);
    }

    if (ev_info.present_current.has_value()) {
        display_values_gun2.charging_current = ev_info.present_current.value();
        EVLOG_debug << fmt::format("EnergyReader Gun2: Charging current = {:.2f} A",
                                   display_values_gun2.charging_current);
    }

    if (ev_info.soc.has_value()) {
        display_values_gun2.current_soc_percent = ev_info.soc.value();
        if (!display_values_gun2.initial_soc_captured && ev_info.soc.value() > 0.0f) {
            display_values_gun2.initial_soc_percent  = ev_info.soc.value();
            display_values_gun2.initial_soc_captured = true;
            EVLOG_info << fmt::format("EnergyReader Gun2: Initial SOC captured = {:.1f}%",
                                      display_values_gun2.initial_soc_percent);
        }
        EVLOG_debug << fmt::format("EnergyReader Gun2: Current SOC = {:.1f}%",
                                   display_values_gun2.current_soc_percent);
    }

    EVLOG_info << fmt::format(
        "EnergyReader Gun2: EV info — Demand={:.2f}V/{:.2f}A  Charging={:.2f}V/{:.2f}A",
        display_values_gun2.demand_voltage,  display_values_gun2.demand_current,
        display_values_gun2.charging_voltage, display_values_gun2.charging_current);
}

// ─────────────────────────────────────────────────────────────────────────────
// Duration helper (Gun 2)
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::freeze_duration_gun2() {
    if (display_values_gun2.charging_active) {
        auto elapsed = std::chrono::steady_clock::now()
                       - display_values_gun2.charge_start_time;
        display_values_gun2.accumulated_duration_seconds +=
            static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        EVLOG_info << fmt::format("EnergyReader Gun2: Duration frozen — accumulated={}s",
                                   display_values_gun2.accumulated_duration_seconds);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Post-completion reset timer (Gun 2)
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::completion_reset_run_gun2(uint64_t generation) {
    EVLOG_info << "EnergyReader Gun2: Post-completion reset timer started";

    for (int i = 0; i < 50; ++i) {
        if (!completion_reset_active_gun2.load() || completion_reset_generation_gun2.load() != generation) {
            EVLOG_info << "EnergyReader Gun2: Post-completion reset cancelled (pre-5s).";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!completion_reset_active_gun2.load() || completion_reset_generation_gun2.load() != generation) return;

    {
        std::lock_guard<std::mutex> lock(display_mutex_gun2);
        display_values_gun2.charging_complete_icon = false;
        display_values_gun2.gun_plugged            = false;
        display_values_gun2.gun_unplugged          = true;
        EVLOG_info << "[ICON Gun2] charging_complete_icon cleared after 5s -> gun_unplugged=1";
    }

    for (int i = 0; i < 50; ++i) {
        if (!completion_reset_active_gun2.load() || completion_reset_generation_gun2.load() != generation) {
            EVLOG_info << "EnergyReader Gun2: Post-completion data reset cancelled (pre-10s).";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!completion_reset_active_gun2.load() || completion_reset_generation_gun2.load() != generation) return;

    {
        std::lock_guard<std::mutex> lock(display_mutex_gun2);

        display_values_gun2.demand_voltage               = 0.0f;
        display_values_gun2.demand_current               = 0.0f;
        display_values_gun2.charging_voltage             = 0.0f;
        display_values_gun2.charging_current             = 0.0f;
        display_values_gun2.current_soc_percent          = 0.0f;
        display_values_gun2.initial_soc_percent          = 0.0f;
        display_values_gun2.initial_soc_captured         = false;
        display_values_gun2.charge_duration_seconds      = 0;
        display_values_gun2.accumulated_duration_seconds = 0;
        display_values_gun2.voltage_dc                   = 0.0f;
        display_values_gun2.current_dc                   = 0.0f;
        display_values_gun2.power_kw                     = 0.0f;
        display_values_gun2.energy_import_kwh            = 0.0f;
        display_values_gun2.energy_export_kwh            = 0.0f;
        display_values_gun2.max_current                  = 0.0f;
        display_values_gun2.min_current                  = 0.0f;
        display_values_gun2.gun_unplugged                = true;
    }

    completion_reset_active_gun2.store(false);
    EVLOG_info << "EnergyReader Gun2: [RESET] All session data zeroed at 10s after charge complete -> idle state.";
}

// ─────────────────────────────────────────────────────────────────────────────
// EVSE Manager session event callback (Gun 2)
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::on_session_event_gun2(const types::evse_manager::SessionEvent& event) {
    std::lock_guard<std::mutex> lock(display_mutex_gun2);

    switch (event.event) {
        case types::evse_manager::SessionEventEnum::SessionStarted:
            completion_reset_active_gun2.store(false);
            completion_reset_generation_gun2.fetch_add(1);

            display_values_gun2.gun_plugged                  = true;
            display_values_gun2.gun_unplugged                = false;
            display_values_gun2.charging_active_icon         = false;
            display_values_gun2.charging_complete_icon       = false;
            display_values_gun2.fault                        = false;
            display_values_gun2.charging_active              = false;
            display_values_gun2.demand_voltage               = 0.0f;
            display_values_gun2.demand_current               = 0.0f;
            display_values_gun2.charging_voltage             = 0.0f;
            display_values_gun2.charging_current             = 0.0f;
            display_values_gun2.current_soc_percent          = 0.0f;
            display_values_gun2.initial_soc_percent          = 0.0f;
            display_values_gun2.initial_soc_captured         = false;
            display_values_gun2.charge_duration_seconds      = 0;
            display_values_gun2.accumulated_duration_seconds = 0;
            display_values_gun2.max_voltage                  = 0.0f;
            display_values_gun2.min_voltage                  = 0.0f;
            display_values_gun2.max_current                  = 0.0f;
            display_values_gun2.min_current                  = 0.0f;
            display_values_gun2.initialized                  = false;

            session_energy_import_baseline_kWh_gun2 = display_values_gun2.energy_import_kwh;
            session_energy_export_baseline_kWh_gun2 = display_values_gun2.energy_export_kwh;
            display_values_gun2.energy_import_kwh   = 0.0f;
            display_values_gun2.energy_export_kwh   = 0.0f;

            EVLOG_info << "[ICON Gun2] SessionStarted → gun_plugged=1";
            break;

        case types::evse_manager::SessionEventEnum::ChargingStarted:
            display_values_gun2.gun_plugged                  = true;
            display_values_gun2.gun_unplugged                = false;
            display_values_gun2.charging_active_icon         = true;
            display_values_gun2.charging_complete_icon       = false;
            display_values_gun2.fault                        = false;
            display_values_gun2.charging_active              = true;
            display_values_gun2.charge_start_time            = std::chrono::steady_clock::now();
            display_values_gun2.charge_duration_seconds      = 0;
            display_values_gun2.accumulated_duration_seconds = 0;

            EVLOG_info << "[ICON Gun2] ChargingStarted → charging_active=1, duration reset";
            break;

        case types::evse_manager::SessionEventEnum::ChargingPausedEV:
        case types::evse_manager::SessionEventEnum::ChargingPausedEVSE:
        case types::evse_manager::SessionEventEnum::WaitingForEnergy:
        case types::evse_manager::SessionEventEnum::SwitchingPhases:
        case types::evse_manager::SessionEventEnum::ReplugStarted:
            freeze_duration_gun2();
            display_values_gun2.charging_active_icon = false;
            display_values_gun2.charging_active      = false;
            EVLOG_info << fmt::format(
                "[ICON Gun2] Paused → charging_active=0, accumulated={}s",
                display_values_gun2.accumulated_duration_seconds);
            break;

        case types::evse_manager::SessionEventEnum::ChargingResumed:
        case types::evse_manager::SessionEventEnum::SessionResumed:
        case types::evse_manager::SessionEventEnum::ReplugFinished:
            display_values_gun2.charge_start_time   = std::chrono::steady_clock::now();
            display_values_gun2.charging_active_icon = true;
            display_values_gun2.charging_active      = true;
            EVLOG_info << fmt::format(
                "[ICON Gun2] Resumed → charging_active=1, accumulated={}s",
                display_values_gun2.accumulated_duration_seconds);
            break;

        case types::evse_manager::SessionEventEnum::ChargingFinished:
        case types::evse_manager::SessionEventEnum::StoppingCharging:
        case types::evse_manager::SessionEventEnum::TransactionFinished:
            freeze_duration_gun2();
            display_values_gun2.gun_plugged            = false;
            display_values_gun2.gun_unplugged          = false;
            display_values_gun2.charging_active_icon   = false;
            display_values_gun2.charging_complete_icon = true;
            display_values_gun2.charging_active        = false;
            display_values_gun2.fault                  = false;

            if (!completion_reset_active_gun2.exchange(true)) {
                const uint64_t generation = completion_reset_generation_gun2.fetch_add(1) + 1;
                if (completion_reset_thread_gun2.joinable()) {
                    completion_reset_thread_gun2.detach();
                }
                completion_reset_thread_gun2 = std::thread([this, generation]() { completion_reset_run_gun2(generation); });
                EVLOG_info << "[ICON Gun2] ChargingFinished -> charging_complete=1, 5s/10s reset timer started";
            } else {
                EVLOG_info << "[ICON Gun2] ChargingFinished -> charging_complete=1, reset timer already running";
            }
            break;

        case types::evse_manager::SessionEventEnum::SessionFinished:
            if (display_values_gun2.charging_complete_icon && completion_reset_active_gun2.load()) {
                display_values_gun2.gun_plugged            = false;
                display_values_gun2.gun_unplugged          = false;
                display_values_gun2.charging_active_icon   = false;
                display_values_gun2.charging_active        = false;
                display_values_gun2.charging_complete_icon = true;
                display_values_gun2.fault                  = false;

                EVLOG_info << "[ICON Gun2] SessionFinished -> complete_icon kept=true (timer running)";
                break;
            } else {
                completion_reset_active_gun2.store(false);
                completion_reset_generation_gun2.fetch_add(1);
                display_values_gun2.charging_complete_icon = false;
            }

            display_values_gun2.gun_plugged            = false;
            display_values_gun2.gun_unplugged          = true;
            display_values_gun2.charging_active_icon   = false;
            display_values_gun2.charging_active        = false;
            display_values_gun2.charging_complete_icon = false;
            display_values_gun2.fault                  = false;
            display_values_gun2.demand_voltage         = 0.0f;
            display_values_gun2.demand_current         = 0.0f;
            display_values_gun2.charging_voltage       = 0.0f;
            display_values_gun2.charging_current       = 0.0f;

            EVLOG_info << "[ICON Gun2] SessionFinished -> complete_icon kept=false (idle)";
            break;

        case types::evse_manager::SessionEventEnum::PluginTimeout:
            display_values_gun2.fault = true;
            EVLOG_warning << "[FAULT Gun2] PluginTimeout received → fault=1";
            break;

        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Serial write loop
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::write_loop() {
    const auto interval = std::chrono::milliseconds(config.publish_interval_ms);
    uint64_t cycle = 0;

    while (running) {
        const auto loop_start = std::chrono::steady_clock::now();
        ++cycle;

        // ── Snapshot display values under mutex (brief lock) ─────────────────
        // The mutex is released BEFORE the slow serial writes (44 VP × 20ms
        // = ~880ms). This prevents blocking powermeter callbacks during writes.
        bool do_write = true;
        {
            std::lock_guard<std::mutex> lock(display_mutex);
            if (display_values.charging_active) {
                auto elapsed = std::chrono::steady_clock::now()
                               - display_values.charge_start_time;
                display_values.charge_duration_seconds =
                    display_values.accumulated_duration_seconds +
                    static_cast<uint32_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
            }
        }
        {
            std::lock_guard<std::mutex> lock2(display_mutex_gun2);
            if (display_values_gun2.charging_active) {
                auto elapsed = std::chrono::steady_clock::now()
                               - display_values_gun2.charge_start_time;
                display_values_gun2.charge_duration_seconds =
                    display_values_gun2.accumulated_duration_seconds +
                    static_cast<uint32_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
            }
        }

        // ── Write to DWIN + print footer + JSON (no mutex held) ──────────────
        {
            const bool ok = write_all_display_registers();
            print_transmit_footer(ok, cycle);
            publish_write_status(ok, ok ? "" : "One or more VP writes failed");
            write_json_file();
        }

        const auto elapsed = std::chrono::steady_clock::now() - loop_start;
        if (elapsed < interval) {
            std::this_thread::sleep_for(interval - elapsed);
        }
    }

    close_serial_connection();
}

// ─────────────────────────────────────────────────────────────────────────────
// Transmit footer
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::print_transmit_footer(bool success, uint64_t cycle) {
    const auto& dv  = display_values;
    const auto& cfg = config;

    // ── Gun 1 print ───────────────────────────────────────────────────────────
    printf("\n─────────────────────── [Cycle %04lu] %s ───────────────────────────\n",
           static_cast<unsigned long>(cycle),
           success ? "OK " : "ERR");

    printf("[GUN1] Meter  : V=%7.2fV  I=%6.2fA  P=%6.3fkW  Ei=%7.3fkWh  Ee=%7.3fkWh\n",
           dv.voltage_dc, dv.current_dc, dv.power_kw,
           dv.energy_import_kwh, dv.energy_export_kwh);

    printf("[GUN1] MinMax : Vmax=%7.2fV  Vmin=%7.2fV  Imax=%6.2fA  Imin=%6.2fA\n",
           dv.max_voltage, dv.min_voltage,
           dv.max_current, dv.min_current);

    printf("[GUN1] HLC    : DmdV=%7.2fV  DmdI=%6.2fA  ChgV=%7.2fV  ChgI=%6.2fA  SOC=%5.1f%%\n",
           dv.demand_voltage,   dv.demand_current,
           dv.charging_voltage, dv.charging_current,
           dv.current_soc_percent);

    {
        const uint32_t g1_hh = dv.charge_duration_seconds / 3600;
        const uint32_t g1_mm = (dv.charge_duration_seconds % 3600) / 60;
        const uint32_t g1_ss = dv.charge_duration_seconds % 60;
        printf("[GUN1] State  : plug=%d  unplug=%d  charging=%d  complete=%d  fault=%d  dur=%02u:%02u:%02u\n",
               dv.gun_plugged ? 1 : 0,
               dv.gun_unplugged ? 1 : 0,
               dv.charging_active_icon ? 1 : 0,
               dv.charging_complete_icon ? 1 : 0,
               dv.fault ? 1 : 0,
               g1_hh, g1_mm, g1_ss);
    }

    // ── Gun 2 print ───────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock2(display_mutex_gun2);
        const auto& dv2 = display_values_gun2;

        printf("[GUN2] Meter  : V=%7.2fV  I=%6.2fA  P=%6.3fkW  Ei=%7.3fkWh  Ee=%7.3fkWh\n",
               dv2.voltage_dc, dv2.current_dc, dv2.power_kw,
               dv2.energy_import_kwh, dv2.energy_export_kwh);

        printf("[GUN2] MinMax : Vmax=%7.2fV  Vmin=%7.2fV  Imax=%6.2fA  Imin=%6.2fA\n",
               dv2.max_voltage, dv2.min_voltage,
               dv2.max_current, dv2.min_current);

        printf("[GUN2] HLC    : DmdV=%7.2fV  DmdI=%6.2fA  ChgV=%7.2fV  ChgI=%6.2fA  SOC=%5.1f%%\n",
               dv2.demand_voltage,   dv2.demand_current,
               dv2.charging_voltage, dv2.charging_current,
               dv2.current_soc_percent);

        const uint32_t g2_hh = dv2.charge_duration_seconds / 3600;
        const uint32_t g2_mm = (dv2.charge_duration_seconds % 3600) / 60;
        const uint32_t g2_ss = dv2.charge_duration_seconds % 60;
        printf("[GUN2] State  : plug=%d  unplug=%d  charging=%d  complete=%d  fault=%d  dur=%02u:%02u:%02u\n",
               dv2.gun_plugged ? 1 : 0,
               dv2.gun_unplugged ? 1 : 0,
               dv2.charging_active_icon ? 1 : 0,
               dv2.charging_complete_icon ? 1 : 0,
               dv2.fault ? 1 : 0,
               g2_hh, g2_mm, g2_ss);
    }

    printf("────────────────────────────────────────────────────────────────────\n");
    fflush(stdout);

    if (success) {
        // EVLOG_info << fmt::format(
        //     "[TX Cycle {}] All DWIN VP registers OK → port={}",
        //     cycle, config.serial_port);
    } else {
        // EVLOG_warning << fmt::format(
        //     "[TX Cycle {}] One or more DWIN VP writes FAILED → port={}",
        //     cycle, config.serial_port);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DWIN VP register writes
// ─────────────────────────────────────────────────────────────────────────────

bool energy_readerImpl::write_all_display_registers() {
    const auto& cfg = config;
    bool all_ok = true;

    // ── Debug log display_values right before they hit the wire ──────────────
    // If these values are already wrong here, the problem is upstream (in
    // update_display_values / the powermeter data itself), not in the DWIN
    // write path below.
    EVLOG_debug << "EnergyReader: Voltage      : " << display_values.voltage_dc;
    EVLOG_debug << "EnergyReader: Current      : " << display_values.current_dc;
    EVLOG_debug << "EnergyReader: Power        : " << display_values.power_kw;
    EVLOG_debug << "EnergyReader: Energy       : " << display_values.energy_import_kwh;
    EVLOG_debug << "EnergyReader: Max Voltage  : " << display_values.max_voltage;
    EVLOG_debug << "EnergyReader: Min Voltage  : " << display_values.min_voltage;
    EVLOG_debug << "EnergyReader: Max Current  : " << display_values.max_current;
    EVLOG_debug << "EnergyReader: Min Current  : " << display_values.min_current;

    auto write = [&](uint16_t vp_addr, uint16_t raw_val, const char* label) {
        if (!write_vp_register(vp_addr, raw_val)) {
            EVLOG_warning << fmt::format(
                "EnergyReader: Failed to write {} (VP 0x{:04X})", label, vp_addr);
            all_ok = false;
        }
    };

    // ── Powermeter measured values (Gun 1) ────────────────────────────────────
    // EVLOG_info << fmt::format(
    // "TX VP=0x{:04X} Value={} Raw={}",
    // cfg.reg_current_dc,
    // display_values.current_dc,
    // scale_to_u16(display_values.current_dc, cfg.current_scale));
    write(cfg.reg_voltage_dc,
          scale_to_u16(display_values.voltage_dc,        cfg.voltage_scale), "voltage_dc");
    // EVLOG_info << fmt::format(
    // "TX VP=0x{:04X} Value={} Raw={}",
    // cfg.reg_current_dc,
    // display_values.current_dc,
    // scale_to_u16(display_values.current_dc, cfg.current_scale));
    write(cfg.reg_current_dc,
          scale_to_u16(display_values.current_dc,        cfg.current_scale), "current_dc");
    write(cfg.reg_power_total,
          scale_to_u16(display_values.power_kw,          cfg.power_scale),   "power_kw");
    write(cfg.reg_energy_import_total,
          scale_to_u16(display_values.energy_import_kwh, cfg.energy_scale),  "energy_import_kwh");
    write(cfg.reg_energy_export_total,
          scale_to_u16(display_values.energy_export_kwh, cfg.energy_scale),  "energy_export_kwh");
    write(cfg.reg_max_voltage,
          scale_to_u16(display_values.max_voltage,       cfg.voltage_scale), "max_voltage");
    write(cfg.reg_min_voltage,
          scale_to_u16(display_values.min_voltage,       cfg.voltage_scale), "min_voltage");
    write(cfg.reg_max_current,
          scale_to_u16(display_values.max_current,       cfg.current_scale), "max_current");
    write(cfg.reg_min_current,
          scale_to_u16(display_values.min_current,       cfg.current_scale), "min_current");

    // ── HLC demand values (EV requested via ISO 15118 CurrentDemandReq) ───────
    write(cfg.reg_demand_voltage,
          scale_to_u16(display_values.demand_voltage,    cfg.voltage_scale), "demand_voltage");
    write(cfg.reg_demand_current,
          scale_to_u16(display_values.demand_current,    cfg.current_scale), "demand_current");

    // ── Charging V/I — show only when charging is active, else 0 ─────────────
    {
        const float chg_v = display_values.charging_active ? display_values.charging_voltage : 0.0f;
        const float chg_i = display_values.charging_active ? display_values.charging_current : 0.0f;
        write(cfg.reg_charging_voltage,
              scale_to_u16(chg_v, cfg.voltage_scale), "charging_voltage");
        write(cfg.reg_charging_current,
              scale_to_u16(chg_i, cfg.current_scale), "charging_current");
    }

    // ── Icon switch VPs (always 0 or 1) ──────────────────────────────────────
    write(cfg.reg_gun_status,
          static_cast<uint16_t>(display_values.gun_plugged ? 1 : 0),
          "gun_plugged");
    write(cfg.reg_gun_unplugged,
          static_cast<uint16_t>(display_values.gun_unplugged ? 1 : 0),
          "gun_unplugged");
    write(cfg.reg_charging_active,
          static_cast<uint16_t>(display_values.charging_active_icon ? 1 : 0),
          "charging_active");
    write(cfg.reg_charging_complete,
          static_cast<uint16_t>(display_values.charging_complete_icon ? 1 : 0),
          "charging_complete");
    write(cfg.reg_fault,
          static_cast<uint16_t>(display_values.fault ? 1 : 0),
          "fault");

    // ── Duration HH:MM:SS (Gun 1) — 3 separate VP writes ─────────────────────
    {
        const uint32_t total_sec = display_values.charge_duration_seconds;
        const uint16_t dur_hh   = static_cast<uint16_t>(total_sec / 3600);
        const uint16_t dur_mm   = static_cast<uint16_t>((total_sec % 3600) / 60);
        const uint16_t dur_ss   = static_cast<uint16_t>(total_sec % 60);
        write(cfg.reg_charge_duration_hours,   dur_hh, "charge_duration_hours");
        write(cfg.reg_charge_duration_minutes, dur_mm, "charge_duration_minutes");
        write(cfg.reg_charge_duration_seconds, dur_ss, "charge_duration_seconds");
    }
    // ── SOC (Gun 1) ───────────────────────────────────────────────────────────
    write(cfg.reg_initial_soc,
          scale_to_u16(display_values.initial_soc_percent, cfg.soc_scale), "initial_soc");
    write(cfg.reg_current_soc,
          scale_to_u16(display_values.current_soc_percent, cfg.soc_scale), "current_soc");

    // ── Powermeter & display values (Gun 2) ───────────────────────────────────
    {
        std::lock_guard<std::mutex> lock2(display_mutex_gun2);
        if (display_values_gun2.initialized) {
            write(cfg.reg_voltage_dc_gun2,
                  scale_to_u16(display_values_gun2.voltage_dc,        cfg.voltage_scale), "voltage_dc_gun2");
            write(cfg.reg_current_dc_gun2,
                  scale_to_u16(display_values_gun2.current_dc,        cfg.current_scale), "current_dc_gun2");
            write(cfg.reg_power_total_gun2,
                  scale_to_u16(display_values_gun2.power_kw,          cfg.power_scale),   "power_kw_gun2");
            write(cfg.reg_energy_import_total_gun2,
                  scale_to_u16(display_values_gun2.energy_import_kwh, cfg.energy_scale),  "energy_import_kwh_gun2");
            write(cfg.reg_energy_export_total_gun2,
                  scale_to_u16(display_values_gun2.energy_export_kwh, cfg.energy_scale),  "energy_export_kwh_gun2");
            write(cfg.reg_max_voltage_gun2,
                  scale_to_u16(display_values_gun2.max_voltage,       cfg.voltage_scale), "max_voltage_gun2");
            write(cfg.reg_min_voltage_gun2,
                  scale_to_u16(display_values_gun2.min_voltage,       cfg.voltage_scale), "min_voltage_gun2");
            write(cfg.reg_max_current_gun2,
                  scale_to_u16(display_values_gun2.max_current,       cfg.current_scale), "max_current_gun2");
            write(cfg.reg_min_current_gun2,
                  scale_to_u16(display_values_gun2.min_current,       cfg.current_scale), "min_current_gun2");

            write(cfg.reg_demand_voltage_gun2,
                  scale_to_u16(display_values_gun2.demand_voltage,    cfg.voltage_scale), "demand_voltage_gun2");
            write(cfg.reg_demand_current_gun2,
                  scale_to_u16(display_values_gun2.demand_current,    cfg.current_scale), "demand_current_gun2");

            // ── Charging V/I Gun2 — show only when charging active, else 0 ──
            {
                const float chg_v2 = display_values_gun2.charging_active ? display_values_gun2.charging_voltage : 0.0f;
                const float chg_i2 = display_values_gun2.charging_active ? display_values_gun2.charging_current : 0.0f;
                write(cfg.reg_charging_voltage_gun2,
                      scale_to_u16(chg_v2, cfg.voltage_scale), "charging_voltage_gun2");
                write(cfg.reg_charging_current_gun2,
                      scale_to_u16(chg_i2, cfg.current_scale), "charging_current_gun2");
            }

            write(cfg.reg_gun_status_gun2,
                  static_cast<uint16_t>(display_values_gun2.gun_plugged ? 1 : 0),
                  "gun_plugged_gun2");
            write(cfg.reg_gun_unplugged_gun2,
                  static_cast<uint16_t>(display_values_gun2.gun_unplugged ? 1 : 0),
                  "gun_unplugged_gun2");
            write(cfg.reg_charging_active_gun2,
                  static_cast<uint16_t>(display_values_gun2.charging_active_icon ? 1 : 0),
                  "charging_active_gun2");
            write(cfg.reg_charging_complete_gun2,
                  static_cast<uint16_t>(display_values_gun2.charging_complete_icon ? 1 : 0),
                  "charging_complete_gun2");
            write(cfg.reg_fault_gun2,
                  static_cast<uint16_t>(display_values_gun2.fault ? 1 : 0),
                  "fault_gun2");

            // ── Duration HH:MM:SS (Gun 2) — 3 separate VP writes ─────────────
            {
                const uint32_t total_sec2 = display_values_gun2.charge_duration_seconds;
                const uint16_t dur_hh2    = static_cast<uint16_t>(total_sec2 / 3600);
                const uint16_t dur_mm2    = static_cast<uint16_t>((total_sec2 % 3600) / 60);
                const uint16_t dur_ss2    = static_cast<uint16_t>(total_sec2 % 60);
                write(cfg.reg_charge_duration_hours_gun2,   dur_hh2, "charge_duration_hours_gun2");
                write(cfg.reg_charge_duration_minutes_gun2, dur_mm2, "charge_duration_minutes_gun2");
                write(cfg.reg_charge_duration_seconds_gun2, dur_ss2, "charge_duration_seconds_gun2");
            }

            // ── SOC (Gun 2) ───────────────────────────────────────────────────
            write(cfg.reg_initial_soc_gun2,
                  scale_to_u16(display_values_gun2.initial_soc_percent, cfg.soc_scale), "initial_soc_gun2");
            write(cfg.reg_current_soc_gun2,
                  scale_to_u16(display_values_gun2.current_soc_percent, cfg.soc_scale), "current_soc_gun2");
        }
    }

    return all_ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Low-level DWIN 8-byte packet write
//  [5A A5 05 82  VP_H VP_L  VAL_H VAL_L]
// ─────────────────────────────────────────────────────────────────────────────

bool energy_readerImpl::write_vp_register(uint16_t vp_address, uint16_t value) {
    std::lock_guard<std::mutex> lock(serial_mutex);

    if (serial_fd < 0) {
        EVLOG_error << "EnergyReader: Serial port not open.";
        return false;
    }

    const std::array<uint8_t, 8> packet = {
        PKT_HEADER_0,
        PKT_HEADER_1,
        PKT_LEN,
        PKT_CMD,
        static_cast<uint8_t>((vp_address >> 8) & 0xFF),
        static_cast<uint8_t>( vp_address        & 0xFF),
        static_cast<uint8_t>((value      >> 8) & 0xFF),
        static_cast<uint8_t>( value             & 0xFF)
    };

    const ssize_t written = write(serial_fd, packet.data(), packet.size());
    if (written != static_cast<ssize_t>(packet.size())) {
        EVLOG_warning << fmt::format(
            "EnergyReader: write() sent {}/{} bytes for VP 0x{:04X}",
            written, packet.size(), vp_address);
        return false;
    }

    tcdrain(serial_fd);

    // ── Inter-packet gap ──────────────────────────────────────────────────────
    // The DGUS T5L display processor needs time between packets to process each
    // VP write. Without this delay, back-to-back writes overflow the display's
    // UART receive buffer and cause a BLACK SCREEN / display reset.
    // 5 ms is sufficient at 115200 baud; increase to 10 ms if black screen
    // persists (e.g. slower display firmware or lower baud rates).
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EVLOG_debug << fmt::format(
        "EnergyReader: VP 0x{:04X} ← {:5u}  [5A A5 05 82 {:02X} {:02X} {:02X} {:02X}]",
        vp_address, value,
        packet[4], packet[5], packet[6], packet[7]);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

uint16_t energy_readerImpl::scale_to_u16(float value, double scale) {
    // Guard against NaN / Infinity from upstream (e.g. a bad powermeter
    // reading). Casting a non-finite double to an integer is undefined /
    // implementation-defined behavior and can produce arbitrary garbage
    // on the DWIN display, so bail out to 0 instead.
    if (!std::isfinite(value)) {
        return 0;
    }

    const double scaled = static_cast<double>(value) * scale;

    if (!std::isfinite(scaled)) return 0;
    if (scaled < 0.0)           return 0;
    if (scaled > 65535.0)       return 65535;
    return static_cast<uint16_t>(std::round(scaled));
}

// ─── JSON status file writer ───────────────────────────────────────────────
//   Writes all live charger data to:
//     /usr/share/everest/modules/EnergyReader/reader_data.json
//
//   charger_state is derived from the boolean icon flags (priority order):
//     fault              → "fault"
//     charging_complete  → "completed"
//     gun_unplugged      → "unplugged"
//     charging_active    → "inprogress"
//     gun_plugged        → "plugged"
//     (none active)      → "idle"
// ─────────────────────────────────────────────────────────────────────────────

void energy_readerImpl::write_json_file() {
    json root;

    // ── Gun 1 Data ────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(display_mutex);
        const auto& dv = display_values;

        std::string charger_state = "idle";
        if      (dv.fault)                  charger_state = "fault";
        else if (dv.charging_complete_icon) charger_state = "completed";
        else if (dv.gun_unplugged)          charger_state = "unplugged";
        else if (dv.charging_active_icon)   charger_state = "inprogress";
        else if (dv.gun_plugged)            charger_state = "plugged";

        const uint32_t total_sec = dv.charge_duration_seconds;
        const uint32_t hh  = total_sec / 3600;
        const uint32_t mm  = (total_sec % 3600) / 60;
        const uint32_t ss  = total_sec % 60;
        char dur_buf[16];
        std::snprintf(dur_buf, sizeof(dur_buf), "%02u:%02u:%02u", hh, mm, ss);

        json& g1 = root["gun1"];
        const bool is_charging = (charger_state == "inprogress" || charger_state == "completed");
        const float charging_current_out = is_charging ? dv.current_dc : 0.0f;
        const float charging_voltage_out = is_charging ? dv.voltage_dc : 0.0f;

        const float energy_kwh_calculated = is_charging
            ? (dv.voltage_dc * dv.current_dc * static_cast<float>(dv.charge_duration_seconds)) / 3600000.0f
            : 0.0f;

        g1["charger_state"]          = charger_state;
        g1["charging_current"]       = charging_current_out;
        g1["charging_voltage"]       = charging_voltage_out;
        g1["current_soc_percent"]    = dv.current_soc_percent;
        g1["demand_current"]         = dv.demand_current;
        g1["demand_voltage"]         = dv.demand_voltage;
        g1["duration"]               = std::string(dur_buf);
        g1["energy_consumption_kwh"] = energy_kwh_calculated;
        g1["initial_soc_percent"]    = dv.initial_soc_percent;

        json& m1 = root["gun1"]["meter"];
        m1["voltage_dc_v"]      = std::round(dv.voltage_dc        * 100.0)   / 100.0;
        m1["current_dc_a"]      = dv.current_dc;
        m1["power_kw"]          = std::round(dv.power_kw          * 1000.0)  / 1000.0;
        m1["energy_import_kwh"] = std::round(dv.energy_import_kwh * 1000.0f) / 1000.0f;
        m1["energy_export_kwh"] = std::round(dv.energy_export_kwh * 1000.0f) / 1000.0f;
        m1["max_voltage_v"]     = std::round(dv.max_voltage       * 100.0f)  / 100.0f;
        m1["min_voltage_v"]     = dv.min_voltage;
        m1["max_current_a"]     = dv.max_current;
        m1["min_current_a"]     = dv.min_current;
        m1["timestamp"]         = dv.timestamp;
    }

    // ── Gun 2 Data ────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(display_mutex_gun2);
        const auto& dv2 = display_values_gun2;

        std::string charger_state2 = "idle";
        if      (dv2.fault)                  charger_state2 = "fault";
        else if (dv2.charging_complete_icon) charger_state2 = "completed";
        else if (dv2.gun_unplugged)          charger_state2 = "unplugged";
        else if (dv2.charging_active_icon)   charger_state2 = "inprogress";
        else if (dv2.gun_plugged)            charger_state2 = "plugged";

        const uint32_t total_sec2 = dv2.charge_duration_seconds;
        const uint32_t hh2  = total_sec2 / 3600;
        const uint32_t mm2  = (total_sec2 % 3600) / 60;
        const uint32_t ss2  = total_sec2 % 60;
        char dur_buf2[16];
        std::snprintf(dur_buf2, sizeof(dur_buf2), "%02u:%02u:%02u", hh2, mm2, ss2);

        json& g2 = root["gun2"];
        const bool is_charging2 = (charger_state2 == "inprogress" || charger_state2 == "completed");
        const float charging_current_out2 = is_charging2 ? dv2.current_dc : 0.0f;
        const float charging_voltage_out2 = is_charging2 ? dv2.voltage_dc : 0.0f;

        const float energy_kwh_calculated2 = is_charging2
            ? (dv2.voltage_dc * dv2.current_dc * static_cast<float>(dv2.charge_duration_seconds)) / 3600000.0f
            : 0.0f;

        g2["charger_state"]          = charger_state2;
        g2["charging_current"]       = charging_current_out2;
        g2["charging_voltage"]       = charging_voltage_out2;
        g2["current_soc_percent"]    = dv2.current_soc_percent;
        g2["demand_current"]         = dv2.demand_current;
        g2["demand_voltage"]         = dv2.demand_voltage;
        g2["duration"]               = std::string(dur_buf2);
        g2["energy_consumption_kwh"] = energy_kwh_calculated2;
        g2["initial_soc_percent"]    = dv2.initial_soc_percent;

        json& m2 = root["gun2"]["meter"];
        m2["voltage_dc_v"]      = std::round(dv2.voltage_dc        * 100.0)   / 100.0;
        m2["current_dc_a"]      = dv2.current_dc;
        m2["power_kw"]          = std::round(dv2.power_kw          * 1000.0)  / 1000.0;
        m2["energy_import_kwh"] = std::round(dv2.energy_import_kwh * 1000.0f) / 1000.0f;
        m2["energy_export_kwh"] = std::round(dv2.energy_export_kwh * 1000.0f) / 1000.0f;
        m2["max_voltage_v"]     = std::round(dv2.max_voltage       * 100.0f)  / 100.0f;
        m2["min_voltage_v"]     = dv2.min_voltage;
        m2["max_current_a"]     = dv2.max_current;
        m2["min_current_a"]     = dv2.min_current;
        m2["timestamp"]         = dv2.timestamp;
    }

    // ── Write atomically (temp file → rename) ────────────────────────────────
    const std::string tmp_path = std::string(JSON_STATUS_FILE) + ".tmp";
    try {
        std::filesystem::path p(JSON_STATUS_FILE);
        std::filesystem::path dir = p.parent_path();
        if (!std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }

        std::ofstream ofs(tmp_path);
        if (!ofs.is_open()) {
            EVLOG_warning << fmt::format(
                "EnergyReader: Cannot open JSON temp file: {}", tmp_path);
            return;
        }
        ofs << root.dump(4) << "\n";
        ofs.close();

        if (ofs.fail()) {
            EVLOG_warning << fmt::format(
                "EnergyReader: Write to JSON temp file failed: {}", tmp_path);
            return;
        }

        // ── Rename temp file over the real target (atomic on same filesystem) ──
        // This step was previously missing: the file above was written but
        // never promoted to JSON_STATUS_FILE, so reader_data.json stayed
        // frozen at its last externally-written content forever.
        std::error_code ec;
        std::filesystem::rename(tmp_path, JSON_STATUS_FILE, ec);
        if (ec) {
            EVLOG_warning << fmt::format(
                "EnergyReader: Failed to rename {} -> {}: {}",
                tmp_path, JSON_STATUS_FILE, ec.message());
            return;
        }

        EVLOG_debug << "EnergyReader: JSON status file written successfully.";
    } catch (const std::exception& ex) {
        EVLOG_warning << fmt::format(
            "EnergyReader: Failed to write JSON status file: {}", ex.what());
    }
}

void energy_readerImpl::publish_write_status(bool success, const std::string& error_msg) {
    json status;
    status["success"]       = success;
    status["error_message"] = error_msg;
    status["timestamp"]     = Everest::Date::to_rfc3339(date::utc_clock::now());
    this->publish_display_write_status(status);
}

} // namespace main
} // namespace module