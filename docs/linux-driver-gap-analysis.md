# Linux driver gap analysis

This document records the current gap between:
- the protocol model inferred from the Linux ks959-sir driver
- the behavior observed on a real KS-959 dongle with Linux userspace tools

The goal is to keep a precise record of:
- what is confirmed
- what is contradicted
- what remains unknown

## Scope

Target device:

- VID = 07D0
- PID = 4959
- Product string = USB to IRDA

Observed USB descriptors:

- low-speed USB 1.1 device
- EP0 max packet size = 8
- one interface
- interface class = vendor specific
- one interrupt IN endpoint = 0x81
- endpoint max packet size = 8

## What the Linux driver suggests

From ks959-sir, the current inferred model is:

### RX polling

The receive path appears to use a class/interface control transfer:

- bmRequestType = 0xA1
- bRequest      = 0x01
- wValue        = 0x0200
- wIndex        = 0x0000
- wLength       = large RX buffer

### OUT requests

The driver uses class/interface OUT requests:

- bmRequestType = 0x21
- bRequest      = 0x09

The comments and code path strongly suggest that:
- the device is not driven by arbitrary vendor requests
- endpoint 0x81 is not the main data path

### RX decode model

The driver maintains a session-wide RX de-obfuscation state.

### TX path

The driver appears to:
- wrap SIR frames
- obfuscate and pad
- submit them via EP0

## What real hardware confirms

The following has been confirmed experimentally on a Raspberry Pi with libusb.

### Confirmed: RX poll request family

The following request succeeds:

- bmRequestType = 0xA1
- bRequest      = 0x01
- wValue        = 0x0200
- wIndex        = 0x0000

Observed result:
- request completes successfully
- returned length is 0 while idle

This strongly confirms that the Linux RX polling direction and request code are correct.

### Confirmed: class/interface OUT request family

The following request family is accepted:

- bmRequestType = 0x21
- bRequest      = 0x09
- wIndex        = 0x0000
- wLength       = 8

Observed result:
- request completes successfully
- many payload variants are accepted

### Confirmed: vendor/interface requests are wrong

Vendor/interface attempts fail with PIPE.

This strongly supports the Linux class/interface model and contradicts the original
phase-1 assumption that the dongle should be driven with arbitrary vendor requests.

### Confirmed: endpoint 0x81 appears inactive

Interrupt reads on endpoint 0x81 produced no observable data during tests.

This is consistent with the Linux comment that endpoint 0x81 is not actually used
for the real transport path.

## What real hardware contradicts

### Contradiction: wIndex = 0x0001 for the 8-byte OUT request

The Linux-based hypothesis initially used:

- bmRequestType = 0x21
- bRequest      = 0x09
- wValue        = 0x0200
- wIndex        = 0x0001
- wLength       = 8

Observed on the real dongle:
- wIndex = 0x0001 -> LIBUSB_ERROR_IO
- wIndex = 0x0000 -> OK

Therefore the original "speed request" interpretation is not valid as-is for this
device sample.

## What remains unknown

### Unknown: meaning of the accepted 8-byte OUT payload

The dongle accepts many combinations of:

- wValue = 0x0000 / 0x0100 / 0x0200 / 0x0400 / 0x0800
- payload[0..3] = baud-like values
- payload[4]    = several flag values

This means the dongle validates the request syntax, but the semantic meaning of
that accepted request is still unclear.

### Unknown: RX arming sequence

No tested userspace sequence has yet produced observable received data, even with:

- accepted EP0 OUT requests
- repeated RX polling
- standard IR remote pointed at the dongle
- Aladin Prime pointed at the dongle

So at least one of the following is still missing:

- a true RX arm command
- a longer initialization sequence
- a TX or mode command that switches the dongle into active receive mode
- a detail provided implicitly by the full Linux kernel IrDA stack

### Unknown: exact TX command model

The Linux source suggests that TX also uses EP0, but the exact relationship between:

- bRequest = 0x09
- wValue
- fragment length
- wrapped/obfuscated payload

has not yet been validated on real hardware.

## Practical conclusion

At this stage:

- EP0 class/interface transport is confirmed
- endpoint 0x81 appears irrelevant in practice
- accepted 8-byte OUT requests are real but not yet functionally understood
- userspace reproduction of RX is not complete yet

The project should therefore stop broad empirical scanning and instead focus on:

1. strict function-by-function reconstruction of ks959-sir behavior
2. careful modeling of the TX path
3. documenting all deviations between Linux assumptions and real hardware

## Recommended repository changes

### Documentation

Keep this file updated whenever a new real-hardware result:
- confirms a Linux-driver hypothesis
- contradicts a Linux-driver hypothesis
- eliminates an alternative path

### Library naming

The 8-byte accepted OUT payload should not be named as a confirmed speed structure yet.

Prefer neutral names such as:
- ks959_out8_payload_t
- ks959_build_out8_setup()

until semantics are proven.

## Current confidence summary

High confidence:
- RX polling request family
- class/interface request family
- vendor/interface requests are wrong
- endpoint 0x81 is not useful in current tests

Medium confidence:
- 8-byte OUT request is some kind of mode/config/control command
- Linux TX path direction is broadly correct

Low confidence:
- exact RX arming sequence
- exact meaning of wValue on OUT requests
- exact role of the accepted 8-byte payload

## Additional userspace result

The dongle accepts multiple OUT payload lengths on:

- bmRequestType = 0x21
- bRequest      = 0x09
- wIndex        = 0x0000

Confirmed accepted lengths in current tests:
- 8
- 16
- 32

Arbitrary payload patterns were accepted without immediate USB error.

Implication:
- request 0x09 appears to be a generic write path rather than a narrowly validated fixed-format command
- the missing piece is more likely the higher-level payload format or the functional sequencing
