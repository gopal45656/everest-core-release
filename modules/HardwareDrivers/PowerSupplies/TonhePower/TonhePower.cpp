// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest

#include "TonhePower.hpp"

#include <cstdlib>
#include <string>

namespace module {
namespace {

bool bring_up_can_interface(const std::string& ifname) {
    const std::string down_cmd = "ip link set " + ifname + " down";
    const int ret_down = std::system(down_cmd.c_str());
    if (ret_down != 0) {
        EVLOG_warning << "Failed to bring CAN interface " << ifname << " down (exit=" << ret_down << ")";
    }

    const std::string setup_cmd = "ip link set " + ifname + " type can bitrate 125000 fd off";
    const int ret_setup = std::system(setup_cmd.c_str());
    if (ret_setup != 0) {
        EVLOG_error << "Failed to configure CAN interface " << ifname << " (exit=" << ret_setup << ")";
        return false;
    }

    const std::string up_cmd = "ip link set " + ifname + " up";
    const int ret_up = std::system(up_cmd.c_str());
    if (ret_up != 0) {
        EVLOG_error << "Failed to bring CAN interface " << ifname << " up (exit=" << ret_up << ")";
        return false;
    }

    EVLOG_info << "CAN interface " << ifname << " initialized for 125000 bps";
    return true;
}

} // namespace

void TonhePower::init() {
    if (!bring_up_can_interface(config.can_device)) {
        EVLOG_AND_THROW(EVEXCEPTION(Everest::EverestConfigError,
                                    "Could not initialize CAN interface ", config.can_device));
    }

    acdc = std::make_unique<TonheCanDevice>();
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

void TonhePower::ready() {
    invoke_ready(*p_main);
}

} // namespace module
