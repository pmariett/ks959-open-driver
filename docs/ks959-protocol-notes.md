# KS-959 protocol notes

## Confirmed on Windows x64

- WinUSB can open the device successfully.
- Raw EP0 control transfers can be issued.
- The device rejects naive control requests with OS error 31.
- The interrupt endpoint 0x81 stays silent during naive probing.

## Conclusion

The KS-959 protocol is not reducible to simple naked setup packets.
A higher-level framing/encoding layer is required.
