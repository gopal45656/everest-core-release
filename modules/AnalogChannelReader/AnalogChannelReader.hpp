// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#ifndef ANALOG_CHANNEL_READER_HPP
#define ANALOG_CHANNEL_READER_HPP

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 2
//

#include "ld-ev.hpp"

// headers for provided interface implementations
#include <generated/interfaces/analog_reader/Implementation.hpp>

// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1
// insert your custom include headers here
// ev@4bf81b14-a215-475c-a1d3-0a484ae48918:v1

namespace module {

struct Conf {
    std::string serial_port;
    int baudrate;
    std::string parity;
    int data_bits;
    int stop_bits;
    int polling_interval_ms;
    double reference_voltage;
};

class AnalogChannelReader : public Everest::ModuleBase {
public:
    AnalogChannelReader() = delete;
    AnalogChannelReader(const ModuleInfo& info, std::unique_ptr<analog_readerImplBase> p_analog_reader, Conf& config) :
        ModuleBase(info), p_analog_reader(std::move(p_analog_reader)), config(config){};

    const std::unique_ptr<analog_readerImplBase> p_analog_reader;
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

#endif // ANALOG_CHANNEL_READER_HPP
