// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "BinarySignalImpl.hpp"

namespace module {
namespace main {

void BinarySignalImpl::init() {
}

void BinarySignalImpl::ready() {
}

void BinarySignalImpl::publish_signal(const std::string& value) {
    int v = std::stoi(value);
    publish_value(v);
}

} // namespace main
} // namespace module
