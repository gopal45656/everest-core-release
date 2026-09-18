// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#ifndef ENERGY_READER_HPP
#define ENERGY_READER_HPP

#include "ld-ev.hpp"

// headers for provided interface implementations
#include <main/energy_readerImpl.hpp>

// headers for required interface implementations
#include <generated/interfaces/powermeter/Interface.hpp>
#include <generated/interfaces/evse_manager/Interface.hpp>

#include <optional>
#include <string>
#include <vector>

namespace module {

namespace main {
struct Conf {
    // ── Serial connection ─────────────────────────────────────────────────────
    int         modbus_device_id;
    std::string serial_port;
    int         baud_rate;
    std::string parity;
    int         stop_bits;
    int         data_bits;
    int         publish_interval_ms;
    int         fault_timeout_seconds;

    // ── Powermeter VP registers ───────────────────────────────────────────────
    int reg_voltage_dc;
    int reg_current_dc;
    int reg_power_total;
    int reg_energy_import_total;
    int reg_energy_export_total;
    int reg_max_voltage;
    int reg_min_voltage;
    int reg_max_current;
    int reg_min_current;

    // ── HLC demand VP registers (EV requested values) ─────────────────────────
    int reg_demand_voltage;          // EV target voltage from CurrentDemandReq
    int reg_demand_current;          // EV target current from CurrentDemandReq

    // ── Supply set VP registers (EVSE commanded output) ───────────────────────
    int reg_charging_voltage;        // Voltage commanded to DC power supply
    int reg_charging_current;        // Current commanded to DC power supply

    // ── Icon switch VP registers (0=OFF  1=ON) ────────────────────────────────
    int reg_gun_status;
    int reg_gun_unplugged;
    int reg_charging_active;
    int reg_charging_complete;
    int reg_fault;
    int reg_charge_duration_hours;
    int reg_charge_duration_minutes;
    int reg_charge_duration_seconds;
    int reg_initial_soc;
    int reg_current_soc;

    // ── Gun 2 Powermeter & Display VP registers ──────────────────────────────
    int reg_voltage_dc_gun2;
    int reg_current_dc_gun2;
    int reg_power_total_gun2;
    int reg_energy_import_total_gun2;
    int reg_energy_export_total_gun2;
    int reg_max_voltage_gun2;
    int reg_min_voltage_gun2;
    int reg_max_current_gun2;
    int reg_min_current_gun2;

    int reg_demand_voltage_gun2;
    int reg_demand_current_gun2;
    int reg_charging_voltage_gun2;
    int reg_charging_current_gun2;

    int reg_gun_status_gun2;
    int reg_gun_unplugged_gun2;
    int reg_charging_active_gun2;
    int reg_charging_complete_gun2;
    int reg_fault_gun2;
    int reg_charge_duration_hours_gun2;
    int reg_charge_duration_minutes_gun2;
    int reg_charge_duration_seconds_gun2;
    int reg_initial_soc_gun2;
    int reg_current_soc_gun2;

    // ── Scaling factors ───────────────────────────────────────────────────────
    double voltage_scale;
    double current_scale;
    double power_scale;
    double energy_scale;
    double soc_scale;

    bool debug;
};
} // namespace main

using Conf = main::Conf;

class EnergyReader : public Everest::ModuleBase {
public:
    EnergyReader() = delete;
    EnergyReader(const ModuleInfo& info,
                 std::unique_ptr<main::energy_readerImpl>        p_main,
                 std::vector<std::unique_ptr<powermeterIntf>>    r_powermeter,
                 std::vector<std::unique_ptr<evse_managerIntf>>  r_evse_manager,
                 main::Conf& config);

    const main::Conf& config;
    const std::unique_ptr<main::energy_readerImpl>        p_main;
    const std::vector<std::unique_ptr<powermeterIntf>>    r_powermeter;
    const std::vector<std::unique_ptr<evse_managerIntf>>  r_evse_manager;

    void init();
    void ready();
};

} // namespace module

#endif // ENERGY_READER_HPP