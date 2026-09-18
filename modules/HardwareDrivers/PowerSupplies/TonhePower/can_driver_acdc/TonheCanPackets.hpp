// SPDX-License-Identifier: Apache-2.0
// Copyright 2020 - 2025 Pionix GmbH and Contributors to EVerest
//
// Tonhe CAN protocol packet definitions
// Protocol: THJS-TXXY-0060 V1.2  (SAE J1939-based, 125 kbps, 29-bit extended)
// Source: tonhe_can_messages_6modules.csv
//
// Byte order: Little-endian (Intel format) throughout
//
// CAN ID structure for this protocol (from CSV):
//
//   Uplink  (M_C_x, module → master):
//     0x1801aNNNN   where NN = module address (01..06 for 6 modules)
//     Format: (priority_pgnh << 24) | (pgn_low << 16) | (MASTER_ADDR << 8) | mod_addr
//
//   Downlink (C_M_x, master → module):
//     Broadcast: 0xNNNNffA0   (dst = 0xFF, src = MASTER_ADDR = 0xA0)
//     Unicast:   0xNNNNmmA0   (dst = mod_addr, src = MASTER_ADDR = 0xA0)
//
// Module addresses are NOT pre-configured — they are auto-discovered from
// incoming M_C_1 uplink frames.  The max address is 240 per spec.
//
// Master (controller) address: 0xA0
// Broadcast address:           0xFF

#ifndef TONHE_CAN_PACKETS_HPP
#define TONHE_CAN_PACKETS_HPP

#include <cstdint>
#include <vector>

namespace tonhe {

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
constexpr uint8_t MASTER_ADDR    = 0xA0;
constexpr uint8_t BROADCAST_ADDR = 0xFF;

// Start/Stop values
constexpr uint8_t CMD_STOP  = 0x55;
constexpr uint8_t CMD_START = 0xAA;

// ---------------------------------------------------------------------------
// CAN ID helpers — derived directly from CSV values
//
// Uplink IDs (module → master):
//   M_C_1_ModN  0x1801aNNN  = (0x18<<24)|(0x01<<16)|(0xA0<<8)|N
//   M_C_2_ModN  0x0802aNNN  = (0x08<<24)|(0x02<<16)|(0xA0<<8)|N
//   M_C_3_ModN  0x180baNNN  = (0x18<<24)|(0x0B<<16)|(0xA0<<8)|N
//   M_C_4_ModN  0x1c91aNNN  = (0x1C<<24)|(0x91<<16)|(0xA0<<8)|N
//
// Downlink broadcast IDs (master → all modules):
//   C_M_1       0x0803ffa0  = (0x08<<24)|(0x03<<16)|(0xFF<<8)|0xA0
//   C_M_2       0x1004ffa0  = (0x10<<24)|(0x04<<16)|(0xFF<<8)|0xA0
//   C_M_3       0x1805ffa0  = (0x18<<24)|(0x05<<16)|(0xFF<<8)|0xA0
//   C_M_4       0x18aaffa0  = (0x18<<24)|(0xAA<<16)|(0xFF<<8)|0xA0
//   C_M_12      0x1c90ffa0  = (0x1C<<24)|(0x90<<16)|(0xFF<<8)|0xA0
//   C_M_23_bcast 0x1809ffa0 = (0x18<<24)|(0x09<<16)|(0xFF<<8)|0xA0
//
// Downlink unicast IDs (master → specific module N):
//   C_M_23_ModN 0x18090Na0  = (0x18<<24)|(0x09<<16)|(N<<8)|0xA0
//   C_M_24_ModN 0x08060Na0  = (0x08<<24)|(0x06<<16)|(N<<8)|0xA0
// ---------------------------------------------------------------------------

// Extract module address from an uplink CAN ID (bits 0-7 = source address)
inline uint8_t mod_addr_from_uplink_id(uint32_t raw_id) {
    return raw_id & 0xFF;
}

// Extract destination address from a downlink CAN ID (bits 8-15)
inline uint8_t dst_addr_from_downlink_id(uint32_t raw_id) {
    return (raw_id >> 8) & 0xFF;
}

// Extract PGN word (bits 16-31) from any CAN ID
inline uint16_t pgn_from_id(uint32_t raw_id) {
    return static_cast<uint16_t>((raw_id >> 16) & 0xFFFF);
}

// Build uplink base (without module address) — or with address
inline uint32_t mc1_base() { return (0x18u<<24)|(0x01u<<16)|(MASTER_ADDR<<8); }
inline uint32_t mc2_base() { return (0x08u<<24)|(0x02u<<16)|(MASTER_ADDR<<8); }
inline uint32_t mc3_base() { return (0x18u<<24)|(0x0Bu<<16)|(MASTER_ADDR<<8); }
inline uint32_t mc4_base() { return (0x1Cu<<24)|(0x91u<<16)|(MASTER_ADDR<<8); }

inline uint32_t mc1_id(uint8_t mod) { return mc1_base() | mod; }
inline uint32_t mc2_id(uint8_t mod) { return mc2_base() | mod; }
inline uint32_t mc3_id(uint8_t mod) { return mc3_base() | mod; }
inline uint32_t mc4_id(uint8_t mod) { return mc4_base() | mod; }

// Build downlink broadcast IDs (exact values from CSV)
inline uint32_t cm1_id()  { return 0x0803FFa0u; }   // C_M_1  broadcast
inline uint32_t cm2_id()  { return 0x1004FFa0u; }   // C_M_2  broadcast
inline uint32_t cm3_id()  { return 0x1805FFa0u; }   // C_M_3  broadcast (timing/keepalive)
inline uint32_t cm4_id()  { return 0x18AAFFa0u; }   // C_M_4  broadcast (input mode)
inline uint32_t cm12_id() { return 0x1C90FFa0u; }   // C_M_12 broadcast (addr mode)
inline uint32_t cm23_broadcast_id() { return 0x1809FFa0u; }  // C_M_23 broadcast

// Build downlink unicast IDs (destination = module address)
inline uint32_t cm23_id(uint8_t mod) { return (0x18u<<24)|(0x09u<<16)|(uint32_t(mod)<<8)|MASTER_ADDR; }
inline uint32_t cm24_id(uint8_t mod) { return (0x08u<<24)|(0x06u<<16)|(uint32_t(mod)<<8)|MASTER_ADDR; }

// PGN values for uplink identification (bits 16-31 of raw_id)
constexpr uint16_t PGN_MC1 = 0x1801u;
constexpr uint16_t PGN_MC2 = 0x0802u;
constexpr uint16_t PGN_MC3 = 0x180Bu;
constexpr uint16_t PGN_MC4 = 0x1C91u;

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------

inline uint16_t le16(const uint8_t* b) {
    return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
}
inline void put_le16(uint8_t* b, uint16_t v) {
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF;
}

// ---------------------------------------------------------------------------
// M_C_1: Charging Module State (CSV: M_C_1_ModN, 0x1801aNNN)
//
//   Byte 0     : ChargingModuleState  (0x00=Normal OFF, 0x01=ON, 0x11=Fault OFF)
//   Bytes 1-2  : OutputVoltage        (LE u16, × 0.1 V, range 0–1100 V)
//   Bytes 3-4  : OutputCurrent        (LE u16, × 0.01 A, range 0–200 A)
//   Byte 5     : Fault bits 0-7       (input undervoltage .. hardware fault)
//   Byte 6     : Fault bits 8-15      (bus exception .. short circuit)
//   Byte 7     : PFC fault bits 0-7   (input overcurrent .. bus overvoltage)
// ---------------------------------------------------------------------------
struct MC1Frame {
    enum class State : uint8_t { NormalOff=0x00, On=0x01, FaultOff=0x11 };

    State    state{State::NormalOff};
    float    voltage_V{0.f};
    float    current_A{0.f};
    uint8_t  fault_lo{0};   // bits 0-7
    uint8_t  fault_hi{0};   // bits 8-15
    uint8_t  pfc_fault{0};  // bits 0-7

    // Fault accessors (CSV StartBit offsets from byte 5 = bit 40)
    bool flt_input_undervoltage()    const { return fault_lo  & (1u<<0); }
    bool flt_input_phase_loss()      const { return fault_lo  & (1u<<1); }
    bool flt_input_overvoltage()     const { return fault_lo  & (1u<<2); }
    bool flt_output_overvoltage()    const { return fault_lo  & (1u<<3); }
    bool flt_output_overcurrent()    const { return fault_lo  & (1u<<4); }
    bool flt_temperature_high()      const { return fault_lo  & (1u<<5); }
    bool flt_fan_fault()             const { return fault_lo  & (1u<<6); }
    bool flt_hardware_fault()        const { return fault_lo  & (1u<<7); }
    bool flt_bus_exception()         const { return fault_hi  & (1u<<0); }
    bool flt_short_circuit()         const { return fault_hi  & (1u<<7); }
    bool pfc_address_conflict()      const { return pfc_fault & (1u<<4); }
    bool pfc_bus_overvoltage()       const { return pfc_fault & (1u<<7); }
    bool any_fault() const {
        return fault_lo & 0b10111000u;  // output OV, OC, temp, HW fault
    }

    static MC1Frame from_bytes(const uint8_t* d) {
        MC1Frame f;
        f.state     = static_cast<State>(d[0]);
        f.voltage_V = le16(d+1) * 0.1f;
        f.current_A = le16(d+3) * 0.01f;
        f.fault_lo  = d[5];
        f.fault_hi  = d[6];
        f.pfc_fault = d[7];
        return f;
    }
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(8, 0);
        b[0] = static_cast<uint8_t>(state);
        put_le16(b.data()+1, static_cast<uint16_t>(voltage_V / 0.1f));
        put_le16(b.data()+3, static_cast<uint16_t>(current_A / 0.01f));
        b[5] = fault_lo; b[6] = fault_hi; b[7] = pfc_fault;
        return b;
    }
};

// ---------------------------------------------------------------------------
// M_C_2: Start/Stop Acknowledgement (CSV: M_C_2_ModN, 0x0802aNNN)
//   Byte 0: 0x00=not received, 0x01=received
//   Bytes 1-7: 0x00
// ---------------------------------------------------------------------------
struct MC2Frame {
    bool received{false};
    static MC2Frame from_bytes(const uint8_t* d) { return {d[0]==0x01}; }
    std::vector<uint8_t> to_bytes() const {
        return {received ? uint8_t(0x01) : uint8_t(0x00), 0,0,0,0,0,0,0};
    }
};

// ---------------------------------------------------------------------------
// M_C_3: AC Input + Temperature (CSV: M_C_3_ModN, 0x180baNNN)
//   Bytes 0-1: A-phase voltage (× 0.1 V, LE)
//   Bytes 2-3: B-phase voltage
//   Bytes 4-5: C-phase voltage
//   Bytes 6-7: Ambient temperature (× 1 °C, LE)
// ---------------------------------------------------------------------------
struct MC3Frame {
    float a_phase_V{0.f}, b_phase_V{0.f}, c_phase_V{0.f};
    float ambient_temp_C{0.f};
    static MC3Frame from_bytes(const uint8_t* d) {
        return {le16(d+0)*0.1f, le16(d+2)*0.1f, le16(d+4)*0.1f,
                static_cast<float>(le16(d+6))};
    }
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(8, 0);
        put_le16(b.data()+0, static_cast<uint16_t>(a_phase_V/0.1f));
        put_le16(b.data()+2, static_cast<uint16_t>(b_phase_V/0.1f));
        put_le16(b.data()+4, static_cast<uint16_t>(c_phase_V/0.1f));
        put_le16(b.data()+6, static_cast<uint16_t>(ambient_temp_C));
        return b;
    }
};

// ---------------------------------------------------------------------------
// M_C_4: Extended State/Fault (CSV: M_C_4_ModN, 0x1c91aNNN)
//   Bytes 0-1: Module state bits (LE u16)
//     Bit0: current equalization, Bit1: mute, Bit2: E2 fault overflow
//     Bit3: DC input source, Bit4: E2 fault disabled, Bit5: hot-plug enabled
//   Bytes 2-3: Fault/warning bits (LE u16)
//     Bit0: pre-level wave stop, Bit1: hot-plug fault, Bit2: CAN timeout
//     Bit4: relay fault, Bit6: internal overtemp, Bit7: air inlet overtemp
//     Bit8: input power limit, Bit9: power limit overtemp, Bit10: discharge abnormal
//   Bytes 4-7: reserved
// ---------------------------------------------------------------------------
struct MC4Frame {
    uint16_t module_state{0};
    uint16_t fault_warning{0};
    bool ms_hot_plug_enabled()   const { return module_state & (1u<<5); }
    bool ms_dc_input()           const { return module_state & (1u<<3); }
    bool fw_can_timeout()        const { return fault_warning & (1u<<2); }
    bool fw_internal_overtemp()  const { return fault_warning & (1u<<6); }
    bool fw_air_inlet_overtemp() const { return fault_warning & (1u<<7); }
    static MC4Frame from_bytes(const uint8_t* d) {
        return {le16(d+0), le16(d+2)};
    }
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(8, 0);
        put_le16(b.data()+0, module_state);
        put_le16(b.data()+2, fault_warning);
        return b;
    }
};

// ---------------------------------------------------------------------------
// C_M_1: Broadcast Start/Stop (CSV: C_M_1_Broadcast, 0x0803ffa0)
//   Bytes 0-2: Module processing flag (LE 24-bit bitmask, bit(N-1)=module N)
//   Byte  3:   0x55=stop, 0xAA=start
//   Byte  4:   bits7-4=group number, bits3-0=address multiple
//   Bytes 5-7: 0x00
// ---------------------------------------------------------------------------
struct CM1Frame {
    uint32_t module_flag{0xFFFFFFu};  // default: all modules 1-24
    bool     start{false};
    uint8_t  group{1};
    uint8_t  addr_multiple{0};

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(8, 0);
        b[0]=module_flag&0xFF; b[1]=(module_flag>>8)&0xFF; b[2]=(module_flag>>16)&0xFF;
        b[3]=start ? CMD_START : CMD_STOP;
        b[4]=((group&0xF)<<4)|(addr_multiple&0xF);
        return b;
    }
    static CM1Frame from_bytes(const uint8_t* d) {
        CM1Frame f;
        f.module_flag   = uint32_t(d[0])|(uint32_t(d[1])<<8)|(uint32_t(d[2])<<16);
        f.start         = (d[3]==CMD_START);
        f.group         = (d[4]>>4)&0xF;
        f.addr_multiple = d[4]&0xF;
        return f;
    }
};

// ---------------------------------------------------------------------------
// C_M_2: Broadcast Voltage/Current Setpoint (CSV: C_M_2_Broadcast, 0x1004ffa0)
//   Bytes 0-2: Module processing flag (same bitmask as C_M_1)
//   Byte  3:   group + address multiple
//   Bytes 4-5: Charging voltage (× 0.1 V, LE, 0–750 V)
//   Bytes 6-7: Charging current (× 0.01 A, LE, 0–500 A)
// ---------------------------------------------------------------------------
struct CM2Frame {
    uint32_t module_flag{0xFFFFFFu};
    float    voltage_V{0.f};
    float    current_A{0.f};
    uint8_t  group{1};
    uint8_t  addr_multiple{0};

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(8, 0);
        b[0]=module_flag&0xFF; b[1]=(module_flag>>8)&0xFF; b[2]=(module_flag>>16)&0xFF;
        b[3]=((group&0xF)<<4)|(addr_multiple&0xF);
        put_le16(b.data()+4, static_cast<uint16_t>(voltage_V/0.1f));
        put_le16(b.data()+6, static_cast<uint16_t>(current_A/0.01f));
        return b;
    }
    static CM2Frame from_bytes(const uint8_t* d) {
        CM2Frame f;
        f.module_flag   = uint32_t(d[0])|(uint32_t(d[1])<<8)|(uint32_t(d[2])<<16);
        f.group         = (d[3]>>4)&0xF;
        f.addr_multiple = d[3]&0xF;
        f.voltage_V     = le16(d+4)*0.1f;
        f.current_A     = le16(d+6)*0.01f;
        return f;
    }
};

// ---------------------------------------------------------------------------
// C_M_3: Timing Command / Keepalive (CSV: C_M_3, 0x1805ffa0)
//   All 8 bytes: 0x00 (reserved/standby)
//   Sent periodically by master to trigger module status reports
// ---------------------------------------------------------------------------
struct CM3Frame {
    std::vector<uint8_t> to_bytes() const { return std::vector<uint8_t>(8, 0); }
};

// ---------------------------------------------------------------------------
// C_M_4: Input Mode Configuration (CSV: C_M_4, 0x18aaffa0)
//   Byte 0: 0=DC input, 1=AC input  (only accepted when module powered off)
//   Bytes 1-7: 0x00
// ---------------------------------------------------------------------------
struct CM4Frame {
    bool ac_input{true};  // default: AC mode
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(8, 0);
        b[0] = ac_input ? 0x01 : 0x00;
        return b;
    }
    static CM4Frame from_bytes(const uint8_t* d) { return {d[0]==0x01}; }
};

// ---------------------------------------------------------------------------
// C_M_12: Address Setting Mode (CSV: C_M_12, 0x1c90ffa0)
//   Byte 0: 0=Automatic (default), 1=Manual  (only when powered off)
//   Bytes 1-7: reserved
// ---------------------------------------------------------------------------
struct CM12Frame {
    bool manual{false};
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(8, 0);
        b[0] = manual ? 0x01 : 0x00;
        return b;
    }
};

// ---------------------------------------------------------------------------
// C_M_23: Module Address Setting
//   Broadcast (CSV: C_M_23_Broadcast, 0x1809ffa0) — set all modules to same addr
//   Unicast   (CSV: C_M_23_ModN,      0x18090Na0) — set specific module
//   Byte 0: new address (1–240, stored in EEPROM)
//   Bytes 1-7: 0x00
// ---------------------------------------------------------------------------
struct CM23Frame {
    uint8_t new_address{1};
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(8, 0);
        b[0] = new_address;
        return b;
    }
};

// ---------------------------------------------------------------------------
// C_M_24: Specific Module Start/Stop + Setpoint
//   (CSV: C_M_24_ModN, 0x08060Na0 — unicast to module N)
//   Byte 0:   0x55=stop, 0xAA=start
//   Byte 1:   charging mode (0x00 in standby)
//   Bytes 2-3: voltage (× 0.1 V, LE, 0–1000 V)
//   Bytes 4-5: current (× 0.01 A, LE, 0–500 A)
//   Bytes 6-7: 0x00
// ---------------------------------------------------------------------------
struct CM24Frame {
    bool    start{false};
    uint8_t mode{0x00};
    float   voltage_V{0.f};
    float   current_A{0.f};

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> b(8, 0);
        b[0]=start ? CMD_START : CMD_STOP;
        b[1]=mode;
        put_le16(b.data()+2, static_cast<uint16_t>(voltage_V/0.1f));
        put_le16(b.data()+4, static_cast<uint16_t>(current_A/0.01f));
        return b;
    }
    static CM24Frame from_bytes(const uint8_t* d) {
        CM24Frame f;
        f.start     = (d[0]==CMD_START);
        f.mode      = d[1];
        f.voltage_V = le16(d+2)*0.1f;
        f.current_A = le16(d+4)*0.01f;
        return f;
    }
};

} // namespace tonhe

#endif // TONHE_CAN_PACKETS_HPP