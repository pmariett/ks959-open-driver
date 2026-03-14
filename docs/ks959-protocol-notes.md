# Protocol notes

## Initial WinUSB control transfer tests

### Observed behavior

Arbitrary vendor-specific control transfers were attempted using WinUSB.

Example:
- `RequestType = 0x40`
- `Request     = 0x10`
- `Value       = 0x0000`
- `Index       = 0x0000`
- `Payload     = 01 02 03 04`

Observed result:
- `WinUsb_ControlTransfer` failed
- Windows error: `31` (`ERROR_GEN_FAILURE`)

### Initial interpretation

At the time of the first tests, this suggested that:
- the device rejects unknown requests
- the dongle probably requires a specific initialization / command sequence
- blind fuzzing is unlikely to be productive

## Phase 2 update — Linux driver alignment

The Linux `ks959-sir` driver strongly changes the interpretation of the Windows
failures above.

### Important correction

The KS-959 protocol is not modeled as arbitrary vendor requests in Linux.

Instead, the Linux driver uses:
- `0x21` for OUT control requests
- `0xA1` for IN control requests

This means the initial Windows tests were very likely using the wrong USB
request type model.

### Consequence

Windows error 31 on arbitrary `0x40` / `0xC0` probes is consistent with the
current Linux-based hypothesis:
- the device expects a very specific EP0 protocol
- request direction, type and payload shape matter
- reproducing Linux requests is the correct next step

## Current working model

### RX polling request

Current high-confidence receive request model:

- `bmRequestType = 0xA1`
- `bRequest      = 0x01`
- `wValue        = 0x0200`
- `wIndex        = 0x0000`
- `wLength       = 2048`

Returned data must be de-obfuscated before SIR unwrapping.

### TX fragment request

Current high-confidence transmit request model:

- `bmRequestType = 0x21`
- `bRequest      = 0x09`
- `wValue        = <clear fragment length>`
- `wIndex        = 0x0000`
- `wLength       = <padded obfuscated fragment length>`

The dongle expects:
- async-wrapped data
- fragment size limited by a 256-byte USB packet ceiling
- obfuscation + padding before submission

### Speed request

Current high-confidence speed-change request model:

- `bmRequestType = 0x21`
- `bRequest      = 0x09`
- `wValue        = 0x0200`
- `wIndex        = 0x0001`
- `wLength       = 8`

Payload:
- 32-bit little-endian baud rate
- 1-byte flags field
- 3 reserved bytes

## Practical implication for this repository

The next WinUSB experiments should stop using arbitrary vendor fuzzing and
should instead validate:

1. speed request at 9600 bps
2. EP0 receive polling request
3. TX fragment request with Linux-compatible wrapping/obfuscation

## Confidence

High confidence:
- EP0 is the main transport path
- request codes and request types above
- endpoint `0x81` is not the main data path

Medium confidence:
- exact Windows timing / retry behavior
- exact separation between protocol layer and public API boundaries


## Raspberry Pi validation summary

Confirmed on real hardware:

- IN class/interface request `0xA1 / 0x01 / 0x0200 / 0x0000` succeeds and returns zero-length data when idle
- OUT class/interface request `0x21 / 0x09` with 8-byte payload succeeds when `wIndex = 0x0000`
- The same request fails with `wIndex = 0x0001`
- Vendor/interface requests fail with `PIPE`
- Multiple `wValue` values are accepted
- Multiple baud/flags payload combinations are accepted

Implication:

- EP0 class/interface transport is confirmed
- previous `wIndex = 0x0001` hypothesis is rejected for this device
- the exact meaning of the 8-byte OUT payload remains unknown

## Linux libusb monitor results

Observed on Raspberry Pi:

- RX poll request `0xA1 / 0x01 / 0x0200 / 0x0000` succeeds reliably
- accepted OUT requests `0x21 / 0x09 / wIndex=0x0000 / len=8` do not yet produce observable RX activity
- the same monitor remains idle with or without the current pre-init sequence

Implication:

- EP0 transport is confirmed
- current accepted 8-byte OUT payload is not sufficient to activate visible RX behavior
- next step is to test with active IR stimulus and broaden the OUT command search

## Raspberry Pi validation update

Observed on real hardware:
- EP0 RX poll `0xA1 / 0x01 / 0x0200 / 0x0000` succeeds but remains idle
- EP0 OUT `0x21 / 0x09 / wIndex=0x0000 / len=8` is accepted
- No RX activity was observed with either a standard IR remote or an Aladin Prime
- Linux comment-based model is therefore only partially confirmed

Implication:
- USB transport assumptions are stronger than before
- functional RX arming sequence is still unknown
- endpoint 0x81 should now be tested experimentally despite Linux comments
