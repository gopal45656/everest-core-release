#!/usr/bin/env python3
"""
energy_reader_test.py
=====================
Python equivalent of energy_readerImpl.cpp for testing with a real
Rishabh RC2111 Modbus RTU meter and a DWIN serial display.

Usage:
    python3 energy_reader_test.py --gun 1
    python3 energy_reader_test.py --gun 2
    python3 energy_reader_test.py --gun 1 --no-display   # skip DWIN, JSON only

Hardware connections (from config-amararaja-06.yaml):
    Gun 1 meter  : /dev/ttyCH9344USB0  (serial_comm_hub1, Rishabh RC2111, slave_id=1)
    Gun 2 meter  : /dev/ttyCH9344USB1  (serial_comm_hub2, Rishabh RC2111, slave_id=1)
    DWIN display : /dev/ttyCH9344USB2  (energy_reader serial_port, 115200 baud)

JSON output : /usr/share/everest/modules/EnergyReader/reader_data.json

Scaling factors (from config):
    voltage  x10    (e.g. 236.4 V  → 2364 sent to DWIN)
    current  x10    (e.g.  14.5 A  →  145)
    power    x100   (e.g.   3.42kW → 342)
    energy   x10    (e.g.   1.23kWh→  12)

DWIN packet format (8 bytes):
    [5A  A5  06  82  VP_H  VP_L  VAL_H  VAL_L]

Dependencies:
    pip install pymodbus pyserial
"""

import argparse
import json
import os
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

# ── optional pymodbus import ──────────────────────────────────────────────────
try:
    from pymodbus.client import ModbusSerialClient
    MODBUS_AVAILABLE = True
except ImportError:
    MODBUS_AVAILABLE = False
    print("[WARN] pymodbus not installed — meter reads will be skipped.")
    print("       Install with: pip install pymodbus")

# ── optional pyserial import ──────────────────────────────────────────────────
try:
    import serial
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False
    print("[WARN] pyserial not installed — DWIN display writes will be skipped.")
    print("       Install with: pip install pyserial")

# =============================================================================
# Constants matching energy_readerImpl.cpp
# =============================================================================

PKT_HEADER_0 = 0x5A
PKT_HEADER_1 = 0xA5
PKT_LEN      = 0x05
PKT_CMD      = 0x82
INTER_PACKET_DELAY_MS = 20   # ms between DWIN VP writes

EMA_ALPHA = 0.25             # same as C++ EMA_SMOOTHING_ALPHA

JSON_STATUS_FILE = "/usr/share/everest/modules/EnergyReader/reader_data.json"

# ── VP register addresses (Gun 1: 100-122, Gun 2: 200-222) ────────────────────
VP = {
    "gun1": {
        "voltage_dc":          100,
        "current_dc":          101,
        "power_total":         102,
        "energy_import":       103,
        "energy_export":       104,
        "max_voltage":         105,
        "min_voltage":         106,
        "max_current":         107,
        "min_current":         108,
        "demand_voltage":      109,
        "demand_current":      110,
        "charging_voltage":    111,
        "charging_current":    112,
        "gun_status":          115,
        "charging_active":     116,
        "charging_complete":   117,
        "gun_unplugged":       118,
        "charge_duration_min": 119,
        "fault":               120,
        "initial_soc":         121,
        "current_soc":         122,
    },
    "gun2": {
        "voltage_dc":          200,
        "current_dc":          201,
        "power_total":         202,
        "energy_import":       203,
        "energy_export":       204,
        "max_voltage":         205,
        "min_voltage":         206,
        "max_current":         207,
        "min_current":         208,
        "demand_voltage":      209,
        "demand_current":      210,
        "charging_voltage":    211,
        "charging_current":    212,
        "gun_status":          215,
        "charging_active":     216,
        "charging_complete":   217,
        "gun_unplugged":       218,
        "charge_duration_min": 219,
        "fault":               220,
        "initial_soc":         221,
        "current_soc":         222,
    }
}

SCALE = {
    "voltage": 10.0,
    "current": 10.0,
    "power":   100.0,
    "energy":  10.0,
    "soc":     10.0,
}

# =============================================================================
# Rishabh RC2111 Modbus register map
# RC2111 uses 16-bit input registers (function code 04)
# Adjust register addresses to match your actual meter datasheet.
# =============================================================================

RC2111_REGS = {
    "voltage_v":        0x0000,   # Voltage (raw / scale TBD)
    "current_a":        0x0006,   # Current (raw / scale TBD)
    "power_w":          0x000C,   # Active power (raw)
    "energy_import_wh": 0x0100,   # Import energy (Wh, 32-bit across 2 regs)
    "energy_export_wh": 0x0102,   # Export energy (Wh, 32-bit)
}

RC2111_VOLTAGE_SCALE  = 0.1   # raw → Volts    (adjust per datasheet)
RC2111_CURRENT_SCALE  = 0.01  # raw → Amps
RC2111_POWER_SCALE    = 1.0   # raw → Watts
RC2111_ENERGY_SCALE   = 1.0   # raw → Wh


# =============================================================================
# GunState — mirrors DisplayValues struct in energy_readerImpl.hpp
# =============================================================================

@dataclass
class GunState:
    # powermeter values
    voltage_dc:         float = 0.0
    current_dc:         float = 0.0
    power_kw:           float = 0.0
    energy_import_kwh:  float = 0.0
    energy_export_kwh:  float = 0.0
    max_voltage:        float = 0.0
    min_voltage:        float = 0.0
    max_current:        float = 0.0
    min_current:        float = 0.0
    timestamp:          str   = ""

    # ev_info / PSU set-points (not available without EVSEManager; left at 0)
    demand_voltage:     float = 0.0
    demand_current:     float = 0.0
    charging_voltage:   float = 0.0
    charging_current:   float = 0.0

    # SOC
    current_soc_percent:   float = 0.0
    initial_soc_percent:   float = 0.0
    initial_soc_captured:  bool  = False

    # session state flags (mirrors session_event logic)
    gun_plugged:              bool  = False
    gun_unplugged:            bool  = True   # idle by default
    charging_active_icon:     bool  = False
    charging_complete_icon:   bool  = False
    charging_active:          bool  = False
    fault:                    bool  = False

    # duration tracking
    charge_start_time:              Optional[float] = None  # time.monotonic()
    charge_duration_seconds:        int   = 0
    accumulated_duration_seconds:   int   = 0

    # EMA / session baseline
    initialized:                    bool  = False
    session_energy_import_baseline: float = 0.0
    session_energy_export_baseline: float = 0.0

    # lock (created post-init)
    lock: threading.Lock = field(default_factory=threading.Lock, compare=False, repr=False)

# =============================================================================
# Helpers
# =============================================================================

def ema_blend(previous: float, sample: float, have_previous: bool) -> float:
    """Exponential moving average — matches C++ ema_blend()"""
    if not have_previous:
        return sample
    return EMA_ALPHA * sample + (1.0 - EMA_ALPHA) * previous


def scale_to_u16(value: float, scale: float) -> int:
    """Matches C++ scale_to_u16() — clamp to [0, 65535]"""
    if not isinstance(value, (int, float)) or value != value:  # NaN check
        return 0
    scaled = value * scale
    if scaled < 0:
        return 0
    if scaled > 65535:
        return 65535
    return int(round(scaled))


def build_dwin_packet(vp_address: int, value: int) -> bytes:
    """Build 8-byte DWIN VP write packet: [5A A5 06 82 VP_H VP_L VAL_H VAL_L]"""
    return bytes([
        PKT_HEADER_0,
        PKT_HEADER_1,
        PKT_LEN,
        PKT_CMD,
        (vp_address >> 8) & 0xFF,
        vp_address & 0xFF,
        (value >> 8) & 0xFF,
        value & 0xFF,
    ])


# =============================================================================
# EnergyReaderTest — main class
# =============================================================================

class EnergyReaderTest:
    def __init__(self, gun_id: int, meter_port: str, meter_baud: int,
                 meter_slave_id: int, display_port: str, display_baud: int,
                 publish_interval_ms: int, no_display: bool):

        assert gun_id in (1, 2), "gun_id must be 1 or 2"
        self.gun_id      = gun_id
        self.gun_key     = f"gun{gun_id}"
        self.vp          = VP[self.gun_key]

        self.meter_port       = meter_port
        self.meter_baud       = meter_baud
        self.meter_slave_id   = meter_slave_id
        self.display_port     = display_port
        self.display_baud     = display_baud
        self.publish_interval = publish_interval_ms / 1000.0
        self.no_display       = no_display

        self.state  = GunState()
        self.serial = None   # DWIN serial handle
        self.client = None   # Modbus client
        self.running = False

        self._serial_lock = threading.Lock()
        self._cycle = 0

    # ─────────────────────────────────────────────────────────────────────────
    # Modbus — read Rishabh RC2111
    # ─────────────────────────────────────────────────────────────────────────

    def _connect_modbus(self) -> bool:
        if not MODBUS_AVAILABLE:
            return False
        try:
            self.client = ModbusSerialClient(
                port=self.meter_port,
                baudrate=self.meter_baud,
                parity='N',
                stopbits=1,
                bytesize=8,
                timeout=1,
            )
            ok = self.client.connect()
            if ok:
                print(f"[MODBUS] Connected to meter on {self.meter_port} "
                      f"@ {self.meter_baud} baud, slave_id={self.meter_slave_id}")
            else:
                print(f"[MODBUS] Failed to connect to {self.meter_port}")
            return ok
        except Exception as e:
            print(f"[MODBUS] Connection error: {e}")
            return False

    def _read_register_u32(self, start_reg: int) -> Optional[int]:
        """Read two consecutive 16-bit input registers as a 32-bit unsigned int."""
        try:
            result = self.client.read_input_registers(start_reg, 2,
                                                       slave=self.meter_slave_id)
            if result.isError():
                return None
            return (result.registers[0] << 16) | result.registers[1]
        except Exception:
            return None

    def _read_register_u16(self, reg: int) -> Optional[int]:
        """Read a single 16-bit input register."""
        try:
            result = self.client.read_input_registers(reg, 1,
                                                       slave=self.meter_slave_id)
            if result.isError():
                return None
            return result.registers[0]
        except Exception:
            return None

    def _poll_meter(self):
        """Poll meter and call update_display_values() — mirrors on_powermeter_update()"""
        if not self.client:
            return

        raw_v = self._read_register_u16(RC2111_REGS["voltage_v"])
        raw_i = self._read_register_u16(RC2111_REGS["current_a"])
        raw_p = self._read_register_u16(RC2111_REGS["power_w"])
        raw_e = self._read_register_u32(RC2111_REGS["energy_import_wh"])
        raw_ex = self._read_register_u32(RC2111_REGS["energy_export_wh"])

        voltage_v = raw_v * RC2111_VOLTAGE_SCALE if raw_v is not None else None
        current_a = raw_i * RC2111_CURRENT_SCALE if raw_i is not None else None
        power_w   = raw_p * RC2111_POWER_SCALE   if raw_p is not None else None
        energy_import_wh = raw_e * RC2111_ENERGY_SCALE if raw_e is not None else None
        energy_export_wh = raw_ex * RC2111_ENERGY_SCALE if raw_ex is not None else None

        self._update_display_values(voltage_v, current_a, power_w,
                                    energy_import_wh, energy_export_wh)

    # ─────────────────────────────────────────────────────────────────────────
    # State update — mirrors update_display_values() in C++
    # ─────────────────────────────────────────────────────────────────────────

    def _update_display_values(self, voltage_v, current_a, power_w,
                               energy_import_wh, energy_export_wh):
        s = self.state
        with s.lock:
            # Voltage
            if voltage_v is not None and voltage_v == voltage_v:  # not NaN
                s.voltage_dc = ema_blend(s.voltage_dc, voltage_v, s.initialized)
                if not s.initialized:
                    s.max_voltage = s.voltage_dc
                    s.min_voltage = s.voltage_dc
                else:
                    s.max_voltage = max(s.max_voltage, s.voltage_dc)
                    s.min_voltage = min(s.min_voltage, s.voltage_dc)

            # Current
            if current_a is not None and current_a == current_a:
                s.current_dc = ema_blend(s.current_dc, current_a, s.initialized)
                if not s.initialized:
                    s.max_current = s.current_dc
                    s.min_current = s.current_dc
                else:
                    s.max_current = max(s.max_current, s.current_dc)
                    s.min_current = min(s.min_current, s.current_dc)

            # Power
            if power_w is not None and power_w == power_w:
                sample_kw = power_w / 1000.0
                s.power_kw = ema_blend(s.power_kw, sample_kw, s.initialized)

            # Energy import (lifetime → session delta)
            if energy_import_wh is not None and energy_import_wh == energy_import_wh:
                session_kwh = (energy_import_wh / 1000.0) - s.session_energy_import_baseline
                s.energy_import_kwh = max(0.0, session_kwh)

            # Energy export
            if energy_export_wh is not None and energy_export_wh == energy_export_wh:
                session_kwh = (energy_export_wh / 1000.0) - s.session_energy_export_baseline
                s.energy_export_kwh = max(0.0, session_kwh)

            s.timestamp   = datetime.now(timezone.utc).isoformat()
            s.initialized = True


    # ─────────────────────────────────────────────────────────────────────────
    # Session event simulation — mirrors on_session_event() in C++
    # ─────────────────────────────────────────────────────────────────────────

    def session_started(self):
        """Call this to simulate EV plug-in (SessionStarted)"""
        s = self.state
        with s.lock:
            s.gun_plugged               = True
            s.gun_unplugged             = False
            s.charging_active_icon      = False
            s.charging_complete_icon    = False
            s.fault                     = False
            s.charging_active           = False
            s.demand_voltage            = 0.0
            s.demand_current            = 0.0
            s.charging_voltage          = 0.0
            s.charging_current          = 0.0
            s.current_soc_percent       = 0.0
            s.initial_soc_percent       = 0.0
            s.initial_soc_captured      = False
            s.charge_duration_seconds   = 0
            s.accumulated_duration_seconds = 0
            s.max_voltage               = 0.0
            s.min_voltage               = 0.0
            s.max_current               = 0.0
            s.min_current               = 0.0
            s.initialized               = False

            # Snapshot lifetime energy as session baseline
            s.session_energy_import_baseline = s.energy_import_kwh
            s.session_energy_export_baseline = s.energy_export_kwh
            s.energy_import_kwh = 0.0
            s.energy_export_kwh = 0.0

        print(f"[SESSION Gun{self.gun_id}] SessionStarted → gun_plugged=1")

    def charging_started(self):
        """Call this to simulate ChargingStarted"""
        s = self.state
        with s.lock:
            s.gun_plugged               = True
            s.gun_unplugged             = False
            s.charging_active_icon      = True
            s.charging_complete_icon    = False
            s.fault                     = False
            s.charging_active           = True
            s.charge_start_time         = time.monotonic()
            s.charge_duration_seconds   = 0
            s.accumulated_duration_seconds = 0
        print(f"[SESSION Gun{self.gun_id}] ChargingStarted → charging_active=1")

    def charging_finished(self):
        """Call this to simulate ChargingFinished"""
        s = self.state
        with s.lock:
            self._freeze_duration_locked(s)
            s.gun_plugged               = False
            s.gun_unplugged             = False
            s.charging_active_icon      = False
            s.charging_complete_icon    = True
            s.charging_active           = False
            s.fault                     = False
        print(f"[SESSION Gun{self.gun_id}] ChargingFinished → charging_complete=1")

        # 5s → clear complete icon, 10s → zero all data
        threading.Thread(target=self._post_completion_reset, daemon=True).start()

    def session_finished(self):
        """Call this to simulate EV unplug (SessionFinished)"""
        s = self.state
        with s.lock:
            s.gun_plugged               = False
            s.gun_unplugged             = True
            s.charging_active_icon      = False
            s.charging_active           = False
            s.charging_complete_icon    = False
            s.fault                     = False
            s.demand_voltage            = 0.0
            s.demand_current            = 0.0
            s.charging_voltage          = 0.0
            s.charging_current          = 0.0
        print(f"[SESSION Gun{self.gun_id}] SessionFinished → gun_unplugged=1 (idle)")

    def _freeze_duration_locked(self, s: GunState):
        """Freeze elapsed time into accumulator (call with lock held)"""
        if s.charging_active and s.charge_start_time is not None:
            elapsed = time.monotonic() - s.charge_start_time
            s.accumulated_duration_seconds += int(elapsed)

    def _post_completion_reset(self):
        """5s clear complete icon, 10s zero all session data"""
        time.sleep(5)
        with self.state.lock:
            self.state.charging_complete_icon = False
            self.state.gun_plugged            = False
            self.state.gun_unplugged          = True
        print(f"[RESET Gun{self.gun_id}] 5s — complete icon cleared, gun_unplugged=1")

        time.sleep(5)
        s = self.state
        with s.lock:
            s.demand_voltage = s.demand_current = 0.0
            s.charging_voltage = s.charging_current = 0.0
            s.current_soc_percent = s.initial_soc_percent = 0.0
            s.initial_soc_captured = False
            s.charge_duration_seconds = s.accumulated_duration_seconds = 0
            s.voltage_dc = s.current_dc = s.power_kw = 0.0
            s.energy_import_kwh = s.energy_export_kwh = 0.0
            s.max_current = s.min_current = 0.0
            s.gun_unplugged = True
        print(f"[RESET Gun{self.gun_id}] 10s — all session data zeroed (idle)")


    # ─────────────────────────────────────────────────────────────────────────
    # DWIN serial display — mirrors write_vp_register() and write_all_display_registers()
    # ─────────────────────────────────────────────────────────────────────────

    def _open_display(self) -> bool:
        if self.no_display or not SERIAL_AVAILABLE:
            return False
        try:
            self.serial = serial.Serial(
                port=self.display_port,
                baudrate=self.display_baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1,
            )
            print(f"[DWIN] Serial opened — port={self.display_port} baud={self.display_baud}")
            return True
        except Exception as e:
            print(f"[DWIN] Cannot open serial port {self.display_port}: {e}")
            return False

    def _write_vp(self, vp_addr: int, value: int, label: str, raw_float: float = None) -> bool:
        if self.serial is None:
            return True   # no display mode — silently skip
        pkt = build_dwin_packet(vp_addr, value)
        with self._serial_lock:
            try:
                written = self.serial.write(pkt)
                self.serial.flush()
                time.sleep(INTER_PACKET_DELAY_MS / 1000.0)
                if written != len(pkt):
                    print(f"[DWIN] Short write on {label}: sent {written}/{len(pkt)} bytes")
                    return False
                # Show: actual value (float) → VP address (hex) → raw int sent
                float_str = f"{raw_float:.4g}" if raw_float is not None else "—"
                print(f"[DWIN] {float_str:>10} → VP 0x{vp_addr:04X} ({vp_addr:3d}) "
                      f"raw={value:5d}  [{' '.join(f'{b:02X}' for b in pkt)}]  ({label})")
                return True
            except Exception as e:
                print(f"[DWIN] Write error on {label}: {e}")
                return False

    def _write_all_registers(self) -> bool:
        s  = self.state
        vp = self.vp
        sc = SCALE
        all_ok = True

        def w(key, raw_val, float_val=None):
            nonlocal all_ok
            if not self._write_vp(vp[key], raw_val, key, float_val):
                all_ok = False

        with s.lock:
            # Measured values
            w("voltage_dc",          scale_to_u16(s.voltage_dc,        sc["voltage"]),  s.voltage_dc)
            w("current_dc",          scale_to_u16(s.current_dc,        sc["current"]),  s.current_dc)
            w("power_total",         scale_to_u16(s.power_kw,          sc["power"]),    s.power_kw)
            w("energy_import",       scale_to_u16(s.energy_import_kwh, sc["energy"]),   s.energy_import_kwh)
            w("energy_export",       scale_to_u16(s.energy_export_kwh, sc["energy"]),   s.energy_export_kwh)
            w("max_voltage",         scale_to_u16(s.max_voltage,        sc["voltage"]),  s.max_voltage)
            w("min_voltage",         scale_to_u16(s.min_voltage,        sc["voltage"]),  s.min_voltage)
            w("max_current",         scale_to_u16(s.max_current,        sc["current"]),  s.max_current)
            w("min_current",         scale_to_u16(s.min_current,        sc["current"]),  s.min_current)

            # Demand / PSU set-points (0 unless set externally)
            w("demand_voltage",      scale_to_u16(s.demand_voltage,     sc["voltage"]),  s.demand_voltage)
            w("demand_current",      scale_to_u16(s.demand_current,     sc["current"]),  s.demand_current)
            w("charging_voltage",    scale_to_u16(s.charging_voltage,   sc["voltage"]),  s.charging_voltage)
            w("charging_current",    scale_to_u16(s.charging_current,   sc["current"]),  s.charging_current)

            # Icons (0 or 1 — no float to show)
            w("gun_status",          1 if s.gun_plugged           else 0)
            w("gun_unplugged",       1 if s.gun_unplugged         else 0)
            w("charging_active",     1 if s.charging_active_icon  else 0)
            w("charging_complete",   1 if s.charging_complete_icon else 0)
            w("fault",               1 if s.fault                 else 0)

            # Duration
            dur_min = int(s.charge_duration_seconds / 60)
            w("charge_duration_min", dur_min, float(dur_min))

            # SOC
            w("initial_soc",         scale_to_u16(s.initial_soc_percent, sc["soc"]),  s.initial_soc_percent)
            w("current_soc",         scale_to_u16(s.current_soc_percent, sc["soc"]),  s.current_soc_percent)

        return all_ok

    # ─────────────────────────────────────────────────────────────────────────
    # JSON file writer — mirrors write_json_file() in C++
    # ─────────────────────────────────────────────────────────────────────────

    def _write_json(self):
        s = self.state
        with s.lock:
            state_str = "idle"
            if s.fault:                  state_str = "fault"
            elif s.charging_complete_icon: state_str = "completed"
            elif s.gun_unplugged:        state_str = "unplugged"
            elif s.charging_active_icon: state_str = "inprogress"
            elif s.gun_plugged:          state_str = "plugged"

            total_sec = s.charge_duration_seconds
            dur = f"{total_sec//3600:02d}:{(total_sec%3600)//60:02d}:{total_sec%60:02d}"

            is_charging = state_str in ("inprogress", "completed")
            gun_data = {
                "charger_state":          state_str,
                "charging_current":       s.current_dc if is_charging else 0.0,
                "charging_voltage":       s.voltage_dc if is_charging else 0.0,
                "current_soc_percent":    s.current_soc_percent,
                "demand_current":         s.demand_current,
                "demand_voltage":         s.demand_voltage,
                "duration":               dur,
                "energy_consumption_kwh": round(
                    s.voltage_dc * s.current_dc * total_sec / 3_600_000.0, 4
                ) if is_charging else 0.0,
                "initial_soc_percent":    s.initial_soc_percent,
                "meter": {
                    "voltage_dc_v":      round(s.voltage_dc,        2),
                    "current_dc_a":      round(s.current_dc,        3),
                    "power_kw":          round(s.power_kw,          3),
                    "energy_import_kwh": round(s.energy_import_kwh, 3),
                    "energy_export_kwh": round(s.energy_export_kwh, 3),
                    "max_voltage_v":     round(s.max_voltage,       2),
                    "min_voltage_v":     round(s.min_voltage,       2),
                    "max_current_a":     round(s.max_current,       3),
                    "min_current_a":     round(s.min_current,       3),
                    "timestamp":         s.timestamp,
                }
            }

        # Read existing JSON so the other gun's data is preserved
        try:
            with open(JSON_STATUS_FILE, "r") as f:
                root = json.load(f)
        except Exception:
            root = {}

        root[self.gun_key] = gun_data

        tmp = JSON_STATUS_FILE + ".tmp"
        try:
            Path(JSON_STATUS_FILE).parent.mkdir(parents=True, exist_ok=True)
            with open(tmp, "w") as f:
                json.dump(root, f, indent=4)
            os.replace(tmp, JSON_STATUS_FILE)
        except Exception as e:
            print(f"[JSON] Write error: {e}")


    # ─────────────────────────────────────────────────────────────────────────
    # Write loop — mirrors write_loop() in C++
    # ─────────────────────────────────────────────────────────────────────────

    def _write_loop(self):
        while self.running:
            loop_start = time.monotonic()
            self._cycle += 1

            # Update live duration counter
            s = self.state
            with s.lock:
                if s.charging_active and s.charge_start_time is not None:
                    elapsed = time.monotonic() - s.charge_start_time
                    s.charge_duration_seconds = (
                        s.accumulated_duration_seconds + int(elapsed)
                    )

            # Poll meter
            self._poll_meter()

            if s.initialized:
                ok = self._write_all_registers()
                self._print_footer(ok)
                self._write_json()
            else:
                print(f"[Cycle {self._cycle:04d}] Waiting for first meter reading...")

            elapsed = time.monotonic() - loop_start
            remaining = self.publish_interval - elapsed
            if remaining > 0:
                time.sleep(remaining)

    def _print_footer(self, ok: bool):
        s = self.state
        with s.lock:
            print(
                f"[C#{self._cycle:04d}] {'OK ' if ok else 'ERR'} | "
                f"V={s.voltage_dc:6.2f}V I={s.current_dc:5.2f}A "
                f"P={s.power_kw:5.3f}kW Ei={s.energy_import_kwh:6.3f}kWh | "
                f"plug={int(s.gun_plugged)} unplug={int(s.gun_unplugged)} "
                f"act={int(s.charging_active_icon)} done={int(s.charging_complete_icon)} "
                f"fault={int(s.fault)} dur={s.charge_duration_seconds}s"
            )

    # ─────────────────────────────────────────────────────────────────────────
    # Start / Stop
    # ─────────────────────────────────────────────────────────────────────────

    def start(self):
        self._open_display()
        self._connect_modbus()
        self.running = True
        self._thread = threading.Thread(target=self._write_loop, daemon=True)
        self._thread.start()
        print(f"[INFO] EnergyReaderTest started for Gun {self.gun_id}")

    def stop(self):
        self.running = False
        if hasattr(self, "_thread"):
            self._thread.join(timeout=3)
        if self.serial:
            self.serial.close()
        if self.client:
            self.client.close()
        print(f"[INFO] EnergyReaderTest stopped for Gun {self.gun_id}")


# =============================================================================
# Interactive test loop
# =============================================================================

def interactive_loop(reader: EnergyReaderTest):
    print()
    print("═" * 60)
    print(f"  EnergyReader Python Test — Gun {reader.gun_id}")
    print("═" * 60)
    print("  Commands:")
    print("    p  — SessionStarted   (EV plugged in)")
    print("    c  — ChargingStarted  (charging begins)")
    print("    f  — ChargingFinished (charging complete)")
    print("    u  — SessionFinished  (EV unplugged)")
    print("    s  — Show current state")
    print("    q  — Quit")
    print("═" * 60)
    print()

    while True:
        try:
            cmd = input("cmd> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            break

        if cmd == "p":
            reader.session_started()
        elif cmd == "c":
            reader.charging_started()
        elif cmd == "f":
            reader.charging_finished()
        elif cmd == "u":
            reader.session_finished()
        elif cmd == "s":
            s = reader.state
            with s.lock:
                print(f"\n  Gun {reader.gun_id} State:")
                print(f"    V={s.voltage_dc:.2f}V  I={s.current_dc:.3f}A  "
                      f"P={s.power_kw:.3f}kW  E={s.energy_import_kwh:.3f}kWh")
                print(f"    plug={s.gun_plugged}  charging={s.charging_active_icon}  "
                      f"complete={s.charging_complete_icon}  fault={s.fault}")
                print(f"    duration={s.charge_duration_seconds}s  "
                      f"initialized={s.initialized}\n")
        elif cmd == "q":
            break
        else:
            print("  Unknown command. Use p/c/f/u/s/q")


# =============================================================================
# Entry point
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="EnergyReader Python test — Rishabh RC2111 + DWIN display"
    )
    parser.add_argument(
        "--gun", type=int, choices=[1, 2], default=1,
        help="Which gun to test: 1 or 2 (default: 1)"
    )
    parser.add_argument(
        "--meter-port", type=str,
        help="Serial port for Rishabh meter "
             "(default: /dev/ttyCH9344USB0 for Gun1, /dev/ttyCH9344USB1 for Gun2)"
    )
    parser.add_argument(
        "--meter-baud", type=int, default=9600,
        help="Meter baud rate (default: 9600)"
    )
    parser.add_argument(
        "--meter-slave", type=int, default=1,
        help="Modbus slave ID (default: 1)"
    )
    parser.add_argument(
        "--display-port", type=str, default="/dev/ttyCH9344USB2",
        help="Serial port for DWIN display (default: /dev/ttyCH9344USB2)"
    )
    parser.add_argument(
        "--display-baud", type=int, default=115200,
        help="DWIN baud rate (default: 115200)"
    )
    parser.add_argument(
        "--interval", type=int, default=1000,
        help="Publish interval in ms (default: 1000)"
    )
    parser.add_argument(
        "--no-display", action="store_true",
        help="Skip DWIN display writes (JSON + console only)"
    )
    args = parser.parse_args()

    # Default meter port based on gun selection
    if args.meter_port is None:
        args.meter_port = "/dev/ttyCH9344USB0" if args.gun == 1 else "/dev/ttyCH9344USB1"

    reader = EnergyReaderTest(
        gun_id            = args.gun,
        meter_port        = args.meter_port,
        meter_baud        = args.meter_baud,
        meter_slave_id    = args.meter_slave,
        display_port      = args.display_port,
        display_baud      = args.display_baud,
        publish_interval_ms = args.interval,
        no_display        = args.no_display,
    )

    reader.start()

    try:
        interactive_loop(reader)
    finally:
        reader.stop()


if __name__ == "__main__":
    main()
