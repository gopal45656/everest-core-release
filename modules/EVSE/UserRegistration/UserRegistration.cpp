// SPDX-License-Identifier: Apache-2.0

#include "UserRegistration.hpp"

namespace module {

UserRegistration::UserRegistration(const ModuleInfo& info,
                                   std::unique_ptr<emptyImplBase> p_main,
                                   std::unique_ptr<auth_token_providerIntf> r_token_provider,
                                   Conf& config) :
    ModuleBase(info),
    p_main(std::move(p_main)),
    r_token_provider(std::move(r_token_provider)),
    config(config) {
}

void UserRegistration::init() {
    invoke_init(*p_main);
}

void UserRegistration::ready() {
    invoke_ready(*p_main);
}

} // namespace module