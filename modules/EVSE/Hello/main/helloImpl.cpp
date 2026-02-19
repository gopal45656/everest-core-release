// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "helloImpl.hpp"
#include <iostream>

namespace module {
namespace main {

void helloImpl::init() {
}

void helloImpl::ready() {
	std::cout << "Hello World" << std::endl;
}

} // namespace main
} // namespace module
