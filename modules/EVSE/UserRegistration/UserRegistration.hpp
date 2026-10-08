// SPDX-License-Identifier: Apache-2.0

#ifndef USER_REGISTRATION_HPP
#define USER_REGISTRATION_HPP

#include "ld-ev.hpp"

#include <generated/interfaces/empty/Implementation.hpp>
#include <generated/interfaces/auth_token_provider/Interface.hpp>

#include <memory>
#include <vector>

namespace module {

struct Conf {};

class UserRegistration : public Everest::ModuleBase {
public:
    UserRegistration() = delete;
    UserRegistration(const ModuleInfo& info, std::unique_ptr<emptyImplBase> p_main,
                     std::unique_ptr<auth_token_providerIntf> r_token_provider,
                     Conf& config);

    const std::unique_ptr<emptyImplBase> p_main;
    const std::unique_ptr<auth_token_providerIntf> r_token_provider;
    Conf& config;

private:
    friend class LdEverest;
    void init();
    void ready();
};

} // namespace module

#endif // USER_REGISTRATION_HPP