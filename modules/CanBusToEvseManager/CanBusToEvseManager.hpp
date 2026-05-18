// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#ifndef CAN_BUS_TO_EVSE_MANAGER_HPP
#define CAN_BUS_TO_EVSE_MANAGER_HPP

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 2
//

#include "ld-ev.hpp"

// headers for provided interface implementations
#include <generated/interfaces/can_signal_receiver/Implementation.hpp>

// headers for required interface implementations
#include <generated/interfaces/CANBus/Interface.hpp>

// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1
// insert your custom include headers here
#include <array>
// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1

namespace module {

struct Conf {
    bool log_frames;
};

class CanBusToEvseManager : public Everest::ModuleBase {
public:
    CanBusToEvseManager() = delete;
    CanBusToEvseManager(const ModuleInfo& info,
                        std::unique_ptr<can_signal_receiverImplBase> p_can_signals,
                        std::unique_ptr<CANBusIntf> r_can_bus,
                        Conf& config) :
        ModuleBase(info),
        p_can_signals(std::move(p_can_signals)),
        r_can_bus(std::move(r_can_bus)),
        config(config){};

    const std::unique_ptr<can_signal_receiverImplBase> p_can_signals;
    const std::unique_ptr<CANBusIntf> r_can_bus;
    const Conf& config;

    // ev@1fce4c5e-0ab8-41bb-90f7-14277703d2ac:v1
    // insert your public definitions here
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

#endif // CAN_BUS_TO_EVSE_MANAGER_HPP
