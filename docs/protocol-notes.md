# Initial WinUSB control transfer tests

## Observed behavior

Arbitrary vendor-specific control transfers were attempted using WinUSB.

Example:

- RequestType: `0x40`
- Request: `0x10`
- Value: `0x0000`
- Index: `0x0000`
- Payload: `01 02 03 04`

Result:

- `WinUsb_ControlTransfer` failed
- Windows error: `31` (`ERROR_GEN_FAILURE`)

## Interpretation

The device appears to reject unknown or invalid vendor-specific control requests.
This suggests that the KS-959 requires a specific initialization and command sequence.
Blind fuzzing is unlikely to be productive.