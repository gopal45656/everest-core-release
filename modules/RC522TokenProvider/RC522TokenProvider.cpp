// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "RC522TokenProvider.hpp"

namespace module {

void RC522TokenProvider::init() {
    invoke_init(*p_main);
}

void RC522TokenProvider::ready() {
    invoke_ready(*p_main);
}

} // namespace module
