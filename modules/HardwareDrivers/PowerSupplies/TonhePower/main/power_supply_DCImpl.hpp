// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
#ifndef MAIN_POWER_SUPPLY_DC_IMPL_HPP
#define MAIN_POWER_SUPPLY_DC_IMPL_HPP

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 3
//

#include <generated/interfaces/power_supply_DC/Implementation.hpp>

#include "../TonhePower.hpp"

// ev@75ac1216-19eb-4182-a85c-820f1fc2c091:v1
#include <atomic>
#include <memory>
#include <optional>
#include <sstream>
// ev@75ac1216-19eb-4182-a85c-820f1fc2c091:v1

namespace module {
namespace main {

struct Conf {};

class power_supply_DCImpl : public power_supply_DCImplBase {
public:
    power_supply_DCImpl() = delete;
    power_supply_DCImpl(Everest::ModuleAdapter* ev, const Everest::PtrContainer<TonhePower>& mod,
                        Conf& config) :
        power_supply_DCImplBase(ev, "main"), mod(mod), config(config){};

    // ev@8ea32d28-373f-4c90-ae5e-b4fcc74e2a61:v1
    // insert your public definitions here
    // ev@8ea32d28-373f-4c90-ae5e-b4fcc74e2a61:v1

protected:
    // command handler functions (virtual)
    virtual void handle_setMode(types::power_supply_DC::Mode& mode,
                                types::power_supply_DC::ChargingPhase& phase) override;
    virtual void handle_setExportVoltageCurrent(double& voltage, double& current) override;
    virtual void handle_setImportVoltageCurrent(double& voltage, double& current) override;

    // ev@d2d1847a-7b88-41dd-ad07-92785f06f5c4:v1
    // insert your protected definitions here
    // ev@d2d1847a-7b88-41dd-ad07-92785f06f5c4:v1

private:
    const Everest::PtrContainer<TonhePower>& mod;
    const Conf& config;

    virtual void init() override;
    virtual void ready() override;

    // ev@3370e4dd-95f4-47a9-aaec-ea76f34a66c9:v1

    // Cached setpoints (atomic to allow concurrent signal callbacks)
    std::atomic<double> exportVoltage{0.};
    std::atomic<double> exportCurrentLimit{0.};
    std::atomic<double> minImportVoltage{0.};
    std::atomic<double> importCurrentLimit{0.};

    // Mode/phase tracking
    std::atomic<types::power_supply_DC::Mode>         mode{types::power_supply_DC::Mode::Off};
    std::atomic<types::power_supply_DC::ChargingPhase> phase{
        types::power_supply_DC::ChargingPhase::CableCheck};

    // Published capabilities (updated whenever active module count changes)
    types::power_supply_DC::Capabilities caps;
    std::optional<types::evse_board_support::HardwareCapabilities> evse_hw_caps;
    std::optional<double> evse_demand_voltage_V;
    std::optional<double> evse_demand_current_A;
    std::mutex                            caps_mutex;
    std::mutex                            evse_state_mutex;

    // Throttle counter for voltage_current publishing (~10 Hz)
    uint8_t throttle_cnt{0};

    // Cache the latest telemetry so cable-check and mode changes can publish
    // feedback immediately even before the next CAN update arrives.
    std::atomic<float> latest_voltage_V{0.f};
    std::atomic<float> latest_current_A{0.f};

    // Track last known active module count to detect hot-plug/unplug
    size_t last_module_count{0};

    bool firsttime{true};

    void apply_evse_manager_limits(types::power_supply_DC::Capabilities& caps) const;

    // Error helpers
    std::string map_error_string_to_power_supply_dc(const std::string& tonhe_error) const;
    std::string format_module_error_message(uint8_t node_id, const std::string& error,
                                            bool active) const;
    // ev@3370e4dd-95f4-47a9-aaec-ea76f34a66c9:v1
};

// ev@3d7da0ad-02c2-493d-9920-0bbbd56b9876:v1
// insert other definitions here
// ev@3d7da0ad-02c2-493d-9920-0bbbd56b9876:v1

} // namespace main
} // namespace module

#endif // MAIN_POWER_SUPPLY_DC_IMPL_HPP
