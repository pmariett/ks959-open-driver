# REVERSE ENGINEERING

# KS-959 Reverse Engineering Plan

This document describes the strategy used to reverse engineer the USB protocol of the **KS-959 USB IrDA adapter (VID_07D0 / PID_4959)**.

The objective is to document the device protocol and provide a modern open source driver stack for Windows x64 using WinUSB.

---

# Goals

The reverse engineering effort aims to determine:

* device initialization sequence
* vendor-specific control requests
* IR transmission commands
* IR reception data format
* status / event messages

Ultimately this will allow implementation of a userspace library:

```
libks959
```

with functions such as:

```
ks959_open()
ks959_send_frame()
ks959_receive_frame()
ks959_set_speed()
```

---

# Known USB architecture

Based on USB descriptor analysis:

| Element          | Value                   |
| ---------------- | ----------------------- |
| Interface class  | Vendor specific (0xFF)  |
| Endpoints        | 1 interrupt IN endpoint |
| Endpoint address | 0x81                    |
| Packet size      | 8 bytes                 |
| Control endpoint | EP0                     |

This strongly suggests that:

* commands are sent using **vendor control transfers**
* events and received data are returned through **interrupt endpoint 0x81**

---

# Reverse engineering strategy

The reverse engineering process consists of four stages.

---

# Stage 1 — Capture USB traffic

The first step is to observe the behavior of the **original driver**.

Recommended setup:

* Windows XP or Windows 7 (32-bit)
* original KS-959 driver
* USB capture tool

Recommended capture tools:

* USBPcap
* Wireshark
* USBlyzer (optional)

Capture scenarios:

1. device plug-in
2. driver initialization
3. IR transmission
4. IR reception
5. device shutdown

The goal is to observe:

```
Control Transfers
Interrupt IN transfers
```

---

# Stage 2 — Identify command structure

Typical vendor protocols use a simple message format such as:

```
[CMD][LEN][DATA][CHECKSUM]
```

or

```
[CMD][PARAM1][PARAM2]
```

Things to identify:

* command IDs
* payload structure
* response format
* timing requirements

---

# Stage 3 — Map protocol operations

Once commands are identified, map them to logical operations.

Example mapping:

| Operation           | USB command      |
| ------------------- | ---------------- |
| Initialize device   | CMD_INIT         |
| Set IR speed        | CMD_SET_SPEED    |
| Send frame          | CMD_TX           |
| Enable receiver     | CMD_RX_ENABLE    |
| Read received frame | interrupt packet |

---

# Stage 4 — Implement prototype driver

Using WinUSB, implement a minimal prototype:

```
open device
send control request
read interrupt endpoint
```

Example architecture:

```
Application
     │
libks959
     │
WinUSB
     │
KS-959 USB device
```

Prototype tools may include:

```
ks959-monitor
ks959-terminal
ks959-dump
```

---

# Expected protocol characteristics

Given the device descriptors:

* packets likely limited to **8 bytes**
* interrupt endpoint used for events
* commands probably transmitted through **control transfers**

This suggests a relatively simple protocol.

---

# Documentation policy

All discoveries should be documented in:

```
docs/protocol-notes.md
```

Each command should include:

* request type
* request code
* value
* index
* payload
* expected response

---

# Legal considerations

The reverse engineering process focuses on **observing USB traffic and documenting device behavior**.

No proprietary driver code is copied or redistributed.

This approach is compatible with most open source licensing policies.

---

# Future work

Once the protocol is documented:

1. implement full `libks959`
2. create command-line tools
3. provide Python bindings
4. support additional operating systems (Linux, macOS)
