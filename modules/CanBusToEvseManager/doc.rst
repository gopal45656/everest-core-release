.. _everest_modules_handwritten_CanBusToEvseManager:

*******************************************
CanBusToEvseManager
*******************************************

:ref:`Link <everest_modules_CanBusToEvseManager>` to the module's reference.

Overview
========

``CanBusToEvseManager`` is a bridge module that implements the **GB/T 27930** DC charging
protocol over CAN bus and integrates it with EVerest's ``EvseManager``.

It sits between the raw CAN hardware driver (``CanBus``) and the EVerest charging stack,
translating GB/T 27930 BMS messages into EVerest power requests — bypassing the ISO 15118
HLC path entirely for DC charging.

.. code-block:: text

    BMS (EV)
      │  CAN frames (BHM, BCP, BCL, BCS, BST, BSD, ...)
      ▼
    CanBus (HardwareDrivers)
      │  last_frame var
      ▼
    CanBusToEvseManager  ◄──── GbtProtocol state machine
      │  can_signal_receiver interface
      │    ├─ can_frame var  ──────────────────► EvseManager (logging + BST detection)
      │    └─ power_request var ───────────────► EvseManager
      │                                              │
      │                                              ├─ powersupply_DC_on()
      │                                              ├─ powersupply_DC_set(V, I)
      │                                              └─ powersupply_DC_off()
      │                                                      │
      │                                              DCSupplySimulator
      │
      └─ CAN TX (CHM, CRM, CTS, CML, CRO, CCS, CST, CSD) ──► BMS


Architecture
============

The module consists of three layers:

1. **CAN transport** — ``r_can_bus`` (``CANBus`` interface) receives raw frames via the
   ``last_frame`` variable subscription and transmits charger response frames via ``call_send()``.

2. **GB/T 27930 state machine** — ``GbtProtocol`` class (``GbtProtocol.hpp``) decodes incoming
   BMS messages and drives the charger-side protocol sequence, generating the correct charger
   responses at each state transition.

3. **EVerest integration** — ``p_can_signals`` (``can_signal_receiver`` interface) publishes
   decoded data to ``EvseManager`` via two variables: ``can_frame`` and ``power_request``.

GB/T 27930 Protocol State Machine
==================================

The ``GbtProtocol`` class implements the charger side of GB/T 27930. It handles all BMS
messages and generates the corresponding charger responses.

State sequence:

.. code-block:: text

    Idle
      │  BHM received (BMS handshake)
      ▼
    Handshake  ──TX──► CHM (charger announces max V/I)
      │  BCP received (battery parameters) — BRM optional
      ▼
    Recognition / Config  ──TX──► CRM + CTS + CML + CRO
      │  Auto-transitions to Charging (BRO not required by all BMS)
      ▼
    Charging  ◄──► BCL/BCS loop
      │  BST received (BMS stop request)
      ▼
    Stopping  ──TX──► CST
      │  BSD received (session statistics)
      ▼
    Finished  ──TX──► CSD

BRM handling note: some BMS implementations skip BRM and go directly from BHM to BCP. The
state machine handles this gracefully by sending CRM when BCP arrives in Handshake state.

Similarly, some BMS implementations skip BRO and go straight from BCP to BCL. The state machine
auto-transitions to Charging after sending CRO during Config, so BCL frames are accepted even
without an explicit BRO.

Message Mapping
---------------

+--------+----------------------------------------+--------+------------------------------------------+
| BMS TX | Description                            | EVSE TX| Description                              |
+========+========================================+========+==========================================+
| BHM    | BMS handshake                          | CHM    | Charger handshake (max V/I/type)         |
+--------+----------------------------------------+--------+------------------------------------------+
| BRM    | Battery recognition (optional)         | CRM    | Charger recognition (ready + status)     |
+--------+----------------------------------------+--------+------------------------------------------+
| BCP    | Battery charge parameters              | CTS    | Estimated charge time (fixed: 3600s)     |
+--------+----------------------------------------+--------+------------------------------------------+
|        |                                        | CML    | Charger mode (CC = 1)                    |
+--------+----------------------------------------+--------+------------------------------------------+
| BRO    | BMS ready for output (optional)        | CRO    | Charger ready to output                  |
+--------+----------------------------------------+--------+------------------------------------------+
| BCL    | Battery charge demand (V/I loop)       | CCS    | Charger charge status (echo of setpoint) |
+--------+----------------------------------------+--------+------------------------------------------+
| BCS    | Battery charge status (V/I/SoC/temp)   |        |                                          |
+--------+----------------------------------------+--------+------------------------------------------+
| BSM    | Battery temperature status (max/min)   |        |                                          |
+--------+----------------------------------------+--------+------------------------------------------+
| BMV    | Battery min/max cell voltage           |        | (logged, not acted upon)                 |
+--------+----------------------------------------+--------+------------------------------------------+
| BMT    | Battery min/max cell temperature       |        | (logged, not acted upon)                 |
+--------+----------------------------------------+--------+------------------------------------------+
| BSP    | BMS stop preparation                   |        | (logged, not acted upon)                 |
+--------+----------------------------------------+--------+------------------------------------------+
| BST    | BMS stop transmission                  | CST    | Charger stop transmission                |
+--------+----------------------------------------+--------+------------------------------------------+
| BSD    | BMS session statistics                 | CSD    | Charger session statistics               |
+--------+----------------------------------------+--------+------------------------------------------+
| BEM    | BMS error message                      | CST    | Charger stop (fault, with fault code)    |
+--------+----------------------------------------+--------+------------------------------------------+
|        |                                        | CEM    | Charger error message (defined in DBC)   |
+--------+----------------------------------------+--------+------------------------------------------+

Hardcoded Charger Parameters
-----------------------------

The following charger capability parameters are hardcoded in ``CanBusToEvseManager.cpp`` and
announced to the BMS during the handshake phase. They can be changed by modifying the
``protocol->`` fields in ``ready()``:

.. list-table::
   :header-rows: 1
   :widths: 30 15 55

   * - Parameter
     - Value
     - Description
   * - ``charger_max_voltage_V``
     - 1000 V
     - Maximum output voltage advertised to the BMS in CHM
   * - ``charger_max_current_A``
     - 250 A
     - Maximum output current advertised to the BMS in CHM
   * - ``charger_type``
     - 1
     - Charger type code (1 = off-board DC charger per GB/T 27930)
   * - ``charger_temp_C``
     - 25 °C
     - Charger internal temperature reported back to the BMS in CCS

Interfaces
==========

Provides
--------

``can_signals`` (``can_signal_receiver``)
  Publishes two variables to connected modules (typically ``EvseManager``):

  - ``can_frame`` — the raw CAN frame object forwarded on every received frame, containing
    ``id`` (uint32), ``extended`` (bool), ``dlc`` (int), and ``data`` (byte array).
  - ``power_request`` — decoded BCL data published every time a BCL frame arrives during the
    Charging state. Fields:

    .. list-table::
       :header-rows: 1
       :widths: 20 15 65

       * - Field
         - Type
         - Description
       * - ``voltage_V``
         - float
         - Target output voltage requested by the BMS (V), clamped to ``charger_max_voltage_V``
       * - ``current_A``
         - float
         - Target output current requested by the BMS (A), clamped to ``charger_max_current_A``
       * - ``charge_mode``
         - int
         - GB/T 27930 charge mode: ``0`` = constant voltage (CV), ``1`` = constant current (CC)

Requires
--------

``can_bus`` (``CANBus``)
  Raw CAN bus access. The module subscribes to the ``last_frame`` variable for RX and calls
  ``send()`` for TX. Exactly one connection is required (``min_connections: 1``,
  ``max_connections: 1``).

Configuration
=============

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Parameter
     - Type / Default
     - Description
   * - ``log_frames``
     - boolean / ``true``
     - Log every received CAN frame at INFO level, including frame ID, DLC, raw data bytes,
       and current protocol state.

EvseManager Integration
=======================

``EvseManager`` was extended to optionally require the ``can_signal_receiver`` interface.
When connected, it subscribes to two vars:

``can_frame``
  Logs every received CAN frame. Also detects BST (frame ID ``0x101956F4``) to trigger
  ``powersupply_DC_off()``.

``power_request``
  On every BCL-derived power request:

  1. Resets the BCL watchdog timer.
  2. Calls ``powersupply_DC_on()`` if the PSU is not already on (first BCL = charging start).
  3. Calls ``powersupply_DC_set(voltage_V, current_A)`` to update the PSU output setpoint.
  4. Calls ``update_local_energy_limit()`` to keep the EnergyManager in sync.

BCL Watchdog
------------

A background thread in EvseManager checks every 500ms whether a BCL frame has been received
within the last 2 seconds. If the timeout is exceeded (BMS disconnected or simulator killed),
it calls ``powersupply_DC_off()`` automatically.

Log output on watchdog trigger::

    [EvseManager] *** GBT WATCHDOG *** No BCL for 2100ms — BMS disconnected, switching PSU OFF

Power Supply Control Flow
=========================

.. code-block:: text

    BCL arrives (BMS requests 450V / 150A)
        │
        ├─ GbtProtocol: update bms.requested_voltage_V / current_A
        ├─ GbtProtocol: TX CCS (echo back actual output)
        └─ publish_power_request({voltage_V:450, current_A:150, charge_mode:1})
                │
                └─ EvseManager::subscribe_power_request()
                        ├─ [first BCL] powersupply_DC_on()
                        │       └─ r_powersupply_DC[0]->call_setMode(Export, Charging)
                        │              → powersupply_dc: "Set mode: Export"
                        │              → evse_manager: "*** CHARGING START ***"
                        │
                        ├─ powersupply_DC_set(450, 150)
                        │       └─ r_powersupply_DC[0]->call_setExportVoltageCurrent(450, 150)
                        │              → powersupply_dc: "Set export voltage/current: 450V / 150A"
                        │
                        └─ update_local_energy_limit(67500W)

    BST arrives (BMS requests stop)
        │
        └─ EvseManager::subscribe_can_frame() detects 0x101956F4
                └─ powersupply_DC_off()
                        └─ r_powersupply_DC[0]->call_setMode(Off, Other)
                               → powersupply_dc: "Set mode: Off"
                               → evse_manager: "*** CHARGING STOP ***"

Key Log Messages
================

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Module
     - Log message
     - Meaning
   * - ``can_bus_bridge``
     - ``[GBT27930] BHM received``
     - BMS connected, handshake started
   * - ``can_bus_bridge``
     - ``[GBT27930] TX CHM``
     - Charger announced capabilities to BMS
   * - ``can_bus_bridge``
     - ``[GBT27930] BCP received``
     - Battery charge parameters received
   * - ``can_bus_bridge``
     - ``[GBT27930] Auto-transitioning to Charging``
     - BRO skipped by BMS, entering charging loop
   * - ``can_bus_bridge``
     - ``[CanBusToEvseManager] Publishing power request: 450V / 15A``
     - BCL decoded, forwarding power request to EvseManager
   * - ``evse_manager``
     - ``*** CHARGING START *** DC power supply ON``
     - First BCL received, PSU switched on
   * - ``evse_manager``
     - ``[PSU] EvseManager driving PSU: 450V / 15A``
     - PSU setpoint updated from BCL
   * - ``powersupply_dc``
     - ``Set export voltage/current: 450V / 15A``
     - PSU hardware command received
   * - ``powersupply_dc``
     - ``Output: 450V / 15A power=6750W mode=Export``
     - PSU actual output (published every 500ms)
   * - ``can_bus_bridge``
     - ``[GBT27930] BST received: stop_flag=1``
     - BMS requested stop
   * - ``evse_manager``
     - ``*** CHARGING STOP *** DC power supply OFF``
     - PSU switched off after BST or watchdog
   * - ``evse_manager``
     - ``*** GBT WATCHDOG *** No BCL for Xms``
     - BMS disconnected without sending BST

Files
=====

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - File
     - Description
   * - ``modules/CanBusToEvseManager/manifest.yaml``
     - Module manifest — interfaces, config schema
   * - ``modules/CanBusToEvseManager/CanBusToEvseManager.hpp``
     - Module class declaration
   * - ``modules/CanBusToEvseManager/CanBusToEvseManager.cpp``
     - Module implementation — CAN subscription, hardcoded charger parameters, power_request publishing
   * - ``modules/CanBusToEvseManager/GbtProtocol.hpp``
     - GB/T 27930 charger-side state machine (header-only)
   * - ``modules/CanBusToEvseManager/gbt27930.h``
     - DBC-generated codec header (cantools v41.2.1), all message structs and encode/decode functions
   * - ``modules/CanBusToEvseManager/gbt27930.c``
     - DBC-generated codec implementation (cantools v41.2.1)
   * - ``modules/CanBusToEvseManager/can_signals/can_signal_receiverImpl.hpp``
     - ``can_signal_receiver`` interface implementation header (publishes vars only, no logic)
   * - ``modules/CanBusToEvseManager/can_signals/can_signal_receiverImpl.cpp``
     - ``can_signal_receiver`` interface implementation (init/ready are no-ops)
   * - ``interfaces/can_signal_receiver.yaml``
     - Interface definition (``can_frame`` + ``power_request`` vars)
   * - ``config/config-sil-dc-canbus.yaml``
     - Example SIL configuration wiring all modules together

Example Configuration
=====================

Minimal wiring in a YAML config:

.. code-block:: yaml

    can_bus_hw:
      module: CanBus
      config_module:
        interface: can0        # or vcan0 for simulation
        bitrate: 500000

    can_bus_bridge:
      module: CanBusToEvseManager
      config_module:
        log_frames: true
      connections:
        can_bus:
          - module_id: can_bus_hw
            implementation_id: can_bus

    evse_manager:
      module: EvseManager
      config_module:
        charge_mode: DC
        # ... other DC config ...
      connections:
        # ... standard connections ...
        can_signal_receiver:
          - module_id: can_bus_bridge
            implementation_id: can_signals

GB/T 27930 Simulator
====================

A standalone BMS simulator can be used to test the module against a virtual CAN interface.
It simulates a battery pack connecting to the charger over a SocketCAN interface and is
expected to be built and run separately from the EVerest stack.

Features:

- Sends BHM on startup to initiate handshake
- Responds to CHM by starting BCL/BCS transmission
- Models battery voltage, current, SoC (coulomb counting), and temperature
- Applies CV region current tapering near max voltage
- Sends BST when SoC reaches 100% or on fault injection
- Sends BSD after BST to complete the session
- Prints all received charger messages (CHM, CRM, CTS, CML, CRO, CCS, CST, CSD)

Fault injection (keyboard input while running):

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Key
     - Fault
   * - ``o``
     - Overvoltage
   * - ``t``
     - Overtemperature
   * - ``c``
     - Communication loss
   * - ``n``
     - Clear fault

To run against a virtual CAN interface::

    # Create virtual CAN interface
    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan
    sudo ip link set up vcan0

    # Run EVerest with config-sil-dc-canbus.yaml
    manager --conf config-sil-dc-canbus.yaml

    # In another terminal, run the BMS simulator
    ./ev_simulator   # uses can0 by default — edit main() to use vcan0
