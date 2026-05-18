// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "CanBus.hpp"

namespace module {

void CanBus::init() {
    invoke_init(*p_can_bus);
}

void CanBus::ready() {
    invoke_ready(*p_can_bus);
}

} // namespace module
