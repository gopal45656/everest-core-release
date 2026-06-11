// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#ifndef CHADEMO_CAN_SIGNAL_RECEIVER_IMPL_HPP
#define CHADEMO_CAN_SIGNAL_RECEIVER_IMPL_HPP

//
// AUTO GENERATED - MARKED REGIONS WILL BE KEPT
// template version 3
//

#include <generated/interfaces/can_signal_receiver/Implementation.hpp>

#include "../Chademo.hpp"

namespace module {
namespace can_signals {

struct Conf {};

class can_signal_receiverImpl : public can_signal_receiverImplBase {
public:
    can_signal_receiverImpl() = delete;
    can_signal_receiverImpl(Everest::ModuleAdapter* ev,
                            const Everest::PtrContainer<Chademo>& mod,
                            Conf& config) :
        can_signal_receiverImplBase(ev, "can_signals"), mod(mod), config(config){};

protected:
    // Nothing to implement — this interface only publishes vars

private:
    const Everest::PtrContainer<Chademo>& mod;
    const Conf& config;

    virtual void init() override;
    virtual void ready() override;
};

} // namespace can_signals
} // namespace module

#endif // CHADEMO_CAN_SIGNAL_RECEIVER_IMPL_HPP
