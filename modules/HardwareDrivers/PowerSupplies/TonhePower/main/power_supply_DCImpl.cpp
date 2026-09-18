// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
//
// power_supply_DCImpl for Tonhe Power – multi-module variant.
//
// Implements the "6 Power Modules on a Single CAN Bus" architecture:
//   • Capabilities are aggregated from all active modules.
//   • Current demand is split equally across active modules by the CAN driver.
//   • Mode changes and setpoints are re-applied automatically when modules
//     come back online after a fault.
//   • Per-module errors are mapped to standardised power_supply_DC error types.

#include "power_supply_DCImpl.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace module {
namespace main {

// ---------------------------------------------------------------------------
// Capabilities: individual module limits for PSUs
// These are used as a static fallback when hardware capability data has not
// yet been received from the modules.
// ---------------------------------------------------------------------------

static constexpr float PER_MODULE_MIN_VOLTAGE_V = 200.f;
static constexpr float PER_MODULE_MAX_VOLTAGE_V = 1000.f;
static constexpr float PER_MODULE_MIN_CURRENT_A = 1.f;
static constexpr float PER_MODULE_MAX_CURRENT_A = 30.f;  // 30 kW / 1000 V
static constexpr float PER_MODULE_MAX_POWER_W   = 30000.f;

// ---------------------------------------------------------------------------

void power_supply_DCImpl::apply_evse_manager_limits(types::power_supply_DC::Capabilities& caps) const {
    if (!evse_hw_caps.has_value()) {
        return;
    }

    const auto& hw_caps = evse_hw_caps.value();
    const auto voltage_limit_V = std::max(1.0, static_cast<double>(caps.max_export_voltage_V > 0.f
                                                                       ? caps.max_export_voltage_V
                                                                       : PER_MODULE_MAX_VOLTAGE_V));

    if (hw_caps.max_current_A_export > 0.0) {
        const double export_current_limit_A = std::min(
            caps.max_export_current_A > 0.f ? caps.max_export_current_A : hw_caps.max_current_A_export,
            hw_caps.max_current_A_export);
        caps.max_export_current_A = static_cast<float>(export_current_limit_A);

        const double export_power_limit_W = hw_caps.max_current_A_export * voltage_limit_V;
        const double capped_export_power_W = caps.max_export_power_W > 0.f ? caps.max_export_power_W
                                                                             : export_power_limit_W;
        caps.max_export_power_W = static_cast<float>(std::min(capped_export_power_W, export_power_limit_W));
    }

    if (hw_caps.max_current_A_import > 0.0f) {
        const double current_from_caps = static_cast<double>(caps.max_import_current_A.value_or(0.0f));
        const double hw_import_current = static_cast<double>(hw_caps.max_current_A_import);
        const double import_current_limit_A = std::min(
            current_from_caps > 0.0 ? current_from_caps : hw_import_current,
            hw_import_current);
        caps.max_import_current_A = static_cast<float>(import_current_limit_A);

        const double import_power_limit_W = hw_import_current * voltage_limit_V;
        const double power_from_caps = static_cast<double>(caps.max_import_power_W.value_or(0.0f));
        const double capped_import_power_W = power_from_caps > 0.0 ? power_from_caps : import_power_limit_W;
        caps.max_import_power_W = static_cast<float>(std::min(capped_import_power_W, import_power_limit_W));
    }
}

void power_supply_DCImpl::init() {
    // ------------------------------------------------------------------
    // Wire up CAN driver signals
    // ------------------------------------------------------------------

    // Aggregate voltage/current telemetry.
    // Cable check and active export need prompt feedback for EvseManager, so
    // those phases publish immediately while the rest stay throttled.
    mod->acdc->signalVoltageCurrent.connect([this](float voltage, float total_current) {
        const bool force_publish = (mode.load() == types::power_supply_DC::Mode::Export ||
                                    phase.load() == types::power_supply_DC::ChargingPhase::CableCheck);
        const bool periodic_publish = (throttle_cnt % 10U) == 0U;
        if (!force_publish && !periodic_publish) {
            ++throttle_cnt;
            return;
        }

        latest_voltage_V.store(voltage);
        latest_current_A.store(total_current);

        ++throttle_cnt;
        types::power_supply_DC::VoltageCurrent vc;
        vc.voltage_V  = voltage;
        vc.current_A  = total_current;
        publish_voltage_current(vc);
    });

    // Mode changes from any module
    mod->acdc->signalModuleStatus.connect(
        [this](can_packet_acdc::PowerModuleStatus status,
               can_packet_acdc::InverterStatus    inverter_status) {
            types::power_supply_DC::Mode new_mode;

            if (status.fault_alarm) {
                new_mode = types::power_supply_DC::Mode::Fault;
            } else if (status.dc_side_off) {
                new_mode = types::power_supply_DC::Mode::Off;
            } else if (inverter_status.invert_mode) {
                new_mode = types::power_supply_DC::Mode::Import;
            } else {
                new_mode = types::power_supply_DC::Mode::Export;
            }

            if (mode.load() != new_mode || firsttime) {
                firsttime = false;
                mode.store(new_mode);
                publish_mode(new_mode);
            }
        });

    // Capability / active-module-count updates
    mod->acdc->signalCapabilitiesUpdate.connect(
        [this](TonheCanDevice::TelemetryMap telemetries) {
            types::power_supply_DC::Capabilities new_caps;
            new_caps.bidirectional = true;

            // Fixed per-module tolerance / ripple values
            new_caps.current_regulation_tolerance_A = 1.f;
            new_caps.peak_current_ripple_A           = 0.2f;

            // Import capability (BEG1K075G supports bidirectional)
            new_caps.min_import_current_A = 0.f;
            new_caps.min_import_voltage_V = PER_MODULE_MIN_VOLTAGE_V;
            new_caps.max_import_voltage_V = PER_MODULE_MAX_VOLTAGE_V;

            size_t active_count = 0;
            float  min_max_v    = PER_MODULE_MAX_VOLTAGE_V;
            float  max_min_v    = PER_MODULE_MIN_VOLTAGE_V;
            float  total_max_current_A = 0.f;
            float  total_max_current_import_A = 0.f;
            float  total_max_power_W = 0.f;

            for (const auto& [node_id, t] : telemetries) {
                if (!t.online)
                    continue;
                ++active_count;

                if (t.valid_caps) {
                    // Use hardware-reported limits
                    min_max_v          = std::min(min_max_v, t.dc_max_output_voltage);
                    max_min_v          = std::max(max_min_v, t.dc_min_output_voltage);
                    total_max_current_A += t.dc_max_output_current;
                    total_max_power_W   += t.dc_rated_output_power;
                } else {
                    // Fall back to per-module static limits
                    total_max_current_A += PER_MODULE_MAX_CURRENT_A;
                    total_max_power_W   += static_cast<float>(
                        mod->config.per_module_rated_power_W);
                }
                total_max_current_import_A += PER_MODULE_MAX_CURRENT_A;
            }

            if (active_count == 0) {
                EVLOG_warning << "TonheBEG: No active modules – publishing zero capabilities.";
                new_caps.min_export_current_A = 0.f;
                new_caps.max_export_current_A = 0.f;
                new_caps.min_export_voltage_V = PER_MODULE_MIN_VOLTAGE_V;
                new_caps.max_export_voltage_V = PER_MODULE_MAX_VOLTAGE_V;
                new_caps.max_export_power_W   = 0.f;
                new_caps.max_import_current_A = 0.f;
                new_caps.max_import_power_W   = 0.f;
            } else {
                new_caps.min_export_current_A = PER_MODULE_MIN_CURRENT_A;
                new_caps.max_export_current_A = total_max_current_A;
                new_caps.min_export_voltage_V = max_min_v;
                new_caps.max_export_voltage_V = min_max_v;
                new_caps.max_export_power_W   = total_max_power_W;
                new_caps.max_import_current_A = total_max_current_import_A;
                new_caps.max_import_power_W   = total_max_power_W;
            }

            new_caps.conversion_efficiency_export =
                mod->config.conversion_efficiency_export;
            new_caps.conversion_efficiency_import =
                mod->config.conversion_efficiency_export;

            {
                std::lock_guard<std::mutex> lock(caps_mutex);
                apply_evse_manager_limits(new_caps);
                caps = new_caps;
            }

            EVLOG_info << "TonheBEG: Capabilities updated – " << active_count
                       << " active module(s): "
                       << new_caps.max_export_current_A << " A total, "
                       << new_caps.max_export_power_W   << " W total, "
                       << new_caps.min_export_voltage_V << "–"
                       << new_caps.max_export_voltage_V << " V";
            publish_capabilities(new_caps);

            // Hot-plug / hot-unplug: restore last setpoints so newly-recovered
            // modules immediately receive the correct commands.
            if (last_module_count != active_count && active_count > 0) {
                if (active_count > last_module_count) {
                    EVLOG_info << "TonheBEG: Hot-plug detected – " << active_count
                               << " module(s) online (was " << last_module_count
                               << "). Redistributing "
                               << exportCurrentLimit.load() << " A.";
                } else {
                    EVLOG_warning << "TonheBEG: Hot-unplug detected – " << active_count
                                  << " module(s) online (was " << last_module_count
                                  << "). Redistributing "
                                  << exportCurrentLimit.load() << " A.";
                }

                // Re-apply stored setpoints
                double v = exportVoltage.load();
                double i = exportCurrentLimit.load();
                if (v > 0. && i > 0.) {
                    handle_setExportVoltageCurrent(v, i);
                }
                types::power_supply_DC::Mode   m = mode.load();
                types::power_supply_DC::ChargingPhase p = phase.load();
                handle_setMode(m, p);
            }
            last_module_count = active_count;
        });

    if (mod->r_evse_manager) {
        mod->r_evse_manager->subscribe_ev_info(
            [this](const types::evse_manager::EVInfo& ev_info) {
                {
                    std::lock_guard<std::mutex> lock(evse_state_mutex);
                    if (ev_info.target_voltage.has_value()) {
                        evse_demand_voltage_V = static_cast<double>(ev_info.target_voltage.value());
                    } else {
                        evse_demand_voltage_V.reset();
                    }

                    if (ev_info.target_current.has_value()) {
                        evse_demand_current_A = static_cast<double>(ev_info.target_current.value());
                    } else {
                        evse_demand_current_A.reset();
                    }

                    EVLOG_info << "TonheBEG: received EvseManager demand V="
                               << (evse_demand_voltage_V.has_value() ? std::to_string(evse_demand_voltage_V.value()) : "n/a")
                               << " V, I="
                               << (evse_demand_current_A.has_value() ? std::to_string(evse_demand_current_A.value()) : "n/a")
                               << " A";
                }
            });

        mod->r_evse_manager->subscribe_hw_capabilities(
            [this](const types::evse_board_support::HardwareCapabilities& hw_caps) {
                {
                    std::lock_guard<std::mutex> lock(caps_mutex);
                    evse_hw_caps = hw_caps;
                    apply_evse_manager_limits(caps);
                }
                publish_capabilities(caps);
            });
    }

    // Per-module error signals
    mod->acdc->signalModuleError.connect(
        [this](uint8_t node_id, std::string error_str, bool active) {
            const std::string ev_error = map_error_string_to_power_supply_dc(error_str);
            const std::string msg      = format_module_error_message(node_id, error_str, active);

            const bool currently_active = error_state_monitor->is_error_active(ev_error, "");

            if (active && !currently_active) {
                EVLOG_error << msg;
                auto severity = (error_str == "FanFault")
                                    ? Everest::error::Severity::Medium
                                    : Everest::error::Severity::High;
                raise_error(error_factory->create_error(ev_error, "", msg, severity));
            } else if (!active && currently_active) {
                EVLOG_info << msg;
                clear_error(ev_error);
            }
        });

    // ------------------------------------------------------------------
    // Initial state
    // ------------------------------------------------------------------
    mod->acdc->switch_on_off(false);
    mod->acdc->adjust_power_factor(1.0f);
    mod->acdc->set_output_mode(TonheCanDevice::OutputMode::Automatic);

    // Populate static fallback capabilities so EvseManager can start
    {
        std::lock_guard<std::mutex> lock(caps_mutex);
        const size_t n = std::max<size_t>(1, mod->config.module_addresses.empty()
                                              ? 1
                                              : /* at least 1 */ 1);
        (void)n; // actual count comes from signalCapabilitiesUpdate
        caps.bidirectional                   = true;
        caps.current_regulation_tolerance_A  = 1.f;
        caps.peak_current_ripple_A           = 0.2f;
        caps.min_export_current_A            = PER_MODULE_MIN_CURRENT_A;
        caps.max_export_current_A            = PER_MODULE_MAX_CURRENT_A * 6.f; // assume up to 6
        caps.min_export_voltage_V            = PER_MODULE_MIN_VOLTAGE_V;
        caps.max_export_voltage_V            = PER_MODULE_MAX_VOLTAGE_V;
        caps.max_export_power_W              = PER_MODULE_MAX_POWER_W * 6.f;
        caps.conversion_efficiency_export    = mod->config.conversion_efficiency_export;
        caps.min_import_current_A            = 0.f;
        caps.max_import_current_A            = PER_MODULE_MAX_CURRENT_A * 6.f;
        caps.min_import_voltage_V            = PER_MODULE_MIN_VOLTAGE_V;
        caps.max_import_voltage_V            = PER_MODULE_MAX_VOLTAGE_V;
        caps.max_import_power_W              = PER_MODULE_MAX_POWER_W * 6.f;
        caps.conversion_efficiency_import    = mod->config.conversion_efficiency_export;
        apply_evse_manager_limits(caps);
    }
}

void power_supply_DCImpl::ready() {
    types::power_supply_DC::Capabilities current_caps;
    {
        std::lock_guard<std::mutex> lock(caps_mutex);
        current_caps = caps;
    }
    publish_capabilities(current_caps);
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

void power_supply_DCImpl::handle_setMode(types::power_supply_DC::Mode& m,
                                         types::power_supply_DC::ChargingPhase& p) {
    EVLOG_info << "TonheBEG: setMode(" << m << ") phase=" << p
               << "  active_modules=" << mod->acdc->active_module_count();

    if (m == types::power_supply_DC::Mode::Off) {
        mod->acdc->switch_on_off(false);
        mod->acdc->set_inverter_mode(false);
    } else if (m == types::power_supply_DC::Mode::Export) {
        mod->acdc->set_inverter_mode(false);
        mod->acdc->switch_on_off(true);
    } else if (m == types::power_supply_DC::Mode::Import) {
        mod->acdc->set_inverter_mode(true);
        mod->acdc->switch_on_off(true);
    } else if (m == types::power_supply_DC::Mode::Fault) {
        mod->acdc->switch_on_off(false);
        mod->acdc->set_inverter_mode(false);
    }

    mode.store(m);
    phase.store(p);

    types::power_supply_DC::VoltageCurrent vc;
    vc.voltage_V = latest_voltage_V.load();
    vc.current_A = latest_current_A.load();
    publish_voltage_current(vc);
}

void power_supply_DCImpl::handle_setExportVoltageCurrent(double& voltage, double& current) {
    types::power_supply_DC::Capabilities current_caps;
    {
        std::lock_guard<std::mutex> lock(caps_mutex);
        current_caps = caps;
    }

    {
        std::lock_guard<std::mutex> lock(evse_state_mutex);
        if (evse_demand_voltage_V.has_value() && evse_demand_voltage_V.value() > 0.0) {
            voltage = evse_demand_voltage_V.value();
        }
        if (evse_demand_current_A.has_value() && evse_demand_current_A.value() > 0.0) {
            current = evse_demand_current_A.value();
        }
    }

    // Clamp to reported capability limits
    voltage = std::max(static_cast<double>(current_caps.min_export_voltage_V),
                       std::min(static_cast<double>(current_caps.max_export_voltage_V), voltage));
    current = std::max(static_cast<double>(current_caps.min_export_current_A),
                       std::min(static_cast<double>(current_caps.max_export_current_A), current));

    exportVoltage.store(voltage);
    exportCurrentLimit.store(current);

    types::power_supply_DC::VoltageCurrent vc;
    vc.voltage_V = latest_voltage_V.load();
    vc.current_A = latest_current_A.load();
    publish_voltage_current(vc);

    const size_t active = mod->acdc->active_module_count();
    const double per_mod = active > 0 ? current / static_cast<double>(active) : current;

    EVLOG_info << "TonheBEG: setExportVoltageCurrent  " << voltage << " V  "
               << current << " A total  →  " << per_mod
               << " A/module  (" << active << " active)";

    mod->acdc->set_voltage_current(static_cast<float>(voltage),
                                   static_cast<float>(current));
}

void power_supply_DCImpl::handle_setImportVoltageCurrent(double& voltage, double& current) {
    types::power_supply_DC::Capabilities current_caps;
    {
        std::lock_guard<std::mutex> lock(caps_mutex);
        current_caps = caps;
    }

    if (!current_caps.min_import_voltage_V.has_value() ||
        !current_caps.max_import_voltage_V.has_value() ||
        !current_caps.min_import_current_A.has_value() ||
        !current_caps.max_import_current_A.has_value())
        return;

    voltage = std::max(static_cast<double>(current_caps.min_import_voltage_V.value()),
                       std::min(static_cast<double>(current_caps.max_import_voltage_V.value()), voltage));
    current = std::max(static_cast<double>(current_caps.min_import_current_A.value()),
                       std::min(static_cast<double>(current_caps.max_import_current_A.value()), current));

    minImportVoltage.store(voltage);
    importCurrentLimit.store(current);

    EVLOG_info << "TonheBEG: setImportVoltageCurrent  " << voltage << " V  " << current << " A";
    mod->acdc->set_voltage_current(static_cast<float>(voltage),
                                   static_cast<float>(current));
}

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

std::string power_supply_DCImpl::map_error_string_to_power_supply_dc(
    const std::string& tonhe_error) const {
    if (tonhe_error == "OverVoltageDC")      return "power_supply_DC/OverVoltageDC";
    if (tonhe_error == "UnderVoltageDC")     return "power_supply_DC/UnderVoltageDC";
    if (tonhe_error == "OverTemperature")    return "power_supply_DC/OverTemperature";
    if (tonhe_error == "OverCurrentDC")      return "power_supply_DC/OverCurrentDC";
    if (tonhe_error == "HardwareFault")      return "power_supply_DC/HardwareFault";
    if (tonhe_error == "CommunicationFault") return "power_supply_DC/CommunicationFault";
    if (tonhe_error == "UnderVoltageAC")     return "power_supply_DC/UnderVoltageAC";
    if (tonhe_error == "FanFault")           return "power_supply_DC/VendorWarning";
    if (tonhe_error == "InputPhaseLoss")     return "power_supply_DC/VendorError";
    return "power_supply_DC/VendorError";
}

std::string power_supply_DCImpl::format_module_error_message(uint8_t node_id,
                                                              const std::string& error,
                                                              bool active) const {
    std::ostringstream ss;
    ss << "TonheBEG[node=0x" << std::hex << std::uppercase
       << static_cast<int>(node_id) << "]: "
       << error << (active ? " detected" : " cleared");
    return ss.str();
}

} // namespace main
} // namespace module
