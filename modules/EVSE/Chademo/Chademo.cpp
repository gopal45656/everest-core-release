// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "Chademo.hpp"

namespace module {

void Chademo::init() {
    invoke_init(*p_main);
    invoke_init(*p_can_signals);
}

void Chademo::ready() {
    invoke_ready(*p_main);
    invoke_ready(*p_can_signals);
}

} // namespace module
