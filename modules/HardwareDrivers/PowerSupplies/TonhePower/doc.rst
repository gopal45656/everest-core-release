.. _everest_modules_handwritten_TonhePower:

*******************************************
TonhePower
*******************************************

:ref:`Link <everest_modules_TonhePower>` to the module's reference.

Driver for Tonhe DC power supply modules using the proprietary CAN protocol
(THJS-TXXY-0060 V1.2). Supports bidirectional (export and import) operation
and controls up to 240 modules sharing a single 125 kbps CAN bus.

Overview
========

The TonhePower module implements the EVerest ``power_supply_DC`` interface for
Tonhe BEG1K075G bidirectional charging modules. Each module is rated at
30 kW (200–1000 V, up to 30 A DC). Multiple modules connected to the same CAN
bus are controlled in parallel, with the driver distributing the requested
current equally across all active units.

The CAN bus is brought up automatically at startup using ``ip link`` commands.
The driver uses SAE J1939-style 29-bit extended CAN frames at 125 kbps.

Module discovery
================

Modules do not need to be pre-configured. When ``module_addresses`` is left
empty (the default), the driver auto-discovers each module's node ID from the
first ``M_C_1`` status frame it receives from that address. Discovered modules
are added to the internal telemetry map and immediately receive setpoints.

If you know the node IDs in advance, you can list them in ``module_addresses``
(e.g. ``"1,2,3,4,5,6"``). Pre-configured modules are tracked from startup even
before their first CAN frame arrives, so the driver can detect heartbeat
timeouts for modules that never come online.

The legacy value ``"0"`` selects single-module broadcast mode and should only
be used with installations that have exactly one module.

CAN protocol
============

The protocol uses two frame directions:

**Module → Master (uplink)**

+--------+------------+-------------------------------------------------------+
| Frame  | PGN        | Purpose                                               |
+========+============+=======================================================+
| M_C_1  | 0x1801     | Module state, DC output voltage/current, fault bits   |
+--------+------------+-------------------------------------------------------+
| M_C_2  | 0x0802     | Start/stop acknowledgement                            |
+--------+------------+-------------------------------------------------------+
| M_C_3  | 0x180B     | AC input phase voltages and ambient temperature       |
+--------+------------+-------------------------------------------------------+
| M_C_4  | 0x1C91     | Extended state/fault-warning bits                     |
+--------+------------+-------------------------------------------------------+

Modules transmit ``M_C_1`` autonomously every ~500 ms without polling.

**Master → Module (downlink)**

+----------+-------------------+-----------------------------------------------+
| Frame    | Type              | Purpose                                       |
+==========+===================+===============================================+
| C_M_1    | Broadcast         | Start/stop all selected modules               |
+----------+-------------------+-----------------------------------------------+
| C_M_2    | Broadcast         | Voltage/current setpoint (per-module share)   |
+----------+-------------------+-----------------------------------------------+
| C_M_3    | Broadcast         | Timing keepalive (every 5 s)                  |
+----------+-------------------+-----------------------------------------------+
| C_M_4    | Broadcast         | Input mode configuration (AC or DC)           |
+----------+-------------------+-----------------------------------------------+
| C_M_12   | Broadcast         | Address setting mode                          |
+----------+-------------------+-----------------------------------------------+
| C_M_24   | Unicast per node  | Per-module start/stop + voltage/current       |
+----------+-------------------+-----------------------------------------------+

The master address is ``0xA0``; the broadcast destination is ``0xFF``.

The driver transmits a ``C_M_4`` frame at startup to configure modules for DC
input operation (the only mode supported at runtime). The TX loop runs every
500 ms and sends ``C_M_24`` unicast setpoints to each active module followed by
a ``C_M_2`` broadcast. ``C_M_3`` keepalives are sent every 5 s (every 10th
TX cycle). ``C_M_1`` start/stop state is re-asserted every 2.5 s so newly
recovered modules always pick up the correct output state.

Setpoint distribution
=====================

When ``setExportVoltageCurrent`` or ``setImportVoltageCurrent`` is called, the
requested total current is divided equally among all currently active modules.
Each module receives its individual share via a ``C_M_24`` unicast frame. The
subsequent ``C_M_2`` broadcast also carries the per-module share (not the
aggregate total) to prevent modules from overriding the ``C_M_24`` value with
the full aggregate, which would cause an overcurrent hazard.

Setpoints are stored atomically and re-applied automatically whenever the
active module count changes (hot-plug or hot-unplug).

Capability aggregation
======================

Capabilities are computed dynamically from the set of active modules. The
aggregate limits are:

* **Max export current** — sum of ``dc_max_output_current`` across all online
  modules (falls back to 30 A per module when hardware-reported limits are not
  yet available).
* **Max export power** — sum of rated output power across all online modules
  (defaults to the ``per_module_rated_power_W`` config value per module).
* **Voltage range** — intersection of per-module voltage ranges
  (200–1000 V static fallback).

If an ``evse_manager`` connection is provided, the driver additionally clamps
the published capabilities to the hardware limits reported by the EVSE board
support layer.

When no modules are online the driver publishes zero capabilities and logs a
warning. Capabilities are re-published whenever the active module count changes.

Bidirectional operation
=======================

The module supports both export (AC → DC charging) and import (DC → AC
vehicle-to-grid) modes. Import mode is selected by calling ``setMode`` with
``Mode::Import``; this asserts the inverter mode flag on all active modules
via the internal ``set_inverter_mode`` command. The same voltage/current limits
apply in both directions.

Heartbeat and fault detection
==============================

Each module's last-seen timestamp is updated on every incoming CAN frame. If
no frame is received within ``heartbeat_timeout_ms``, the module is marked
offline and a ``CommunicationFault`` error is raised. When the module resumes
sending frames it is marked online again, the ``CommunicationFault`` is cleared,
and the current setpoints are re-applied immediately.

The default timeout is 60 000 ms. On embedded hardware with slow storage
(eMMC/SD cards), OS-level IRQ delays can momentarily starve the CAN receive
thread. Using a value well above the module's internal CAN timeout
(10 000 ms per the V1.13 spec, typically 60 000 ms) avoids false timeouts that
could interrupt active charging.

Error mapping
=============

Fault bits from ``M_C_1`` and ``M_C_4`` frames are translated to EVerest
``power_supply_DC`` error types:

+---------------------+----------------------------------+------------------+
| Tonhe fault         | EVerest error type               | Severity         |
+=====================+==================================+==================+
| OverVoltageDC       | power_supply_DC/OverVoltageDC    | High             |
+---------------------+----------------------------------+------------------+
| UnderVoltageDC      | power_supply_DC/UnderVoltageDC   | High             |
+---------------------+----------------------------------+------------------+
| OverTemperature     | power_supply_DC/OverTemperature  | High             |
+---------------------+----------------------------------+------------------+
| OverCurrentDC       | power_supply_DC/OverCurrentDC    | High             |
+---------------------+----------------------------------+------------------+
| HardwareFault       | power_supply_DC/HardwareFault    | High             |
+---------------------+----------------------------------+------------------+
| CommunicationFault  | power_supply_DC/CommunicationFault | High           |
+---------------------+----------------------------------+------------------+
| UnderVoltageAC      | power_supply_DC/UnderVoltageAC   | High             |
+---------------------+----------------------------------+------------------+
| FanFault            | power_supply_DC/VendorWarning    | Medium           |
+---------------------+----------------------------------+------------------+
| InputPhaseLoss      | power_supply_DC/VendorError      | High             |
+---------------------+----------------------------------+------------------+

Errors are raised once when a fault bit transitions from clear to set, and
cleared when the bit transitions back. Duplicate raises are suppressed by
checking the active error state before raising.

Temperature protection
======================

When ``overtemperature_limit_C`` is set (default 85 °C), the driver compares
each module's ambient temperature (from ``M_C_3``) against the threshold after
every frame. Exceeding the limit raises an ``OverTemperature`` error for that
module. Set to ``0`` to disable the software temperature check and rely solely
on the module's internal hardware protection.

Configuration reference
=======================

.. list-table::
   :header-rows: 1
   :widths: 30 10 10 50

   * - Parameter
     - Type
     - Default
     - Description
   * - ``can_device``
     - string
     - ``can0``
     - SocketCAN interface name. The driver brings the interface up at 125 kbps
       before opening it.
   * - ``module_addresses``
     - string
     - ``""``
     - Optional comma-separated list of node IDs (e.g. ``"1,2,3,4,5,6"``).
       Leave empty for auto-discovery. Use ``"0"`` only for legacy single-module
       broadcast mode.
   * - ``heartbeat_timeout_ms``
     - integer
     - ``60000``
     - Milliseconds without a frame before a module is considered offline. Must
       exceed the module's internal CAN timeout (10 000 ms). Use 60 000 ms or
       higher on embedded hardware with slow storage.
   * - ``overtemperature_limit_C``
     - number
     - ``85.0``
     - Software temperature limit in °C. Set to ``0`` to disable.
   * - ``conversion_efficiency_export``
     - number
     - ``0.95``
     - AC-to-DC conversion efficiency reported in capabilities (0.0–1.0). Also
       used for the import direction.
   * - ``per_module_rated_power_W``
     - number
     - ``30000.0``
     - Rated output power per module in Watts. Used for capability aggregation
       when hardware-reported limits are not yet available.

Hardware notes
==============

* The BEG1K075G module uses a 29-bit extended CAN frame format (CAN 2.0B).
* All multi-byte values in the protocol are little-endian (Intel byte order).
* The CAN bus must be terminated at both physical ends with 120 Ω resistors.
* Up to 240 module addresses (0x01–0xF0) are supported by the protocol; the
  practical limit per bus segment is determined by bus load and cable length.
* Modules accept input mode changes (``C_M_4``) only while powered off.
* Address assignment (``C_M_23``) is stored in module EEPROM and survives power
  cycles.
