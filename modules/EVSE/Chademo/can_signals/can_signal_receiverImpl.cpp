// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "can_signal_receiverImpl.hpp"

namespace module {
namespace can_signals {

void can_signal_receiverImpl::init() {
    // Nothing to initialize — this implementation only publishes vars
}

void can_signal_receiverImpl::ready() {
    // Nothing to do on ready — publishing is driven by the parent module
}

} // namespace can_signals
} // namespace module
