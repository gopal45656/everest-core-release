.. _chademo_module:

Chademo
=======

Overview
--------

The ``Chademo`` module implements the **CHAdeMO DC fast charging protocol** within EVerest.
It handles the full CHAdeMO handshake and charging session over a SocketCAN interface,
communicating directly with the EV's Battery Management System (BMS) and coordinating
with the DC power supply and board support package (BSP).

.. code-block:: text

    EV (BMS)
      │  CAN frames (0x100, 0x101, 0x102)
      ▼
    CAN Bus (main_mcan0, 500 kbps)
      │
      ▼
    Chademo module
      ├─ r_power_supply (power_supply_DC) ──► InfyPower_BEG1K075G
      │                                           └─ CAN → Physical PSU
      └─ r_bsp (evse_board_support)       ──► YetiSimulator
                                                  └─ Contactor control / GPIO

Architecture
------------

The module runs a single background thread (``run_state_machine()``) that:

1. Reads incoming CAN frames from the EV in a non-blocking loop
2. Drives the EVSE-side CHAdeMO state machine
3. Sends periodic heartbeat frames (0x108, 0x109) to the EV
4. Controls the power supply and contactors via EVerest interfaces

CHAdeMO Protocol — CAN Message Map
------------------------------------

+--------+------------------+----------+------------------------------------------+
| CAN ID | Direction        | Name     | Description                              |
+========+==================+==========+==========================================+
| 0x100  | EV → EVSE        | EV Batt  | Target voltage, current, SOC, temp       |
+--------+------------------+----------+------------------------------------------+
| 0x101  | EV → EVSE        | EV Time  | Remaining charge time, target SOC        |
+--------+------------------+----------+------------------------------------------+
| 0x102  | EV → EVSE        | EV Ctrl  | Contactor status, fault flags            |
+--------+------------------+----------+------------------------------------------+
| 0x108  | EVSE → EV        | EVSE Hb  | Charger status, present V/I, IN1 signal  |
+--------+------------------+----------+------------------------------------------+
| 0x109  | EVSE → EV        | EVSE Par | IN2 (charge permission), max V/I, faults |
+--------+------------------+----------+------------------------------------------+

State Machine
-------------

The EVSE-side state machine (``EVSE_STATE``) drives the charging session:

.. code-block:: text

    WAIT_FOR_PLUG
      │  GPIO detects EV presence (pin goes LOW)
      │  BSP enabled, heartbeat 0x108 started
      ▼
    WAIT_FOR_EV_100
      │  EV sends 0x100 (target voltage/current)
      ▼
    SEND_EVSE_PARAMS_108
      │  Log: "Scaling Power Supply to match EV Battery Voltage"
      ▼
    PRECHARGE
      │  Continue sending 0x108
      ▼
    WAIT_FOR_CONFIRMATION
      │  EV sends 0x101 (SOC received)
      ▼
    SEND_IN2_109
      │  Send 0x109 with IN2=HIGH (charge permission)
      │  BSP: call_allow_power_on(true)
      │  Log: "Signal d1 (Charge Permission) HIGH via BSP"
      ▼
    WAIT_FOR_102
      │  EV sends 0x102 with contactor closed bit
      │  Log: "Relays Closed. Commencing Power Delivery."
      ▼
    CHARGING_OPERATIONAL  ──► handle_start_charging()
      │  PSU: setMode(Export, Charging)
      │  PSU: setExportVoltageCurrent(V, I) — ramps up 5V per loop
      │  Log: ">>> [POWER UPDATE] <<< Target: X kW / Actual: Y kW"
      │  When V reaches target: Log: "!!! TARGET REACHED !!!"
      ▼
    FINISHED
      │  handle_stop_charging()
      │  PSU: IN2 LOW → EV opens contactors
      │  BSP: call_allow_power_on(false)
      │  Log: "Shutting down DC Power Supply."
      │  Log: "Physical Contactors OPENED via BSP."
      ▼
    (session complete)

    FAULT (any state)
      │  CAN timeout (1s watchdog) or EV fault flag in 0x102
      └─ handle_stop_charging() → FAULT state

Voltage Ramp
~~~~~~~~~~~~

During ``CHARGING_OPERATIONAL``, the module ramps the output voltage from 0V to the
EV's requested voltage in 5V steps per loop iteration (``publish_interval_ms``).
This simulates a soft-start precharge ramp. The power supply receives a new
``setExportVoltageCurrent`` call on every loop tick until the target is reached.

Interfaces
----------

Provides
~~~~~~~~

``main`` (``chademo``)
  Exposes ``start_charging()`` and ``stop_charging()`` commands. These are called
  internally by the state machine — external callers can also trigger them.

Requires
~~~~~~~~

``power_supply`` (``power_supply_DC``)
  Used to control the DC output:

  - ``call_setMode(Export, Charging)`` — enables output
  - ``call_setExportVoltageCurrent(V, I)`` — sets voltage/current setpoint
  - ``call_setMode(Off, Other)`` — disables output on stop/fault

``bsp`` (``evse_board_support``)
  Used for contactor and enable control:

  - ``call_enable(true/false)`` — enables/disables the EVSE
  - ``call_allow_power_on({allow_power_on: true/false})`` — closes/opens contactors

Configuration
-------------

.. list-table::
   :header-rows: 1

   * - Parameter
     - Type
     - Default
     - Description
   * - ``can_interface``
     - string
     - ``main_mcan0``
     - SocketCAN interface name
   * - ``bitrate``
     - integer
     - ``500000``
     - CAN bus bitrate in bps
   * - ``max_voltage_limit``
     - number
     - ``500.0``
     - Maximum output voltage advertised to EV (V)
   * - ``max_current_limit``
     - number
     - ``50.0``
     - Maximum output current advertised to EV (A)
   * - ``publish_interval_ms``
     - integer
     - ``100``
     - State machine loop interval in milliseconds
   * - ``presence_gpio_chip``
     - integer
     - ``2``
     - GPIO chip index for EV presence detection (e.g. ``/dev/gpiochip2``)
   * - ``presence_gpio_line``
     - integer
     - ``36``
     - GPIO line number for EV presence detection

Safety Features
---------------

**CAN Watchdog**
  If no CAN frame is received from the EV for 1 second during an active session,
  the module triggers an emergency stop::

    Emergency: CAN Timeout (1s). Car disconnected or crashed.

**EV Fault Detection**
  If the EV sets the overtemperature/fault bit in frame 0x102 (byte 4 = 0x01),
  the module immediately transitions to ``FAULT`` state and stops charging.

**GPIO Presence Check**
  EV presence is detected by reading a GPIO pin via ``gpioget``. The pin going
  LOW indicates the EV is connected. If the pin goes HIGH during ``WAIT_FOR_EV_100``,
  the session is reset.

Key Log Messages
----------------

.. list-table::
   :header-rows: 1

   * - Log message
     - Meaning
   * - ``Vehicle Detected via /dev/gpiochipX``
     - EV plug-in detected via GPIO
   * - ``RX 0x100: EV Req 300V, 20A``
     - EV sent its target voltage and current
   * - ``RX 0x101 Data: [...] | SOC: 55%``
     - EV battery status received with raw bytes
   * - ``Signal d1 (Charge Permission) HIGH via BSP``
     - IN2 asserted, BSP contactors enabled
   * - ``Relays Closed. Commencing Power Delivery.``
     - EV confirmed contactors closed (0x102), charging begins
   * - ``>>> [POWER UPDATE] <<< Target: 6 kW / Actual: 2.1 kW``
     - Periodic ramp progress log (every 2 seconds)
   * - ``!!! TARGET REACHED !!! Final Power: 6 kW at 300V``
     - Voltage ramp complete, target reached
   * - ``Shutting down DC Power Supply.``
     - Stop sequence initiated
   * - ``Physical Contactors OPENED via BSP.``
     - Session fully terminated

Power Supply Module — InfyPower BEG1K075G
------------------------------------------

The ``InfyPower_BEG1K075G`` module is the hardware driver for the InfyPower BEG1K075G
AC/DC power supply. It communicates with the physical unit over a dedicated CAN bus
using a proprietary protocol (``can_driver_acdc``).

Capabilities:

- Max export: 1000V / 73.3A / 22 kW
- Min export: 200V / 1A
- Bidirectional (supports V2G import mode)
- Conversion efficiency: 95% export, 95% import

The driver translates EVerest ``power_supply_DC`` interface calls into CAN commands:

.. list-table::
   :header-rows: 1

   * - EVerest call
     - Hardware action
   * - ``setMode(Export)``
     - ``switch_on_off(true)`` + ``set_inverter_mode(false)``
   * - ``setMode(Import)``
     - ``set_inverter_mode(true)`` + ``switch_on_off(true)``
   * - ``setMode(Off)``
     - ``switch_on_off(false)``
   * - ``setExportVoltageCurrent(V, I)``
     - ``set_voltage_current(V, I)`` via CAN
   * - ``setImportVoltageCurrent(V, I)``
     - ``set_voltage_current(V, I)`` via CAN

Log output during ramp::

    DCSupply :: Updating voltage/current via CAN: 10V / 20A
    DCSupply :: Updating voltage/current via CAN: 15V / 20A
    ...
    DCSupply :: Updating voltage/current via CAN: 300V / 20A

EV Simulator — ev_chademo
--------------------------

A standalone CHAdeMO EV simulator is provided at ``ev_chademo/``. It runs on a
separate board (e.g. ruggedboard-imx6ul) connected to the same CAN bus.

Files:

- ``main.cpp`` — entry point, CAN socket setup, main loop
- ``chademo.cpp`` — CHAdeMO EV-side state machine
- ``chademo.h`` — state definitions, data structures, CAN IDs

EV State Machine (``CHADEMOSTATE``):

.. code-block:: text

    STOPPED
      │  IN1 detected (0x108 with IN1=HIGH received)
      ▼
    STARTUP → SEND_INITIAL_PARAMS
      │  Sends 0x100 (target V/I), 0x101 (SOC), 0x102 (status)
      ▼
    SET_CHARGE_BEGIN
      │  Log: "[EV] EVSE Compatible. Setting OUT1 (Car Ready)."
      ▼
    WAIT_FOR_BEGIN_CONFIRMATION
      │  Waits for IN2=HIGH in 0x109
      │  Log: "[RX 0x109] Permission: YES (IN2 HIGH)"
      ▼
    RUNNING
      │  Sends periodic 0x100/0x101/0x102 at 100ms
      │  Prints charging status every 2 seconds:
      │    Voltage: X V / Current: Y A / Power: Z kW / SOC: N%
      │  Stops when IN2 goes LOW
      ▼
    CEASE_CURRENT
      │  Log: "[EV] EVSE Revoked Permission (IN2=0). Stopping Charge..."
      │  Log: "[EV] Target reached or EVSE Stop. Opening Contactors."
      ▼
    (session complete)

Default EV parameters (set in ``main.cpp``):

.. list-table::
   :header-rows: 1

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

Configuration File
------------------

``config/chademo_config.yaml``:

.. code-block:: yaml

    active_modules:
      chademo:
        module: Chademo
        config_module:
          can_interface: "main_mcan0"
          bitrate: 500000
          max_voltage_limit: 500
          max_current_limit: 50
          presence_gpio_chip: 2
          presence_gpio_line: 36
        connections:
          bsp:
            - implementation_id: board_support
              module_id: Yeti
          power_supply:
            - implementation_id: main
              module_id: DCSupply

      DCSupply:
        module: InfyPower_BEG1K075G
        config_module:
          can_device: main_mcan0

      Yeti:
        module: YetiSimulator
        config_module:
          connector_id: 1

Hardware Setup
--------------

.. code-block:: text

    phyboard-lyra-am62xx (EVSE)          ruggedboard-imx6ul (EV Simulator)
    ┌─────────────────────────┐           ┌──────────────────────────┐
    │  EVerest                │           │  ev_chademo              │
    │  ├─ Chademo module      │           │  └─ chademo_test binary  │
    │  ├─ InfyPower driver    │           └──────────┬───────────────┘
    │  └─ YetiSimulator       │                      │
    │                         │           CAN bus (can0 / main_mcan0)
    │  main_mcan0 ────────────┼───────────────────────┘
    │  gpiochip2 line 36 ─────┼── EV presence wire
    └─────────────────────────┘

Files
-----

.. list-table::
   :header-rows: 1

   * - File
     - Description
   * - ``modules/EVSE/Chademo/manifest.yaml``
     - Module manifest — interfaces, config schema
   * - ``modules/EVSE/Chademo/Chademo.hpp/cpp``
     - Module class — wires init/ready to implementation
   * - ``modules/EVSE/Chademo/main/chademoImpl.hpp/cpp``
     - CHAdeMO state machine, CAN TX/RX, PSU/BSP control
   * - ``modules/HardwareDrivers/PowerSupplies/InfyPower_BEG1K075G/``
     - InfyPower hardware driver (CAN-based AC/DC PSU)
   * - ``config/chademo_config.yaml``
     - EVerest configuration for CHAdeMO charging
   * - ``ev_chademo/main.cpp``
     - EV simulator entry point
   * - ``ev_chademo/chademo.cpp``
     - EV-side CHAdeMO state machine
   * - ``ev_chademo/chademo.h``
     - EV state definitions and CAN ID constants
