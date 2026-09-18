// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#ifndef MAIN_ENERGY_READER_IMPL_HPP
#define MAIN_ENERGY_READER_IMPL_HPP

#include <generated/interfaces/energy_reader/Implementation.hpp>
#include <generated/interfaces/powermeter/Interface.hpp>
#include <generated/interfaces/evse_manager/Interface.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Raw serial port headers
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <cstring>

namespace module {

namespace main {
struct Conf;
} // namespace main

class EnergyReader;

namespace main {

struct DisplayValues {
    // ── Powermeter measured values ────────────────────────────────────────────
    float voltage_dc{0.0f};          // Actual measured DC voltage  (V)
    float current_dc{0.0f};          // Actual measured DC current  (A)
    float power_kw{0.0f};
    float energy_import_kwh{0.0f};
    float energy_export_kwh{0.0f};
    float max_voltage{0.0f};
    float min_voltage{0.0f};
    float max_current{0.0f};
    float min_current{0.0f};
    std::string timestamp;
    bool initialized{false};
    uint32_t accumulated_duration_seconds = 0;

    // ── HLC demand values (EV requested via ISO 15118 CurrentDemandReq) ───────
    // Sourced from evse_manager → ev_info → dc_current_demand / dc_voltage_demand
    float demand_voltage{0.0f};      // EV target voltage requested  (V)
    float demand_current{0.0f};      // EV target current requested  (A)

    // ── Supply set values (EVSE commanded to DC power supply) ─────────────────
    // Sourced from evse_manager → limits → evse_max_current / evse_max_voltage
    // updated whenever powersupply_DC_set is called (reflected via ev_info)
    float charging_voltage{0.0f};    // Voltage commanded to PSU     (V)
    float charging_current{0.0f};    // Current commanded to PSU     (A)

    // ── Icon switch values (0=OFF  1=ON) ──────────────────────────────────────
    bool gun_plugged{false};
    bool gun_unplugged{true};
    bool charging_active_icon{false};
    bool charging_complete_icon{false};
    bool fault{false};

    // ── Charge duration timer ─────────────────────────────────────────────────
    uint32_t charge_duration_seconds{0};
    std::chrono::steady_clock::time_point charge_start_time;
    bool charging_active{false};

    // ── SOC values (from ev_info) ─────────────────────────────────────────────
    float current_soc_percent{0.0f};
    float initial_soc_percent{0.0f};
    bool  initial_soc_captured{false};
};

class energy_readerImpl : public energy_readerImplBase {
public:
    energy_readerImpl() = delete;

    energy_readerImpl(Everest::ModuleAdapter* ev,
                      const Everest::PtrContainer<EnergyReader>& mod,
                      Conf& config) :
        energy_readerImplBase(ev, "main"), mod(mod), config(config) {
    }

    void init();
    void ready();

    Object handle_get_last_values() override;

private:
    const Everest::PtrContainer<EnergyReader>& mod;
    const Conf& config;
    void freeze_duration();
    uint32_t accumulated_duration_seconds{0};
    // ── Serial port ───────────────────────────────────────────────────────────
    int serial_fd{-1};
    std::mutex serial_mutex;

    // ── Powermeter (Gun 1) ────────────────────────────────────────────────────
    types::powermeter::Powermeter pm_last_reading;
    std::mutex pm_mutex;

    // ── Powermeter (Gun 2) ────────────────────────────────────────────────────
    types::powermeter::Powermeter pm_last_reading_gun2;
    std::mutex pm_mutex_gun2;

    // ── Display state (Gun 1) ─────────────────────────────────────────────────
    DisplayValues display_values;
    std::mutex display_mutex;

    // ── Session energy baselines (lifetime totals captured at session start) ─
    float session_energy_import_baseline_kWh{0.0f};
    float session_energy_export_baseline_kWh{0.0f};
    float session_energy_import_baseline_kWh_gun2{0.0f};
    float session_energy_export_baseline_kWh_gun2{0.0f};

    // ── Display state (Gun 2) ─────────────────────────────────────────────────
    DisplayValues display_values_gun2;
    std::mutex display_mutex_gun2;

    // ── Write loop ────────────────────────────────────────────────────────────
    std::thread write_thread;
    std::atomic<bool> running{false};

    // ── Fault detection timer ─────────────────────────────────────────────────
    std::thread       fault_timer_thread;
    std::atomic<bool> fault_timer_active{false};

    // ── Post-completion reset timer (Gun 1) ───────────────────────────────────
    std::atomic<bool>     completion_reset_active{false};
    std::atomic<uint64_t> completion_reset_generation{0};
    std::thread           completion_reset_thread;
    void                  completion_reset_run(uint64_t generation);

    // ── Post-completion reset timer (Gun 2) ───────────────────────────────────
    std::atomic<bool>     completion_reset_active_gun2{false};
    std::atomic<uint64_t> completion_reset_generation_gun2{0};
    std::thread           completion_reset_thread_gun2;
    void                  completion_reset_run_gun2(uint64_t generation);

    void freeze_duration_gun2();
    // ── Serial connection ─────────────────────────────────────────────────────
    void open_serial_connection();
    void close_serial_connection();

    // ── Callbacks (Gun 1) ─────────────────────────────────────────────────────
    void on_powermeter_update(const types::powermeter::Powermeter& pm);
    void update_display_values(const types::powermeter::Powermeter& pm);
    void on_session_event(const types::evse_manager::SessionEvent& event);
    void on_ev_info_update(const types::evse_manager::EVInfo& ev_info);

    // ── Callbacks (Gun 2) ─────────────────────────────────────────────────────
    void on_powermeter_update_gun2(const types::powermeter::Powermeter& pm);
    void update_display_values_gun2(const types::powermeter::Powermeter& pm);
    void on_session_event_gun2(const types::evse_manager::SessionEvent& event);
    void on_ev_info_update_gun2(const types::evse_manager::EVInfo& ev_info);

    // ── Fault timer ───────────────────────────────────────────────────────────
    void fault_timer_run();

    // ── Write loop & printing ─────────────────────────────────────────────────
    void print_live_data();
    void print_transmit_header(uint64_t cycle);
    void print_transmit_footer(bool success, uint64_t cycle);
    void write_loop();
    bool write_vp_register(uint16_t vp_address, uint16_t value);
    bool write_all_display_registers();
    void publish_write_status(bool success, const std::string& error_msg = "");
    void write_json_file();                       // ← NEW: write JSON status file
    static uint16_t scale_to_u16(float value, double scale);
};

} // namespace main
} // namespace module

#endif // MAIN_ENERGY_READER_IMPL_HPP