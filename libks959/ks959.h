#ifndef KS959_H
#define KS959_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Public version
 */
#define KS959_VERSION_MAJOR 0
#define KS959_VERSION_MINOR 1
#define KS959_VERSION_PATCH 0

/*
 * Device identifiers
 */
#define KS959_USB_VID 0x07D0u
#define KS959_USB_PID 0x4959u

/*
 * Supported SIR rates according to Linux ks959-sir support notes.
 */
typedef enum ks959_baud_rate {
    KS959_BAUD_9600   = 9600,
    KS959_BAUD_19200  = 19200,
    KS959_BAUD_38400  = 38400,
    KS959_BAUD_57600  = 57600
} ks959_baud_rate_t;

/*
 * Error codes
 */
typedef enum ks959_status {
    KS959_OK = 0,
    KS959_ERR_INVALID_ARG = -1,
    KS959_ERR_NO_DEVICE = -2,
    KS959_ERR_OPEN_FAILED = -3,
    KS959_ERR_WINUSB_INIT_FAILED = -4,
    KS959_ERR_IO = -5,
    KS959_ERR_TIMEOUT = -6,
    KS959_ERR_NOT_SUPPORTED = -7,
    KS959_ERR_BUFFER_TOO_SMALL = -8,
    KS959_ERR_PROTOCOL = -9,
    KS959_ERR_INTERNAL = -10
} ks959_status_t;

/*
 * Opaque device handle
 */
typedef struct ks959_device ks959_device_t;

/*
 * Setup packet model for raw control transfers.
 * Mirrors the fields needed by WinUSB / USB setup transactions.
 */
typedef struct ks959_setup_packet {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} ks959_setup_packet_t;

/*
 * Library lifecycle
 */
int ks959_open(ks959_device_t **out_dev);
void ks959_close(ks959_device_t *dev);

/*
 * Optional helpers
 */
int ks959_get_version(unsigned *major, unsigned *minor, unsigned *patch);
int ks959_get_vid_pid(ks959_device_t *dev, uint16_t *vid, uint16_t *pid);

/*
 * Raw control I/O
 *
 * These functions are intended to be the lowest public layer over the device.
 * The implementation is expected to use WinUSB control transfers on EP0.
 */
int ks959_ctrl_write(ks959_device_t *dev,
                     const ks959_setup_packet_t *setup,
                     const uint8_t *buffer,
                     size_t buffer_len,
                     size_t *bytes_transferred);

int ks959_ctrl_read(ks959_device_t *dev,
                    const ks959_setup_packet_t *setup,
                    uint8_t *buffer,
                    size_t buffer_len,
                    size_t *bytes_transferred);

/*
 * Protocol codec
 *
 * These functions will eventually implement the KS-959-specific wrapping,
 * padding and obfuscation rules.
 *
 * Current project state:
 * - function contracts are defined
 * - implementation is pending extraction from ks959-sir behavior
 */
int ks959_encode_tx_payload(const uint8_t *ir_frame,
                            size_t ir_frame_len,
                            uint8_t *usb_payload,
                            size_t *usb_payload_len);

int ks959_decode_rx_payload(const uint8_t *usb_payload,
                            size_t usb_payload_len,
                            uint8_t *ir_frame,
                            size_t *ir_frame_len);

/*
 * Higher-level transport helpers
 *
 * These are intentionally generic for now. Once the Linux protocol logic
 * is mapped precisely, these will wrap one or more raw control transfers.
 */
int ks959_send_ir_frame(ks959_device_t *dev,
                        const uint8_t *ir_frame,
                        size_t ir_frame_len);

int ks959_receive_ir_frame(ks959_device_t *dev,
                           uint8_t *ir_frame,
                           size_t *ir_frame_len);

/*
 * Device configuration
 *
 * The KS-959 Linux support documents SIR-only operation in the range
 * 9600..57600 bps.
 */
int ks959_set_baud_rate(ks959_device_t *dev, ks959_baud_rate_t baud);

/*
 * Diagnostics
 */
int ks959_last_os_error(ks959_device_t *dev, uint32_t *out_error_code);

#ifdef __cplusplus
}
#endif

#endif /* KS959_H */