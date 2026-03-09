# KS-959 USB IrDA Adapter — USB Architecture

## Device identification

The KS-959 USB IrDA adapter exposes the following USB identifiers:

| Field               | Value         |
| ------------------- | ------------- |
| Vendor ID           | 0x07D0        |
| Product ID          | 0x4959        |
| Manufacturer string | "Kingsun CO." |
| Product string      | "USB to IRDA" |

USB version reported by the device:

```
bcdUSB = 0x0110
```

This indicates a **USB 1.1 full-speed device**.

---

# Device descriptor

```
bLength            0x12
bDescriptorType    0x01
bcdUSB             0x0110
bDeviceClass       0x00 (defined at interface level)
bDeviceSubClass    0x00
bDeviceProtocol    0x00
bMaxPacketSize0    8
idVendor           0x07D0
idProduct          0x4959
bcdDevice          0x0100
iManufacturer      1  ("Kingsun CO.")
iProduct           2  ("USB to IRDA")
iSerialNumber      0
bNumConfigurations 1
```

Key observations:

* Device class is **0x00**, meaning the class is defined by the interface.
* Endpoint 0 (control endpoint) uses **8-byte packets**, typical of older USB devices.

---

# Configuration descriptor

```
bNumInterfaces     1
bmAttributes       0x80 (bus powered)
MaxPower           100 mA
```

The device exposes a **single configuration** with one interface.

---

# Interface descriptor

```
bInterfaceNumber     0
bAlternateSetting    0
bNumEndpoints        1
bInterfaceClass      0xFF (vendor specific)
bInterfaceSubClass   0
bInterfaceProtocol   0
```

Observations:

* The interface uses **vendor-specific class (0xFF)**.
* No standard USB class (CDC, HID, etc.) is implemented.

This confirms the device requires a **custom protocol driver**.

---

# Endpoint descriptors

## Endpoint 0 (implicit)

All USB devices implement **endpoint 0**, used for control transfers.

```
Type: Control
Max packet size: 8 bytes
```

This endpoint is likely used for:

* device initialization
* configuration commands
* sending IR frames

---

## Endpoint 1

```
bEndpointAddress  0x81
Direction         IN
Transfer type     Interrupt
MaxPacketSize     8
Interval          1
```

Characteristics:

* **Interrupt IN endpoint**
* 8-byte packets
* polled very frequently by the host

---

# Probable communication model

Based on the descriptors, the device likely uses the following architecture:

```
Host (PC)
   │
   │ Control Transfers (EP0)
   │  - initialization
   │  - configuration
   │  - send IR data
   ▼
KS-959 USB IRDA adapter
   ▲
   │ Interrupt IN (EP81)
   │  - status messages
   │  - received IR data
   │  - event notifications
   │
Host (PC)
```

This suggests a **command-response protocol** where:

* commands are sent using **vendor control transfers**
* device events and data are returned through **interrupt endpoint 0x81**

---

# Reverse engineering implications

The protocol surface appears relatively small:

* 1 interface
* 1 interrupt endpoint
* control endpoint for commands

Reverse engineering should focus on:

1. **Vendor control requests** used during device initialization
2. **Interrupt packets** received from endpoint 0x81
3. Control requests used when transmitting IR data

Capturing USB traffic from the original Windows driver should reveal the protocol.

---

# Next steps

Planned reverse engineering steps:

1. Capture USB traffic using USBPcap on a system with the original driver
2. Identify initialization sequence
3. Identify command set
4. Implement equivalent functionality using WinUSB
5. Create open source library `libks959`
