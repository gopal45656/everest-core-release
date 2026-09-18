// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest

#include "InfyPower_BEG1K075G.hpp"

namespace module {

void InfyPower_BEG1K075G::init() {
    acdc = std::make_unique<InfyCanDevice>();
    acdc->set_can_device(config.can_device);
    acdc->set_config_values(config.module_addresses,
                            config.heartbeat_timeout_ms,
                            static_cast<float>(config.overtemperature_limit_C),
                            static_cast<float>(config.per_module_rated_power_W),
                            static_cast<float>(config.conversion_efficiency_export));

    if (!acdc->open()) {
        EVLOG_AND_THROW(EVEXCEPTION(Everest::EverestConfigError,
                                    "Could not open CAN interface ", config.can_device));
    }

    invoke_init(*p_main);
}

void InfyPower_BEG1K075G::ready() {
    invoke_ready(*p_main);
}

} // namespace module
