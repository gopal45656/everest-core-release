.. _everest_modules_handwritten_Chademo:

*******************************************
Chademo
*******************************************

:ref:`Link <everest_modules_Chademo>` to the module's reference.

Overview
========

The ``Chademo`` module implements the **CHAdeMO DC fast charging protocol** within EVerest.
It handles the full CHAdeMO handshake and charging session over a SocketCAN interface via
the ``CANBus`` module, communicates with the EV's Battery Management System (BMS), and
coordinates with the board support package (BSP) for contactor control.

The module does **not** drive the DC power supply directly. Instead, it publishes
``power_request`` objects via the ``can_signal_receiver`` interface. An ``EvseManager``
instance (or equivalent) subscribes to that variable and translates it into
``powersupply_DC_set()`` calls.

.. code-block:: text

    EV (BMS)
      │  CAN frames (0x100, 0x101, 0x102) — 11-bit standard IDs
      ▼
    CANBus module  (r_can_bus)
      │  last_frame var → subscribe_last_frame callback
      ▼
    Chademo module
      ├─ p_main  (chademo interface)
      │    └─ handle_start_charging() / handle_stop_charging()
      │
      ├─ p_can_signals  (can_signal_receiver interface)
      │    ├─ publish_can_frame()      ──────────────────────► EvseManager (stop detection)
      │    └─ publish_power_request()  ──────────────────────► EvseManager → PSU set-point
      │
      └─ r_bsp  (evse_board_support interface)
           ├─ call_enable(true/false)          — EVSE enable/disable
           └─ call_allow_power_on({true/false}) — contactor open/close


Architecture
============

On ``init()``, the module opens the CAN interface via ``r_can_bus->call_open("main_mcan0")``
and subscribes to ``last_frame``. Every received frame:

1. Resets the 1-second CAN watchdog.
2. Is forwarded raw to EvseManager via ``p_can_signals->publish_can_frame()``.
3. Is dispatched into the state machine via ``handle_incoming_can()``.

On ``ready()``, a background thread is started that runs ``run_state_machine()`` in a loop
at ``publish_interval_ms`` intervals.

CHAdeMO CAN Frame Map
=====================

All CHAdeMO frames use **11-bit standard CAN IDs** (not extended).

EV → EVSE frames (received)
-----------------------------

**0x100 — EV Battery Parameters**

Decoded in ``WAIT_FOR_EV_100`` state only.

.. list-table::
   :header-rows: 1
   :widths: 10 10 80

   * - Byte
     - Byte
     - Field
   * - 0
     - 1
     - Target charge voltage (V), little-endian uint16: ``(data[1] << 8) | data[0]``
   * - 2
     - —
     - Maximum charge current (A), uint8

**0x101 — EV Status / SOC**

Decoded in ``WAIT_FOR_CONFIRMATION`` state only. SOC is read from ``data[2]``.

**0x102 — EV Control**

Decoded in any state.

.. list-table::
   :header-rows: 1
   :widths: 10 20 70

   * - Byte
     - Bit / value
     - Field
   * - 0
     - bit 0
     - EV contactors closed (``data[0] & 0x01``). Triggers ``CHARGING_OPERATIONAL`` when set in ``WAIT_FOR_102``.
   * - 4
     - 0x01
     - EV overtemperature / fault flag. Triggers ``FAULT`` state immediately from any state.

EVSE → EV frames (transmitted)
--------------------------------

**0x108 — EVSE Heartbeat**

Sent on every loop tick in most states. Layout:

.. list-table::
   :header-rows: 1
   :widths: 10 90

   * - Byte
     - Content
   * - 0
     - Charger status: always ``0x01``
   * - 1
     - Max output voltage low byte (little-endian)
   * - 2
     - Max output voltage high byte (little-endian)
   * - 3
     - Max output current (A)
   * - 4
     - IN1 signal: ``0x01`` = HIGH (charger available), ``0x00`` = LOW
   * - 5
     - Charger temperature (°C), hardcoded to 35

**0x109 — EVSE Parameters / Permission**

Sent when asserting or revoking charge permission. Layout:

.. list-table::
   :header-rows: 1
   :widths: 10 90

   * - Byte
     - Content
   * - 0
     - Fault flag: ``0x01`` = fault present, ``0x00`` = normal
   * - 1
     - IN2 (charge permission): ``0x01`` = permission granted, ``0x00`` = revoked
   * - 2
     - Max voltage low byte (little-endian)
   * - 3
     - Max voltage high byte (little-endian)
   * - 4
     - Max current (A)
   * - 5
     - Reserved (``0x00``)
   * - 6
     - Hardcoded ``0x02``

State Machine
=============

The ``EVSE_STATE`` enum drives the charging session. The background thread advances
through states on every loop tick or in response to incoming CAN frames.

.. code-block:: text

    WAIT_FOR_PLUG
      │  gpioget reads /dev/gpiochipN line M — LOW = EV present
      │  r_bsp->call_enable(true), heartbeat 0x108 started
      ▼
    WAIT_FOR_EV_100
      │  Sends 0x108 every tick
      │  Polls GPIO — if pin goes HIGH: unplug detected, back to WAIT_FOR_PLUG
      │  Waits for 0x100 from EV
      ▼
    SEND_EVSE_PARAMS_108  (triggered by 0x100 receipt)
      │  Logs: "Scaling Power Supply to match EV Battery Voltage..."
      │  Sends 0x108
      ▼
    PRECHARGE
      │  Sends 0x108, resets watchdog
      ▼
    WAIT_FOR_CONFIRMATION
      │  Sends 0x108 every tick
      │  Watchdog active from here — 1s timeout → FAULT
      │  Waits for 0x101 from EV (SOC)
      ▼
    SEND_IN2_109  (triggered by 0x101 receipt)
      │  Sends 0x109 with IN2=HIGH (permission granted)
      │  r_bsp->call_allow_power_on({allow_power_on: true})
      ▼
    WAIT_FOR_102
      │  Sends 0x108 + 0x109 (permission) every tick
      │  Sends BSP power-on command once (guarded by bsp_signal_sent flag)
      │  Waits for 0x102 with contactors-closed bit
      ▼
    CHARGING_OPERATIONAL  (triggered by 0x102 contactors-closed bit)
      │  Calls handle_start_charging() on every tick
      │  Ramps simulated_actual_v by +5V per tick until target reached
      │  Publishes power_request {voltage_V, current_A, charge_mode:0} every tick
      │  Sends 0x108 with current simulated voltage every tick
      │  Logs power update every 2 seconds
      │  When simulated_actual_v >= target OR SoC >= 100% → FINISHED
      ▼
    FINISHED
      │  handle_stop_charging()
      │  Publishes power_request {voltage_V:0, current_A:0} → PSU off signal
      │  Sends 0x109 with IN2=LOW → EV opens contactors
      │  r_bsp->call_allow_power_on({allow_power_on: false})
      │  running = false → thread exits
      ▼
    (session complete)

    FAULT (from any state except WAIT_FOR_PLUG, WAIT_FOR_EV_100, SEND_EVSE_PARAMS_108)
      │  Sends 0x109 with fault=true
      │  handle_stop_charging()
      │  running = false → thread exits

.. note::
   The enum also contains ``HANDSHAKE``, ``CHG_PARAMETERS``, and ``CHARGING`` states
   that are defined but never reached in the current implementation. They are dead code.

Voltage Ramp
------------

During ``CHARGING_OPERATIONAL``, a ``static float simulated_actual_v`` local to
``handle_start_charging()`` starts at 0.0 V and increases by 5.0 V per loop tick
(every ``publish_interval_ms`` ms) until it reaches the EV's requested target voltage.

On every tick, a ``power_request`` is published with the current ``simulated_actual_v``
and the EV's requested current. The EvseManager receives this and calls
``powersupply_DC_set(simulated_actual_v, target_i)`` on the DC PSU — this is the
soft-start ramp.

.. note::
   Because ``simulated_actual_v`` is a ``static`` local variable, it does **not** reset
   between calls. If ``handle_start_charging()`` is ever re-entered after a partial session,
   the ramp will continue from where it left off rather than starting from 0 V.

Power Request Publishing
========================

The ``can_signals`` (``can_signal_receiver``) interface is the integration point
between this module and EvseManager:

- **During charging** — ``handle_start_charging()`` publishes on every loop tick:

  .. code-block:: json

     { "voltage_V": <simulated_actual_v>, "current_A": <ev_req_i>, "charge_mode": 0 }

  ``charge_mode: 0`` indicates constant current (CC) mode.

- **On stop or fault** — ``handle_stop_charging()`` publishes a zero request to signal
  EvseManager to turn the PSU off:

  .. code-block:: json

     { "voltage_V": 0.0, "current_A": 0.0, "charge_mode": 0 }

- **Every received frame** — the raw CAN frame object is forwarded via
  ``publish_can_frame()`` so EvseManager can detect stop conditions (e.g. 0x102 fault).

Safety Features
===============

CAN Watchdog
------------

Active from ``WAIT_FOR_CONFIRMATION`` state onwards (not active during
``WAIT_FOR_PLUG``, ``WAIT_FOR_EV_100``, ``SEND_EVSE_PARAMS_108``, or ``FAULT``).

If no CAN frame is received for **1 second**, the module transitions to ``FAULT``::

    Emergency: CAN Timeout (1s). Car disconnected or crashed.

EV Fault Detection
------------------

Frame 0x102, ``data[4] == 0x01`` is interpreted as an EV overtemperature or fault
condition. This triggers an immediate transition to ``FAULT`` from **any** state::

    EV reported Overtemp/Fault!

GPIO Presence Detection
-----------------------

EV presence is read by calling ``gpioget`` as a subprocess:

- In ``WAIT_FOR_PLUG``: uses path ``/dev/gpiochipN``
- In ``WAIT_FOR_EV_100``: uses path ``gpiochipN`` (no ``/dev/`` prefix)

.. warning::
   There is a path inconsistency between the two states. ``WAIT_FOR_PLUG`` uses
   ``/dev/gpiochipN`` (correct) while ``WAIT_FOR_EV_100`` uses ``gpiochipN`` (missing
   ``/dev/`` prefix). This may cause the unplug detection to silently fail on some systems.

Interfaces
==========

Provides
--------

``main`` (``chademo``)
  Exposes ``start_charging()`` and ``stop_charging()`` commands that map to
  ``handle_start_charging()`` and ``handle_stop_charging()``. The state machine calls
  these internally; external callers may also invoke them.

``can_signals`` (``can_signal_receiver``)
  Publishes two variables:

  - ``can_frame`` — raw CAN frame forwarded on every received EV frame.
  - ``power_request`` — published every loop tick during charging and once on stop.
    Fields: ``voltage_V`` (float), ``current_A`` (float), ``charge_mode`` (int, always 0).

  The ``init()`` and ``ready()`` implementations are no-ops; all publishing is driven by
  the parent module.

Requires
--------

``bsp`` (``evse_board_support``)
  Used for EVSE enable/disable and contactor control:

  - ``call_enable(true)`` — called on EV detection
  - ``call_enable(false)`` — called on unplug detection
  - ``call_allow_power_on({allow_power_on: true})`` — closes contactors / asserts IN2
  - ``call_allow_power_on({allow_power_on: false})`` — opens contactors on stop/fault

``can_bus`` (``CANBus``)
  Raw CAN bus access. Exactly one connection required (``min_connections: 1``,
  ``max_connections: 1``).

  - ``call_open("main_mcan0")`` — opens the interface on init (interface name is
    hardcoded in ``chademoImpl.cpp``, not read from config)
  - ``call_send(id, extended, dlc, data)`` — transmits 0x108 and 0x109 frames
  - ``subscribe_last_frame(callback)`` — receives all EV frames

  .. note::
     The CAN interface name ``"main_mcan0"`` is hardcoded in ``chademoImpl::init()``
     and is not driven by any configuration parameter, despite the intent suggested by
     the ``can_interface`` config key described in some documentation. The config
     key does not exist in ``manifest.yaml``.

Configuration
=============

.. list-table::
   :header-rows: 1
   :widths: 30 15 15 40

   * - Parameter
     - Type
     - Default
     - Description
   * - ``max_voltage_limit``
     - number
     - ``500.0``
     - Maximum output voltage advertised to the EV in 0x108 and 0x109 (V)
   * - ``max_current_limit``
     - number
     - ``50.0``
     - Maximum output current advertised to the EV in 0x108 and 0x109 (A)
   * - ``publish_interval_ms``
     - integer
     - ``100``
     - State machine loop interval (ms). Controls CAN heartbeat rate and ramp step rate.
   * - ``presence_gpio_chip``
     - integer
     - ``2``
     - GPIO chip index for EV presence detection (used to build ``/dev/gpiochipN``)
   * - ``presence_gpio_line``
     - integer
     - ``36``
     - GPIO line number for EV presence detection

.. note::
   There is no ``can_interface`` or ``bitrate`` config parameter in ``manifest.yaml``.
   The CAN interface name is hardcoded as ``"main_mcan0"`` in the implementation.

Key Log Messages
================

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Log message
     - Meaning
   * - ``CHAdeMO Module Initialized — using CanBus module for CAN``
     - init() complete, CAN subscription set up
   * - ``Vehicle Detected via /dev/gpiochipX``
     - GPIO pin LOW detected, EV present
   * - ``RX 0x100: EV Req 300V, 20A``
     - EV sent target voltage and current
   * - ``Scaling Power Supply to match EV Battery Voltage...``
     - Transition from WAIT_FOR_EV_100 to PRECHARGE
   * - ``RX 0x101 Data: [xx xx ...] | SOC: 55%``
     - EV battery status with raw hex bytes and SoC
   * - ``Signal d1 (Charge Permission) HIGH via BSP``
     - 0x109 IN2=HIGH sent, BSP contactor asserted
   * - ``BSP Power-On command sent once.``
     - Deduplicated BSP power-on in WAIT_FOR_102
   * - ``Relays Closed. Commencing Power Delivery.``
     - 0x102 contactors-closed bit received, transitioning to CHARGING_OPERATIONAL
   * - ``[CHAdeMO] Published power_request: 50V / 20A``
     - power_request published to EvseManager (ramp in progress)
   * - ``>>> [POWER UPDATE] <<< Target: 6 kW / Actual: 0.25 kW``
     - Periodic ramp progress (logged every 2 seconds)
   * - ``!!! TARGET REACHED !!! Final Power: 6 kW at 300V``
     - Ramp complete, transitioning to FINISHED
   * - ``Shutting down DC Power Supply.``
     - Stop sequence started
   * - ``Physical Contactors OPENED via BSP.``
     - BSP allow_power_on(false) called, session fully terminated
   * - ``Emergency: CAN Timeout (1s). Car disconnected or crashed.``
     - CAN watchdog fired, transitioning to FAULT
   * - ``EV reported Overtemp/Fault!``
     - 0x102 fault flag set, transitioning to FAULT
   * - ``Plug removed! Resetting.``
     - GPIO pin HIGH during WAIT_FOR_EV_100, unplug detected

Example Configuration
=====================

``config/chademo_config.yaml``:

.. code-block:: yaml

    active_modules:
      chademo:
        module: Chademo
        config_module:
          max_voltage_limit: 500
          max_current_limit: 50
          publish_interval_ms: 100
          presence_gpio_chip: 2
          presence_gpio_line: 36
        connections:
          bsp:
            - implementation_id: board_support
              module_id: Yeti
          can_bus:
            - implementation_id: can_bus
              module_id: CanBusHW

      CanBusHW:
        module: CanBus
        config_module:
          interface: main_mcan0
          bitrate: 500000

      Yeti:
        module: YetiSimulator
        config_module:
          connector_id: 1

EV Simulator — ev_chademo
==========================

A standalone CHAdeMO EV simulator is provided at ``ev_chademo/``. It runs on a
separate board (e.g. ruggedboard-imx6ul) connected to the same CAN bus.

EV State Machine (``CHADEMOSTATE``):

.. code-block:: text

    STOPPED
      │  IN1 detected (0x108 received with IN1=HIGH)
      ▼
    STARTUP → SEND_INITIAL_PARAMS
      │  Sends 0x100 (target V/I), 0x101 (SOC), 0x102 (status)
      ▼
    SET_CHARGE_BEGIN
      │  "[EV] EVSE Compatible. Setting OUT1 (Car Ready)."
      ▼
    WAIT_FOR_BEGIN_CONFIRMATION
      │  Waits for IN2=HIGH in 0x109
      │  "[RX 0x109] Permission: YES (IN2 HIGH)"
      ▼
    RUNNING
      │  Sends periodic 0x100/0x101/0x102 at 100ms
      │  Charging status printed every 2 seconds
      │  Stops when IN2 goes LOW (EVSE revokes permission)
      ▼
    CEASE_CURRENT
      │  "[EV] EVSE Revoked Permission (IN2=0). Stopping Charge..."
      ▼
    (session complete)

Default EV parameters:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Parameter
     - Value
   * - Target charge voltage
     - 300 V
   * - Max charge current
     - 20 A
   * - Min charge current
     - 5 A
   * - Pack size
     - 20 kWh
   * - Starting SoC
     - 55%
   * - Overtemperature threshold
     - 60°C

To build and run::

    # On the EV board (ruggedboard-imx6ul)
    cd ev_chademo
    g++ -o chademo_test main.cpp chademo.cpp -lpthread
    ./chademo_test

Hardware Setup
==============

.. code-block:: text

    phyboard-lyra-am62xx (EVSE)              ruggedboard-imx6ul (EV Simulator)
    ┌──────────────────────────────┐          ┌────────────────────────────┐
    │  EVerest                     │          │  ev_chademo                │
    │  ├─ Chademo module           │          │  └─ chademo_test binary    │
    │  ├─ CanBus module (mcan0)    │          └──────────┬─────────────────┘
    │  └─ YetiSimulator (BSP)      │                     │
    │                              │    CAN bus (main_mcan0 / can0)
    │  main_mcan0 ─────────────────┼─────────────────────┘
    │  gpiochip2 line 36 ──────────┼─── EV presence signal (active LOW)
    └──────────────────────────────┘

Files
=====

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - File
     - Description
   * - ``modules/EVSE/Chademo/manifest.yaml``
     - Module manifest — interfaces, config schema
   * - ``modules/EVSE/Chademo/Chademo.hpp/cpp``
     - Module class — init/ready delegate to p_main and p_can_signals
   * - ``modules/EVSE/Chademo/main/chademoImpl.hpp``
     - EVSE state machine, CAN handler declarations, atomic signal variables
   * - ``modules/EVSE/Chademo/main/chademoImpl.cpp``
     - Full state machine, CAN TX/RX, BSP control, power_request publishing
   * - ``modules/EVSE/Chademo/can_signals/can_signal_receiverImpl.hpp/cpp``
     - can_signal_receiver interface stub (init/ready are no-ops)
   * - ``config/chademo_config.yaml``
     - EVerest configuration for CHAdeMO charging
   * - ``ev_chademo/main.cpp``
     - EV simulator entry point and default parameters
   * - ``ev_chademo/chademo.cpp``
     - EV-side CHAdeMO state machine
   * - ``ev_chademo/chademo.h``
     - EV state definitions and CAN ID constants
