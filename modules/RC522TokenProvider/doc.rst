.. _everest_modules_handwritten_RC522TokenProvider:

*******************************************
RC522TokenProvider
*******************************************

:ref:`Link <everest_modules_RC522TokenProvider>` to the module's reference.

Overview
========

The ``RC522TokenProvider`` module reads RFID/NFC token identifiers from a serial-connected reader RC522-based reader and publishes them to the EVerest authorization framework via the ``auth_token_provider`` interface.

It supports three operating modes:

- **dummy** — publishes a static, pre-configured token without any hardware, useful for testing and development.
- **serial_push** — continuously reads from the serial port and immediately publishes every token it receives.
- **serial_on_auth_required** — waits for the EVSE to signal ``AuthRequired`` before reading from the serial port and publishing the next available token.

The module connects to an EVSE via the ``evse_manager`` interface to receive session events.

Operating Modes
===============

dummy
-----

In ``dummy`` mode, no serial port is opened. When the EVSE signals a ``SessionStarted`` event, the
module publishes the statically configured ``token`` value. This mode is intended for integration
testing when no physical reader is available.

serial_push
-----------

In ``serial_push`` mode, the module opens the configured serial device on startup and runs a
background thread that continuously reads lines from the port. Each time a complete line is
received (delimited by ``line_delimiter``), it is matched against ``token_regex``. If a token is
extracted, it is immediately published to the authorization framework. This mode is suited for
readers that push tokens spontaneously (e.g., tap-and-go).

serial_on_auth_required
-----------------------

In ``serial_on_auth_required`` mode, the serial port is opened on startup and the reader thread
runs continuously, queuing any received tokens internally. When the EVSE signals an
``AuthRequired`` event, the module waits (up to ``timeout`` seconds, or indefinitely if
``timeout`` is ``0``) for a token to appear in the queue before publishing it. This is useful for
installations where the authorization flow should be tightly coupled to a charge session start.

Serial Port Configuration
==========================

The serial port is opened and configured using standard POSIX ``termios`` settings. The module
supports the following parameters:

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Parameter
     - Default
     - Description
   * - ``serial_device``
     - *(empty)*
     - Path to the serial device, e.g. ``/dev/ttyUSB0`` or ``/dev/ttyACM0``. Required for ``serial_push`` and ``serial_on_auth_required`` modes.
   * - ``baudrate``
     - ``9600``
     - Serial baud rate. Supported values: ``9600``, ``19200``, ``38400``, ``57600``, ``115200``.
   * - ``data_bits``
     - ``8``
     - Number of data bits per frame (5–8).
   * - ``parity``
     - ``none``
     - Parity setting: ``none``, ``even``, or ``odd``.
   * - ``stop_bits``
     - ``1``
     - Number of stop bits: ``1`` or ``2``.
   * - ``line_delimiter``
     - ``\n``
     - Character sequence that marks the end of one token frame. The reader accumulates incoming bytes and splits on this delimiter.
   * - ``token_regex``
     - ``([0-9A-Fa-f]+)``
     - Regular expression applied to each received line. The **first capture group** is used as the token string. Extracted tokens are normalized to uppercase.

Token Configuration
===================

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Parameter
     - Default
     - Description
   * - ``token``
     - ``DEADBEEF``
     - Static token string. Used as the published token in ``dummy`` mode.
   * - ``type``
     - ``RFID``
     - Authorization type string, mapped to ``AuthorizationType``. Typically ``RFID``.
   * - ``timeout``
     - ``0``
     - In ``serial_on_auth_required`` mode: how long (in seconds) to wait for a token after ``AuthRequired``. ``0`` means wait indefinitely.
   * - ``connector_id``
     - ``0``
     - If greater than ``0``, the published token is restricted to this connector ID only.

Interfaces
==========

Provided
--------

- **main** (``auth_token_provider``) — publishes ``ProvidedIdToken`` objects to the EVerest
  authorization stack whenever a token is read or the dummy token is triggered.

Required
--------

- **evse** (``evse_manager``) — subscribes to ``SessionEvent`` notifications to detect
  ``AuthRequired`` (used in ``serial_on_auth_required`` mode) and ``SessionStarted``
  (used in ``dummy`` mode).

Published Token Format
======================

Every published token is a ``ProvidedIdToken`` with the following fields set:

- ``id_token.value`` — the token string (uppercase hex for serial modes, or the configured ``token`` for dummy mode).
- ``id_token.type`` — ``ISO14443`` (used for RFID-like card identifiers).
- ``authorization_type`` — mapped from the ``type`` configuration parameter (default: ``RFID``).
- ``parent_id_token`` — set to the same value as ``id_token``.
- ``connectors`` — populated only when ``connector_id > 0``.

Configuration Examples
======================

Dummy mode (testing)
--------------------

.. code-block:: yaml

   RC522TokenProvider:
     module: RC522TokenProvider
     config_module:
       main:
         mode: dummy
         token: AABBCCDD
         type: RFID
         connector_id: 0

Serial push mode
----------------

.. code-block:: yaml

   RC522TokenProvider:
     module: RC522TokenProvider
     config_module:
       main:
         mode: serial_push
         serial_device: /dev/ttyUSB0
         baudrate: 9600
         data_bits: 8
         parity: none
         stop_bits: 1
         line_delimiter: "\n"
         token_regex: "([0-9A-Fa-f]+)"
         type: RFID
         connector_id: 0

Serial on auth required mode
-----------------------------

.. code-block:: yaml

   RC522TokenProvider:
     module: RC522TokenProvider
     config_module:
       main:
         mode: serial_on_auth_required
         serial_device: /dev/ttyACM0
         baudrate: 115200
         data_bits: 8
         parity: none
         stop_bits: 1
         line_delimiter: "\n"
         token_regex: "([0-9A-Fa-f]+)"
         type: RFID
         timeout: 30
         connector_id: 1

Implementation Notes
====================

- The serial reader runs in a dedicated background thread. On module shutdown, the thread is
  signalled via the ``running_`` atomic flag and joined cleanly in the destructor.
- Read calls on the serial port use a 1-second ``VTIME`` timeout so the thread wakes periodically
  to check the ``running_`` flag even when no data arrives.
- Partial frames are accumulated in an internal buffer (``pending_``) and only processed once a
  complete delimiter-terminated line is received. Both ``\n`` and ``\r\n`` line endings are handled.
- In ``serial_on_auth_required`` mode, tokens received before ``AuthRequired`` is signalled are
  held in an internal queue. Only the first queued token is consumed per ``AuthRequired`` event.
- If the ``token_regex`` is invalid at startup, the module falls back to the default hex pattern
  ``([0-9A-Fa-f]+)`` and logs an error.
