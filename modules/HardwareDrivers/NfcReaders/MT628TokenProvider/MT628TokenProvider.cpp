// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest

#include "MT628TokenProvider.hpp"

namespace module {

void MT628TokenProvider::init() {
    invoke_init(*p_main);
}

void MT628TokenProvider::ready() {
    invoke_ready(*p_main);
}

} // namespace module
