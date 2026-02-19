// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "Hello.hpp"

namespace module {

void Hello::init() {
    invoke_init(*p_main);
}

void Hello::ready() {
    invoke_ready(*p_main);
}

} // namespace module
