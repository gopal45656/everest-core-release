// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>

extern "C" {
#include "gbt27930.h"
}

// EVerest framework types used for CAN send — these are already defined by
// the framework (utils/types.hpp) as json::array_t and json::object_t.
// Do NOT redefine them here.

namespace module {

/// GB/T 27930 charger-side state machine.
///
/// Call on_frame() for every received CAN frame.
/// Provide a send_fn callback to transmit CAN frames.
///
/// State sequence (charger side):
///   Idle -> Handshake -> Recognition -> Config ->
///   Ready -> Charging -> Stopping -> Finished
class GbtProtocol {
public:
    enum class State {
        Idle,        // Waiting for BHM from BMS
        Handshake,   // Exchanging CHM / BRM
        Recognition, // CRM / BCP exchange
        Config,      // CTS, CML, CRO / BRO exchange
        Charging,    // CCS loop, BCL / BCS
        Stopping,    // BST / CST exchange
        Finished,    // BSD / CSD exchange, session done
    };

    using SendFn = std::function<void(uint32_t id, bool extended, int dlc,
                                     const std::array<uint8_t, 8>& data)>;

    explicit GbtProtocol(SendFn send_fn) : send_(std::move(send_fn)) {}

    /// Feed a received CAN frame into the state machine.
    void on_frame(uint32_t id, bool extended, int dlc, const uint8_t* data) {
        switch (id) {
        case GBT27930_BHM_FRAME_ID: handle_bhm(data, dlc); break;
        case GBT27930_BRM_FRAME_ID: handle_brm(data, dlc); break;
        case GBT27930_BCP_FRAME_ID: handle_bcp(data, dlc); break;
        case GBT27930_BRO_FRAME_ID: handle_bro(data, dlc); break;
        case GBT27930_BCL_FRAME_ID: handle_bcl(data, dlc); break;
        case GBT27930_BCS_FRAME_ID: handle_bcs(data, dlc); break;
        case GBT27930_BSM_FRAME_ID: handle_bsm(data, dlc); break;
        case GBT27930_BST_FRAME_ID: handle_bst(data, dlc); break;
        case GBT27930_BSD_FRAME_ID: handle_bsd(data, dlc); break;
        case GBT27930_BEM_FRAME_ID: handle_bem(data, dlc); break;
        default: break;
        }
    }

    State state() const { return state_; }

    const std::string& state_name() const {
        static const std::string names[] = {
            "Idle", "Handshake", "Recognition",
            "Config", "Charging", "Stopping", "Finished"
        };
        return names[static_cast<int>(state_)];
    }

    // Charger parameters — set before session starts
    double charger_max_voltage_V{1000.0};
    double charger_max_current_A{250.0};
    uint8_t charger_type{1};           // 1 = off-board DC
    double output_voltage_V{0.0};
    double output_current_A{0.0};
    double charger_temp_C{25.0};

    // Last decoded BMS values (read-only from outside)
    struct BmsInfo {
        double battery_voltage_V{0};
        double battery_current_A{0};
        uint8_t soc{0};
        double battery_temp_C{0};
        double requested_voltage_V{0};
        double requested_current_A{0};
        uint8_t charge_mode{0};
        double max_charge_voltage_V{0};
        double max_charge_current_A{0};
    } bms;

private:
    State state_{State::Idle};
    SendFn send_;

    // ------------------------------------------------------------------ //
    // Helpers
    // ------------------------------------------------------------------ //

    void send_frame(uint32_t id, bool extended, int dlc,
                    const std::array<uint8_t, 8>& buf) {
        send_(id, extended, dlc, buf);
    }

    template <typename PackFn, typename Msg>
    void send_msg(uint32_t id, bool extended, PackFn pack_fn, const Msg& msg) {
        std::array<uint8_t, 8> buf{};
        pack_fn(buf.data(), &msg, 8);
        send_frame(id, extended, 8, buf);
    }

    void transition(State next) {
        state_ = next;
    }

    // ------------------------------------------------------------------ //
    // BMS message handlers
    // ------------------------------------------------------------------ //

    // BHM — BMS Handshake Message: BMS announces itself
    void handle_bhm(const uint8_t* data, int dlc) {
        if (state_ != State::Idle && state_ != State::Handshake) return;

        gbt27930_bhm_t bhm{};
        gbt27930_bhm_unpack(&bhm, data, dlc);

        EVLOG_info << "[GBT27930] BHM received: bms_version=" << (int)bhm.bms_version
                   << " capacity=" << gbt27930_bhm_battery_capacity_decode(bhm.battery_capacity) << "Ah"
                   << " nominal_voltage=" << gbt27930_bhm_nominal_voltage_decode(bhm.nominal_voltage) << "V"
                   << " battery_type=" << (int)bhm.battery_type;

        transition(State::Handshake);
        send_chm();
    }

    // BRM — Battery Recognition Message (optional, BMS may skip it)
    void handle_brm(const uint8_t* data, int dlc) {
        if (state_ != State::Handshake) return;

        gbt27930_brm_t brm{};
        gbt27930_brm_unpack(&brm, data, dlc);

        EVLOG_info << "[GBT27930] BRM received: series=" << brm.series_cells
                   << " parallel=" << brm.parallel_cells
                   << " capacity=" << gbt27930_brm_pack_capacity_decode(brm.pack_capacity) << "Ah";

        transition(State::Recognition);
        send_crm(true);
    }

    // BCP — Battery Charge Parameters
    void handle_bcp(const uint8_t* data, int dlc) {
        // Accept BCP in Handshake too — some BMS implementations skip BRM
        if (state_ != State::Recognition && state_ != State::Handshake) return;

        // If BRM was skipped, send CRM now before processing BCP
        if (state_ == State::Handshake) {
            EVLOG_warning << "[GBT27930] BRM not received, BMS skipped to BCP — sending CRM anyway";
            send_crm(true);
        }

        gbt27930_bcp_t bcp{};
        gbt27930_bcp_unpack(&bcp, data, dlc);

        bms.max_charge_voltage_V = gbt27930_bcp_max_charge_voltage_decode(bcp.max_charge_voltage);
        bms.max_charge_current_A = gbt27930_bcp_max_charge_current_decode(bcp.max_charge_current);

        EVLOG_info << "[GBT27930] BCP received: max_voltage=" << bms.max_charge_voltage_V << "V"
                   << " max_current=" << bms.max_charge_current_A << "A"
                   << " energy=" << gbt27930_bcp_energy_capacity_decode(bcp.energy_capacity) << "kWh";

        transition(State::Config);
        send_cts();
        send_cml();
        // Send CRO immediately — some BMS implementations skip BRO entirely
        // and go straight to BCL after BCP. Transitioning to Charging here
        // ensures we respond to BCL correctly.
        send_cro(true);
        transition(State::Charging);
        EVLOG_info << "[GBT27930] Auto-transitioning to Charging (BRO not required by this BMS)";
    }

    // BRO — BMS Ready for Output
    void handle_bro(const uint8_t* data, int dlc) {
        // Already handled by auto-transition in handle_bcp for BMS that skip BRO.
        // If BRO does arrive (strict BMS), confirm CRO again.
        if (state_ == State::Charging) return;  // already there
        if (state_ != State::Config) return;

        gbt27930_bro_t bro{};
        gbt27930_bro_unpack(&bro, data, dlc);

        EVLOG_info << "[GBT27930] BRO received: bms_ready=" << (int)bro.bms_ready;

        if (bro.bms_ready == 1) {
            transition(State::Charging);
            send_cro(true);
        } else {
            send_cro(false);
        }
    }

    // BCL — Battery Charge Demand (voltage/current request during charging)
    void handle_bcl(const uint8_t* data, int dlc) {
        // Accept BCL in Config state too — BMS may start sending before BRO
        if (state_ != State::Charging && state_ != State::Config) return;

        gbt27930_bcl_t bcl{};
        gbt27930_bcl_unpack(&bcl, data, dlc);

        bms.requested_voltage_V = gbt27930_bcl_requested_voltage_decode(bcl.requested_voltage);
        bms.requested_current_A = gbt27930_bcl_requested_current_decode(bcl.requested_current);
        bms.charge_mode = bcl.charge_mode;

        EVLOG_info << "[GBT27930] BCL: req_voltage=" << bms.requested_voltage_V << "V"
                   << " req_current=" << bms.requested_current_A << "A"
                   << " mode=" << (int)bms.charge_mode;

        // Mirror the BMS request back as our output — clamped to charger limits
        output_voltage_V = std::min(bms.requested_voltage_V, charger_max_voltage_V);
        output_current_A = std::min(bms.requested_current_A, charger_max_current_A);

        // Reply with current charger output status
        send_ccs();
    }

    // BCS — Battery Charge Status
    void handle_bcs(const uint8_t* data, int dlc) {
        // Accept BCS in Config state too — BMS may start sending before BRO
        if (state_ != State::Charging && state_ != State::Config) return;

        gbt27930_bcs_t bcs{};
        gbt27930_bcs_unpack(&bcs, data, dlc);

        bms.battery_voltage_V = gbt27930_bcs_battery_voltage_decode(bcs.battery_voltage);
        bms.battery_current_A = gbt27930_bcs_battery_current_decode(bcs.battery_current);
        bms.soc = bcs.soc;
        bms.battery_temp_C = gbt27930_bcs_battery_temp_decode(bcs.battery_temp);

        EVLOG_info << "[GBT27930] BCS: voltage=" << bms.battery_voltage_V << "V"
                   << " current=" << bms.battery_current_A << "A"
                   << " SoC=" << (int)bms.soc << "%"
                   << " temp=" << bms.battery_temp_C << "C";
    }

    // BSM — Battery Status Message (temperatures)
    void handle_bsm(const uint8_t* data, int dlc) {
        gbt27930_bsm_t bsm{};
        gbt27930_bsm_unpack(&bsm, data, dlc);

        EVLOG_info << "[GBT27930] BSM: max_temp=" << gbt27930_bsm_max_temp_decode(bsm.max_temp) << "C"
                   << " min_temp=" << gbt27930_bsm_min_temp_decode(bsm.min_temp) << "C";
    }

    // BST — BMS Stop Transmission
    void handle_bst(const uint8_t* data, int dlc) {
        if (state_ != State::Charging) return;

        gbt27930_bst_t bst{};
        gbt27930_bst_unpack(&bst, data, dlc);

        EVLOG_info << "[GBT27930] BST received: stop_flag=" << (int)bst.stop_flag
                   << " fault_code=0x" << std::hex << bst.fault_code;

        transition(State::Stopping);
        send_cst(0, 0);  // charger normal stop, no fault
    }

    // BSD — BMS Statistics Data (end of session)
    void handle_bsd(const uint8_t* data, int dlc) {
        if (state_ != State::Stopping) return;

        gbt27930_bsd_t bsd{};
        gbt27930_bsd_unpack(&bsd, data, dlc);

        EVLOG_info << "[GBT27930] BSD received: discharge_time=" << bsd.discharge_time << "s";

        transition(State::Finished);
        send_csd(static_cast<uint16_t>(bsd.discharge_time));

        EVLOG_info << "[GBT27930] Session finished.";
    }

    // BEM — BMS Error Message
    void handle_bem(const uint8_t* data, int dlc) {
        gbt27930_bem_t bem{};
        gbt27930_bem_unpack(&bem, data, dlc);

        EVLOG_error << "[GBT27930] BEM error: code=0x" << std::hex << bem.error_code;

        // Abort charging if active
        if (state_ == State::Charging) {
            transition(State::Stopping);
            send_cst(1, bem.error_code);
        }
    }

    // ------------------------------------------------------------------ //
    // Charger transmit helpers
    // ------------------------------------------------------------------ //

    // CHM — Charger Handshake Message
    void send_chm() {
        gbt27930_chm_t msg{};
        msg.charger_max_voltage = gbt27930_chm_charger_max_voltage_encode(charger_max_voltage_V);
        msg.charger_max_current = gbt27930_chm_charger_max_current_encode(charger_max_current_A);
        msg.charger_type = charger_type;
        send_msg(GBT27930_CHM_FRAME_ID, true, gbt27930_chm_pack, msg);
        EVLOG_info << "[GBT27930] TX CHM: max_voltage=" << charger_max_voltage_V
                   << "V max_current=" << charger_max_current_A << "A";
    }

    // CRM — Charger Recognition Message
    void send_crm(bool ready) {
        gbt27930_crm_t msg{};
        msg.charger_ready = ready ? 1 : 0;
        msg.charger_status = 1;  // normal
        send_msg(GBT27930_CRM_FRAME_ID, true, gbt27930_crm_pack, msg);
        EVLOG_info << "[GBT27930] TX CRM: ready=" << ready;
    }

    // CTS — Charger Time Sync (estimated charge time)
    void send_cts() {
        gbt27930_cts_t msg{};
        msg.estimated_charge_time = 3600;  // 1 hour estimate
        send_msg(GBT27930_CTS_FRAME_ID, true, gbt27930_cts_pack, msg);
        EVLOG_info << "[GBT27930] TX CTS: estimated_time=3600s";
    }

    // CML — Charger Maximum Limits
    void send_cml() {
        gbt27930_cml_t msg{};
        msg.charger_mode = 1;  // constant current
        send_msg(GBT27930_CML_FRAME_ID, true, gbt27930_cml_pack, msg);
        EVLOG_info << "[GBT27930] TX CML: mode=1 (CC)";
    }

    // CRO — Charger Ready for Output
    void send_cro(bool ready) {
        gbt27930_cro_t msg{};
        msg.charger_ready_to_charge = ready ? 1 : 0;
        send_msg(GBT27930_CRO_FRAME_ID, true, gbt27930_cro_pack, msg);
        EVLOG_info << "[GBT27930] TX CRO: ready=" << ready;
    }

    // CCS — Charger Charge Status (sent in response to BCL)
    void send_ccs() {
        gbt27930_ccs_t msg{};
        msg.output_voltage = gbt27930_ccs_output_voltage_encode(output_voltage_V);
        msg.output_current = gbt27930_ccs_output_current_encode(output_current_A);
        msg.charger_temp = gbt27930_ccs_charger_temp_encode(charger_temp_C);
        msg.charger_status = 1;  // charging
        send_msg(GBT27930_CCS_FRAME_ID, true, gbt27930_ccs_pack, msg);
        EVLOG_info << "[GBT27930] TX CCS: voltage=" << output_voltage_V
                   << "V current=" << output_current_A << "A";
    }

    // CST — Charger Stop Transmission
    void send_cst(uint8_t stop_reason, uint16_t fault_code) {
        gbt27930_cst_t msg{};
        msg.stop_reason = stop_reason;
        msg.fault_code = fault_code;
        send_msg(GBT27930_CST_FRAME_ID, true, gbt27930_cst_pack, msg);
        EVLOG_info << "[GBT27930] TX CST: stop_reason=" << (int)stop_reason
                   << " fault=0x" << std::hex << fault_code;
    }

    // CSD — Charger Statistics Data
    void send_csd(uint16_t charge_time_s) {
        gbt27930_csd_t msg{};
        msg.discharge_time = charge_time_s;
        send_msg(GBT27930_CSD_FRAME_ID, true, gbt27930_csd_pack, msg);
        EVLOG_info << "[GBT27930] TX CSD: charge_time=" << charge_time_s << "s";
    }
};

} // namespace module
