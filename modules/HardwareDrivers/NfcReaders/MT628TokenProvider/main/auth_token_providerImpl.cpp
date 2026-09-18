// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest

#include "auth_token_providerImpl.hpp"

#include <everest/logging.hpp>

#include <iomanip>
#include <sstream>

namespace module {
namespace main {

void auth_token_providerImpl::init() {
    serial_.enableDebug(config.debug);

    if (!serial_.open(config.serial_port, config.baud_rate)) {
        if (!this->error_state_monitor->is_error_active("generic/CommunicationFault", "Communication timed out")) {
            auto error = this->error_factory->create_error(
                "generic/CommunicationFault", "Communication timed out",
                "Could not open serial port " + config.serial_port);
            raise_error(error);
        }
        EVLOG_error << "MT628TokenProvider: Failed to open serial port " << config.serial_port;
        return;
    }

    EVLOG_info << "MT628TokenProvider: Opened " << config.serial_port << " at " << config.baud_rate << " baud";
}

void auth_token_providerImpl::ready() {
    if (!serial_.isOpen()) {
        EVLOG_error << "MT628TokenProvider: Serial port not open, cannot start polling";
        return;
    }

    running_ = true;
    poll_thread_ = std::thread([this]() { pollLoop(); });
}

void auth_token_providerImpl::pollLoop() {
    std::string last_uid;
    bool card_was_present = false;

    EVLOG_info << "MT628TokenProvider: Searching for NFC card on " << config.serial_port << "...";

    while (running_) {
        if (serial_.findCard()) {
            std::string uid = serial_.getCardUID();

            if (!uid.empty()) {
                const bool first_detection = !card_was_present;
                card_was_present = true;

                if (first_detection) {
                    EVLOG_info << "MT628TokenProvider: Card detected, UID=" << uid;
                    if (config.buzz_on_card_detected) {
                        serial_.buzz(static_cast<uint8_t>(config.buzz_count),
                                     static_cast<uint16_t>(config.buzz_duration_ms));
                    }
                }

                // Debounce: don't re-publish the same card until it's removed and re-presented
                const auto now = std::chrono::steady_clock::now();
                const auto debounce = std::chrono::milliseconds(config.token_debounce_interval_ms);
                bool same_card = (uid == last_uid);
                bool within_debounce = (now < last_token_submit_ + debounce);

                if (!same_card || !within_debounce) {
                    types::authorization::ProvidedIdToken provided_token;
                    provided_token.id_token = {uid, types::authorization::IdTokenType::ISO14443};
                    provided_token.authorization_type = types::authorization::AuthorizationType::RFID;
                    provided_token.parent_id_token = {uid, types::authorization::IdTokenType::ISO14443};

                    EVLOG_info << "MT628TokenProvider: Publishing token, UID=" << uid;
                    publish_provided_token(provided_token);

                    last_token_submit_ = now;
                    last_uid = uid;
                } else {
                    EVLOG_debug << "MT628TokenProvider: Debouncing same card UID=" << uid;
                }
            } else {
                EVLOG_warning << "MT628TokenProvider: Card found but failed to read UID";
                card_was_present = false;
                last_uid.clear();
            }
        } else {
            if (card_was_present) {
                // Card just removed — log once and reset
                EVLOG_info << "MT628TokenProvider: Card removed, searching for NFC card on "
                           << config.serial_port << "...";
            }
            card_was_present = false;
            last_uid.clear();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(config.read_timeout_ms));
    }

    EVLOG_info << "MT628TokenProvider: Polling stopped";
}

} // namespace main
} // namespace module
