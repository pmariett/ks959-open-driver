# KS-959 Reverse Engineering Plan

This document describes the current reverse engineering strategy for the
KS-959 USB IrDA adapter (VID 07D0 / PID 4959).

The goal of this project is to document the real protocol used by the device
and implement an open source userspace stack for modern Windows systems
using WinUSB.

The protocol model is reconstructed from:
- behavior observed with this repository
- information extracted from the Linux driver ks959-sir

This document describes the current understanding and the strategy used
to confirm and implement the protocol.

------------------------------------------------------------
## PROJECT GOAL
------------------------------------------------------------

The objective is to implement a clean userspace API:

    ks959_open()
    ks959_set_baud_rate()
    ks959_send_ir_frame()
    ks959_receive_ir_frame()

The project does NOT attempt to guess arbitrary USB commands.
Instead it reconstructs the actual protocol used by the device.

------------------------------------------------------------
## CURRENT UNDERSTANDING
------------------------------------------------------------

The KS-959 exposes the following USB interface structure:

    Interface class : vendor specific
    Endpoint 0x00   : control endpoint
    Endpoint 0x81   : interrupt IN (present but not used for main transport)

Important finding from the Linux driver:

    The real protocol uses control transfers on endpoint 0.

The interrupt endpoint 0x81 is present but appears to be unused for
data transport.

------------------------------------------------------------
### USB CONTROL REQUEST MODEL
------------------------------------------------------------

The Linux driver uses class/interface control requests.

OUT transfers:

    bmRequestType = 0x21

IN transfers:

    bmRequestType = 0xA1

This means the protocol does NOT use arbitrary vendor requests.

Earlier tests using request types 0x40 and 0xC0 were therefore
likely incorrect.

------------------------------------------------------------
### RX DATA PATH
------------------------------------------------------------

Receiving data is implemented by polling the device using a control
transfer.

The request observed in the Linux driver is:

    bmRequestType = 0xA1
    bRequest      = 0x01
    wValue        = 0x0200
    wIndex        = 0x0000
    wLength       = 2048

The returned data is not raw IrDA data.

The host must:

1) de-obfuscate the data
2) pass the result to an async SIR frame unwrap stage

------------------------------------------------------------
### TX DATA PATH
------------------------------------------------------------

Transmitting data also uses control transfers.

Request structure:

    bmRequestType = 0x21
    bRequest      = 0x09
    wValue        = <clear fragment length>
    wIndex        = 0x0000
    wLength       = <padded obfuscated length>

The payload sent to the dongle is NOT the raw IrDA frame.

The process is:

1) wrap frame using async SIR
2) fragment the wrapped data
3) obfuscate and pad each fragment
4) send fragment through EP0 control transfer

------------------------------------------------------------
### SPEED CONFIGURATION
------------------------------------------------------------

Speed configuration uses a dedicated control request.

Request:

    bmRequestType = 0x21
    bRequest      = 0x09
    wValue        = 0x0200
    wIndex        = 0x0001
    wLength       = 8

Payload structure:

    struct speed_params
    {
        uint32 baudrate_le;
        uint8 flags;
        uint8 reserved[3];
    }

Typical configuration used by Linux:

    baudrate = requested speed
    flags    = 0x03
    reserved = 0

------------------------------------------------------------
### FRAGMENT SIZE LIMITS
------------------------------------------------------------

The dongle accepts a maximum USB packet size of 256 bytes.

The Linux driver computes the padded size of TX fragments as:

    padded_len = ((clear_len + 7) & ~7) + 16

Given the USB limit of 256 bytes, the maximum clear fragment size
that still fits is approximately:

    240 bytes

------------------------------------------------------------
### TX OBFUSCATION
------------------------------------------------------------

TX payloads are obfuscated before being sent to the dongle.

Algorithm observed in the Linux driver:

- output buffer is first zeroed
- a lookup string is used:

    "wangshuofei19710"

- XOR mask:

    mask = lookup[(len & 0x0F) ^ 0x06] ^ 0x55

- each byte is XORed with mask
- remaining bytes are padding

------------------------------------------------------------
### RX DEOBFUSCATION
------------------------------------------------------------

RX decoding uses a session-wide XOR variable.

State variable:

    rx_variable_xormask

Algorithm:

For each received byte:

    rx_variable_xormask++
    decoded = encoded ^ rx_variable_xormask ^ 0x55

If the XOR variable wraps to zero, the byte is discarded.

Important property:

    The RX state is NOT reset for each frame.

It is persistent for the whole session.

------------------------------------------------------------
### MINIMAL INITIALIZATION SEQUENCE
------------------------------------------------------------

Based on Linux behavior, the minimal startup sequence is:

1) open device with WinUSB
2) configure baud rate (usually 9600)
3) start EP0 RX polling
4) send TX fragments when needed

No large initialization sequence has been observed.

------------------------------------------------------------
## REVERSE ENGINEERING STRATEGY
------------------------------------------------------------

### Phase 1 (already done)

- open device with WinUSB
- inspect endpoints
- attempt control transfers
- observe failures of arbitrary vendor requests

### Phase 2 (current)

- reconstruct protocol from Linux driver
- document protocol model
- implement protocol layer

Primary outputs:

    docs/ks959-protocol-model.md
    libks959/ks959_proto.h
    libks959/ks959_proto.c

### Phase 3 (validation)

- reproduce Linux requests on Windows
- validate RX polling
- validate speed change
- validate TX fragments

Optional validation using Linux host (Raspberry Pi):

- run the ks959-sir driver
- observe behavior
- confirm protocol assumptions

------------------------------------------------------------
## NEXT IMPLEMENTATION FILES
------------------------------------------------------------

Priority files for the next step:

    docs/ks959-protocol-model.md
    libks959/ks959_proto.h
    libks959/ks959_proto.c

------------------------------------------------------------
## DOCUMENTATION POLICY
------------------------------------------------------------

Every confirmed protocol element should document:

- request type
- request code
- direction
- value
- index
- payload format
- confidence level
- source of evidence

------------------------------------------------------------
## LEGAL NOTE
------------------------------------------------------------

This project documents observable device behavior and a protocol model.

No proprietary driver code is redistributed.