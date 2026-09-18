// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "EnergyReader.hpp"

namespace module {

EnergyReader::EnergyReader(const ModuleInfo& info,
                            std::unique_ptr<main::energy_readerImpl>        p_main,
                            std::vector<std::unique_ptr<powermeterIntf>>    r_powermeter,
                            std::vector<std::unique_ptr<evse_managerIntf>>  r_evse_manager,
                            main::Conf& config) :
    ModuleBase(info),
    p_main(std::move(p_main)),
    r_powermeter(std::move(r_powermeter)),
    r_evse_manager(std::move(r_evse_manager)),
    config(config) {
}

void EnergyReader::init() {
    invoke_init(*p_main);
}

void EnergyReader::ready() {
    invoke_ready(*p_main);
}

} // namespace module