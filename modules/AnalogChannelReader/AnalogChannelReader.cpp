// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#include "AnalogChannelReader.hpp"

namespace module {

void AnalogChannelReader::init() {
    invoke_init(*p_analog_reader);
}

void AnalogChannelReader::ready() {
    invoke_ready(*p_analog_reader);
}

} // namespace module
