# ks959-probe

`ks959-probe` is a minimal diagnostic tool for the KS-959 USB IrDA adapter
(`VID_07D0`, `PID_4959`) attached to WinUSB.

It performs the following checks:

- enumerates present USB device interfaces for the KS-959 device
- opens the device path exposed by WinUSB
- initializes a WinUSB handle
- queries interface settings
- lists endpoint pipes
- prints whether endpoint `0x81` interrupt IN is present

This tool does **not** send any vendor-specific control transfers yet.
It is only meant to validate the Windows x64 + WinUSB setup.

## Expected device

- Vendor ID: `0x07D0`
- Product ID: `0x4959`

## Build

Open a **Developer Command Prompt for Visual Studio** and run:

```bat
cl /W4 /EHsc /DUNICODE /D_UNICODE tools\ks959-probe.c setupapi.lib winusb.lib
```

This should produce:

```
ks959-probe.exe
```
## Run

Plug the KS-959 adapter, make sure the WinUSB INF is installed, then run:

```
ks959-probe.exe
```

## Expected output

Typical output should look similar to:

```
KS-959 probe starting...
Searching for device interface...
Device path: \\?\usb#vid_07d0&pid_4959#...
WinUSB initialized successfully.
Interface 0 has 1 pipe(s).
Pipe 0:
  PipeType        : Interrupt
  PipeId          : 0x81
  MaximumPacketSize: 8
  Interval        : 1
Found expected interrupt IN endpoint 0x81.
Probe completed successfully.
```

## Notes

Known USB descriptor characteristics for this device:
- one vendor-specific interface  
- one interrupt IN endpoint  
- endpoint address 0x81  
- max packet size 8  

This matches the current reverse engineering notes.

## Next steps

Future versions should add:

- vendor control transfer test  
- interrupt read test  
- endpoint polling  
- protocol logging  