// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#ifndef CAN_BUS_CANBUS_IMPL_HPP
#define CAN_BUS_CANBUS_IMPL_HPP

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 3
//

#include <generated/interfaces/CANBus/Implementation.hpp>

#include "../CanBus.hpp"

// ev@75ac1216-19eb-4182-a85c-820f1fc2c091:v1
// insert your custom include headers here
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
// ev@75ac1216-19eb-4182-a85c-820f1fc2c091:v1

namespace module {
namespace can_bus {

struct Conf {};

class CANBusImpl : public CANBusImplBase {
public:
    CANBusImpl() = delete;
    CANBusImpl(Everest::ModuleAdapter* ev, const Everest::PtrContainer<CanBus>& mod, Conf& config) :
        CANBusImplBase(ev, "can_bus"), mod(mod), config(config){};

    // ev@8ea32d28-373f-4c90-ae5e-b4fcc74e2a61:v1
    // insert your public definitions here
    // ev@8ea32d28-373f-4c90-ae5e-b4fcc74e2a61:v1

protected:
    // command handler functions (virtual)
    virtual bool handle_open(std::string& ifname) override;
    virtual void handle_close() override;
    virtual bool handle_send(int& id, bool& extended, int& dlc, Array& data) override;
    virtual Object handle_receive() override;

    // ev@d2d1847a-7b88-41dd-ad07-92785f06f5c4:v1
    // insert your protected definitions here
    // ev@d2d1847a-7b88-41dd-ad07-92785f06f5c4:v1

private:
    const Everest::PtrContainer<CanBus>& mod;
    const Conf& config;

    virtual void init() override;
    virtual void ready() override;

    // ev@3370e4dd-95f4-47a9-aaec-ea76f34a66c9:v1
    // insert your private definitions here
    int socket_fd = -1;
    bool is_open = false;

    std::thread rx_thread;
    std::atomic<bool> running{false};
    // ev@3370e4dd-95f4-47a9-aaec-ea76f34a66c9:v1
};

// ev@3d7da0ad-02c2-493d-9920-0bbbd56b9876:v1
// insert other definitions here
// ev@3d7da0ad-02c2-493d-9920-0bbbd56b9876:v1

} // namespace can_bus
} // namespace module

#endif // CAN_BUS_CANBUS_IMPL_HPP
