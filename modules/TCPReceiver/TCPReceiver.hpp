// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#ifndef TCP_RECEIVER_HPP
#define TCP_RECEIVER_HPP

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 2
//

#include "ld-ev.hpp"

// headers for provided interface implementations
#include <generated/interfaces/BinarySignal/Implementation.hpp>

// headers for required interface implementations
#include <generated/interfaces/auth/Interface.hpp>

// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1
// insert your custom include headers here
#include <atomic>
#include <thread>
#include <string>
#include <netinet/in.h>
#include <arpa/inet.h>
// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1

namespace module {

struct Conf {
    std::string host;
    int port;
    std::string socket_mode;
    int reconnect_interval_ms;
};

class TCPReceiver : public Everest::ModuleBase {
public:
    TCPReceiver() = delete;
    TCPReceiver(const ModuleInfo& info, std::unique_ptr<BinarySignalImplBase> p_main,
                   std::unique_ptr<authIntf> r_auth, Conf& config) :
        ModuleBase(info), p_main(std::move(p_main)), r_auth(std::move(r_auth)), config(config){};

    ~TCPReceiver() override;
    
    const std::unique_ptr<BinarySignalImplBase> p_main;
    const std::unique_ptr<authIntf> r_auth;
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
    // UNIX socket server state
    std::thread server_thread;
    std::atomic<bool> running{true};
    // ev@211cfdbe-f69a-4cd6-a4ec-f8aaa3d1b6c8:v1
};

// ev@087e516b-124c-48df-94fb-109508c7cda9:v1
// insert other definitions here
// ev@087e516b-124c-48df-94fb-109508c7cda9:v1

} // namespace module

#endif // TCP_RECEIVER_HPP
