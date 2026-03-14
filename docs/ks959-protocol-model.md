# KS-959 protocol model

## Scope

This document captures the current protocol model for the KingSun / Extended Systems
KS-959 USB IrDA adapter (VID 07D0, PID 4959), reconstructed from the Linux
`ks959-sir` driver behavior.

This is not an official hardware specification.
It is a working model derived from:
- the Linux driver implementation
- the comments embedded in that driver
- current observations already documented in this repository

## High-confidence findings

### 1. Endpoint 0x81 is not the real data path

Although the device exposes one interrupt IN endpoint (0x81), the Linux driver states
that this endpoint is not actually used for transport.

All meaningful traffic is performed through control transfers on endpoint 0.

Implication for this repository:
- do not design the main protocol around endpoint 0x81
- treat 0x81 as non-essential unless future captures prove otherwise

### 2. The KS-959 does not use arbitrary vendor requests

The Linux driver uses class/interface control requests:
- OUT: bmRequestType = 0x21
- IN : bmRequestType = 0xA1

So the current Windows tests based on arbitrary vendor-specific requests are not a valid
model of the real protocol.

### 3. Receive path is polled through EP0

The receive polling request is:

- bmRequestType = 0xA1
- bRequest      = 0x01
- wValue        = 0x0200
- wIndex        = 0x0000
- wLength       = 0x0800

The device returns obfuscated data that must be de-obfuscated and then unwrapped as a
SIR async frame.

### 4. Transmit path is also EP0-based

To send a fragment:

- bmRequestType = 0x21
- bRequest      = 0x09
- wValue        = <clear wrapped fragment length>
- wIndex        = 0x0000
- wLength       = <obfuscated padded payload length>

The payload is not the raw IR frame.
The driver:
1. wraps the frame as async SIR
2. fragments it
3. obfuscates and pads each fragment
4. sends it through EP0

### 5. Baud rate change is a dedicated control transfer

The speed-change request is:

- bmRequestType = 0x21
- bRequest      = 0x09
- wValue        = 0x0200
- wIndex        = 0x0001
- wLength       = 0x0008

Payload layout (8 bytes):

struct ks959_speedparams {
    uint32_t baudrate_le;
    uint8_t flags;
    uint8_t reserved[3];
};

Observed Linux configuration:
- baudrate = requested speed, little-endian
- flags = 0x03 (8 data bits)
- reserved = 0

## TX protocol model

### Step 1: SIR wrapping

Linux uses the normal IrDA async wrapper before sending data to the dongle.

For the Windows port, the KS-959 transport layer should operate on already wrapped bytes,
or should contain an internal helper that performs equivalent wrapping.

### Step 2: Fragmentation

The dongle accepts at most 256 bytes per USB send transaction.

The Linux driver computes the obfuscated length as:

padded_len = ((clear_len + 7) & ~0x7) + 0x10

Therefore the maximum clear fragment size that still fits in 256 bytes is 240 bytes.

### Step 3: TX obfuscation

The Linux algorithm is:

- output buffer is zero-filled
- let lookup = "wangshuofei19710"
- xor_mask = lookup[(clear_len & 0x0f) ^ 0x06] ^ 0x55
- each clear byte is XORed with xor_mask
- remaining bytes are zero padding

This is fragment-local.
The mask depends on the fragment length.

## RX protocol model

### Poll request

The host repeatedly submits the fixed receive request:

- bmRequestType = 0xA1
- bRequest      = 0x01
- wValue        = 0x0200
- wIndex        = 0x0000
- wLength       = 2048

### RX de-obfuscation

The Linux driver maintains a session-wide byte counter-like XOR state:

rx_variable_xormask starts at 0

For each received byte:
1. rx_variable_xormask++
2. decoded = encoded ^ rx_variable_xormask ^ 0x55

If rx_variable_xormask becomes 0 after increment (8-bit wraparound), the decoded byte is
discarded instead of being fed to the async SIR unwrap logic.

Implications:
- RX decode state is session-wide, not frame-local
- every 256th byte slot is special and skipped
- reset behavior of this state should be tied to device/session open, not to each frame

### Step 3: SIR unwrapping

Decoded bytes are then passed into an async SIR unwrapping state machine until a full
frame is reconstructed.

## Minimal Windows bring-up sequence

Based on Linux behavior, the minimal realistic initialization sequence is:

1. open device through WinUSB
2. use EP0 only
3. send speed request for 9600 bps
4. start RX polling loop on EP0
5. send wrapped/obfuscated TX fragments on demand

## What this changes in the current repository

### Existing assumptions to revise

The following assumptions in current docs should be revised:

- "commands are sent using vendor control transfers"
- "events and received data are returned through interrupt endpoint 0x81"

### New implementation direction

The next implementation should add:
- class/interface control request constants
- speed request packing
- TX fragment obfuscation
- RX de-obfuscation state
- EP0 polling receive loop

## Confidence level

High confidence:
- request types and request codes
- speed request format
- TX obfuscation algorithm
- RX de-obfuscation algorithm
- EP0-only transport model

Medium confidence:
- exact correspondence between Linux IrDA wrapper behavior and the exact Windows-facing
  API boundaries we should expose in libks959

Open questions:
- whether the original Windows XP driver uses exactly the same sequencing/timing
- whether any device variants depend on endpoint 0x81 for status only
- whether speeds above 57600 are usable in practice on modern hosts
