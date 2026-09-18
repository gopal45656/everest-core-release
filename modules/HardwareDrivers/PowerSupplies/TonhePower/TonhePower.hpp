// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
#ifndef TONHE_POWER_HPP
#define TONHE_POWER_HPP

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 2
//

#include "ld-ev.hpp"

// headers for provided interface implementations
#include <generated/interfaces/power_supply_DC/Implementation.hpp>
#include <generated/interfaces/evse_manager/Interface.hpp>

// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1
#include "can_driver_acdc/TonheCanDevice.hpp"
// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1

namespace module {

struct Conf {
    std::string can_device;
    std::string module_addresses;
    int         heartbeat_timeout_ms;
    double      overtemperature_limit_C;
    double      conversion_efficiency_export;
    double      per_module_rated_power_W;
};

class TonhePower : public Everest::ModuleBase {
public:
    TonhePower() = delete;
    TonhePower(const ModuleInfo& info, std::unique_ptr<power_supply_DCImplBase> p_main,
               std::unique_ptr<evse_managerIntf> r_evse_manager, Conf& config) :
        ModuleBase(info), p_main(std::move(p_main)), r_evse_manager(std::move(r_evse_manager)), config(config){};

    const std::unique_ptr<power_supply_DCImplBase> p_main;
    const std::unique_ptr<evse_managerIntf> r_evse_manager;
    const Conf& config;

    // ev@1fce4c5e-0ab8-41bb-90f7-14277703d2ac:v1
    std::unique_ptr<TonheCanDevice> acdc;
    // ev@1fce4c5e-0ab8-41bb-90f7-14277703d2ac:v1

protected:
    // ev@4714b2ab-a24f-4b95-ab81-36439e1478de:v1
    // insert your protected definitions here
    // ev@4714b2ab-a24f-4b95-ab81-36439e1478de:v1

private:
    friend class LdEverest;
    void init();
    void ready();

    // ev@211cfdbe-f69a-4cd6-a4ec-f8aaa3d1b6c8:v1
    // insert your private definitions here
    // ev@211cfdbe-f69a-4cd6-a4ec-f8aaa3d1b6c8:v1
};

// ev@087e516b-124c-48df-94fb-109508c7cda9:v1
// insert other definitions here
// ev@087e516b-124c-48df-94fb-109508c7cda9:v1

} // namespace module

#endif // TONHE_POWER_HPP
