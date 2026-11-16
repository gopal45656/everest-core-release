.. _everest_modules_handwritten_CanBus:

*******************************************
CanBus
*******************************************

:ref:`Link <everest_modules_CanBus>` to the module's reference.

Generic CAN bus interface module for EVSE communication. Provides raw CAN
frame transmit/receive functionality over Linux SocketCAN. Higher-level
protocol modules — such as GB/T 27930 DC, CHAdeMO, or diagnostic services —
connect to this module to send and receive CAN frames without managing the
socket themselves.

Overview
========

The CanBus module wraps a single SocketCAN interface (``PF_CAN / SOCK_RAW /
CAN_RAW``) and exposes it through the EVerest ``CANBus`` interface. It handles:

* bringing the CAN interface up at the configured bitrate via ``ip link``
* opening and binding the raw CAN socket
* a background receive thread that publishes every incoming frame via the
  ``last_frame`` signal
* a synchronous ``send`` command for outgoing frames
* a synchronous ``receive`` command for one-shot polling (non-blocking read)
* clean teardown on ``close`` — socket is closed and the RX thread is joined

The module provides one interface implementation: ``can_bus``.

Startup sequence
================

``init()`` calls ``handle_open`` with the configured interface name. Inside
``handle_open``:

1. The interface is taken down with ``ip link set <if> down`` so the bitrate
   can be changed on a live interface (the kernel rejects bitrate changes on a
   running interface).
2. The interface is brought back up with ``ip link set <if> up type can bitrate
   <bitrate>``. If this command fails (e.g. the interface is already up with
   the same bitrate), the driver logs a warning but continues — the socket
   open may still succeed.
3. The TX queue length is set to 1000 via ``ip link set <if> txqueuelen 1000``
   to prevent ``ENOBUFS`` errors when multiple sockets share the same
   interface.
4. A ``PF_CAN / SOCK_RAW / CAN_RAW`` socket is created, bound to the
   interface index, and stored.
5. ``publish_is_open(true)`` is called to signal readiness to connected modules.

``ready()`` starts the background RX thread.

Receive thread
==============

The RX thread uses ``poll()`` with a timeout of ``recv_timeout_ms``
milliseconds to wait for incoming data. On each ``POLLIN`` event it reads one
``struct can_frame`` and publishes it via ``publish_last_frame`` with the
following fields:

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Field
     - Type
     - Description
   * - ``id``
     - integer
     - CAN ID, masked to 29 bits (``CAN_EFF_MASK``). Both standard (11-bit)
       and extended (29-bit) IDs land here; check ``extended`` to distinguish.
   * - ``extended``
     - boolean
     - ``true`` when the ``CAN_EFF_FLAG`` bit was set in the raw frame ID.
   * - ``dlc``
     - integer
     - Data Length Code — number of valid bytes in ``data`` (0–8).
   * - ``data``
     - array of integers
     - Frame payload bytes.

Each received frame is also logged at ``INFO`` level in the format::

    RX 0000CF1A   [8]  11 22 33 44 55 66 77 88

where the first field is the 8-digit zero-padded uppercase hex CAN ID, the
second is the DLC, and the remainder are the payload bytes.

Errors during ``poll()`` or ``read()`` are logged at ``ERROR`` level and the
loop continues. Incomplete frames (``read()`` returns fewer bytes than
``sizeof(struct can_frame)``) are logged as warnings and skipped.

Sending frames
==============

The ``send`` command accepts:

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Parameter
     - Type
     - Description
   * - ``id``
     - integer
     - CAN ID to transmit.
   * - ``extended``
     - boolean
     - Set to ``true`` to send an extended (29-bit) frame; ``false`` for
       standard (11-bit).
   * - ``dlc``
     - integer
     - Number of data bytes (0–8).
   * - ``data``
     - array of integers
     - Payload bytes. Only the first ``min(dlc, 8)`` elements are sent.

Returns ``true`` if the full frame was written to the socket, ``false``
otherwise. A failed write is logged at ``ERROR`` level. A successful write is
logged at ``INFO`` level in the same format as received frames but prefixed
with ``TX``.

The ``receive`` command provides a non-blocking single-frame poll — it calls
``read()`` directly (the socket is opened in non-blocking mode via
``O_NONBLOCK``) and returns an empty object if no frame is available. For
continuous reception, prefer the ``last_frame`` signal from the background
thread.

Teardown
========

``handle_close`` closes the socket file descriptor, sets ``is_open`` to
``false``, publishes ``is_open(false)``, sets the ``running`` flag to
``false``, and joins the RX thread. After ``handle_close`` returns the socket
is fully released and the thread has exited.

CAN-FD
======

The ``enable_fd`` configuration option is defined but not yet wired to the
socket — the implementation currently uses a standard ``struct can_frame``
(maximum 8 bytes). CAN-FD support (``struct canfd_frame``, up to 64 bytes)
is planned for a future revision.

Configuration reference
=======================

.. list-table::
   :header-rows: 1
   :widths: 28 10 12 50

   * - Parameter
     - Type
     - Default
     - Description
   * - ``interface``
     - string
     - ``can0``
     - SocketCAN network interface name (e.g. ``can0``, ``can1``, ``vcan0``).
       The driver brings this interface down and back up at startup to apply
       the configured bitrate.
   * - ``bitrate``
     - integer
     - ``500000``
     - CAN bus bitrate in bits per second. Passed directly to the ``ip link``
       command. Common values: 125000, 250000, 500000, 1000000.
   * - ``enable_fd``
     - boolean
     - ``false``
     - Reserved for future CAN-FD support. Has no effect in the current
       implementation.
   * - ``recv_timeout_ms``
     - integer
     - ``100``
     - ``poll()`` timeout in milliseconds used by the RX thread. A shorter
       value reduces shutdown latency at the cost of slightly higher CPU
       usage when the bus is idle.
   * - ``tx_queue_size``
     - integer
     - ``100``
     - Reserved for future use. The current implementation sets the kernel TX
       queue length to a fixed value of 1000 via ``ip link set txqueuelen``
       regardless of this setting.

Usage example
=============

Add the CanBus module to your EVerest configuration and connect a protocol
module (e.g. a GB/T 27930 controller) to its ``can_bus`` interface::

    CanBus:
      module: CanBus
      config_module:
        interface: can0
        bitrate: 250000
        recv_timeout_ms: 50

    GBT27930Controller:
      module: GBT27930Controller
      connections:
        can_bus:
          - module_id: CanBus
            implementation_id: can_bus

The protocol module calls ``send`` to transmit frames and subscribes to
``last_frame`` to receive them. It may also call ``open`` and ``close`` to
manage the socket lifecycle if the bus needs to be restarted at runtime.

Hardware notes
==============

* The CAN bus must be physically terminated at both ends with 120 Ω resistors.
* Virtual interfaces (``vcan0`` etc.) can be used for integration testing
  without real hardware: ``sudo modprobe vcan && sudo ip link add dev vcan0
  type vcan && sudo ip link set vcan0 up``.
* Because the driver calls ``ip link`` via ``std::system()``, the EVerest
  process must have sufficient privileges (``CAP_NET_ADMIN``) or be run as
  root to bring the interface up.
* Standard and extended CAN 2.0B frames are supported. CAN-FD is not yet
  supported.
