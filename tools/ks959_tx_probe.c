#include <libusb-1.0/libusb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KS959_VID 0x07D0
#define KS959_PID 0x4959

#define IFACE_NUM  0
#define TIMEOUT_MS 1000

#define REQTYPE_OUT_CLASS_IFACE 0x21
#define REQTYPE_IN_CLASS_IFACE  0xA1

#define KS959_REQ_RX   0x01
#define KS959_REQ_OUT  0x09

#define KS959_RX_WVALUE 0x0200
#define KS959_RX_WINDEX 0x0000

static void hex_dump(const char *title, const uint8_t *buf, int len, int max_show)
{
    int i;
    int shown = (len < max_show) ? len : max_show;

    printf("%s (%d bytes)\n", title, len);
    for (i = 0; i < shown; ++i) {
        if ((i % 16) == 0) {
            printf("  %04x :", i);
        }
        printf(" %02X", buf[i]);
        if ((i % 16) == 15 || i == shown - 1) {
            printf("\n");
        }
    }
    if (shown < len) {
        printf("  ... truncated ...\n");
    }
}

static int open_ks959(libusb_context **out_ctx, libusb_device_handle **out_handle)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;
    int ret;

    ret = libusb_init(&ctx);
    if (ret != 0) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(ret));
        return ret;
    }

    handle = libusb_open_device_with_vid_pid(ctx, KS959_VID, KS959_PID);
    if (!handle) {
        fprintf(stderr, "KS-959 not found\n");
        libusb_exit(ctx);
        return LIBUSB_ERROR_NO_DEVICE;
    }

    if (libusb_kernel_driver_active(handle, IFACE_NUM) == 1) {
        ret = libusb_detach_kernel_driver(handle, IFACE_NUM);
        if (ret != 0) {
            fprintf(stderr, "detach_kernel_driver failed: %s\n", libusb_error_name(ret));
        }
    }

    ret = libusb_claim_interface(handle, IFACE_NUM);
    if (ret != 0) {
        fprintf(stderr, "claim_interface failed: %s\n", libusb_error_name(ret));
        libusb_close(handle);
        libusb_exit(ctx);
        return ret;
    }

    *out_ctx = ctx;
    *out_handle = handle;
    return 0;
}

static void close_ks959(libusb_context *ctx, libusb_device_handle *handle)
{
    if (handle) {
        libusb_release_interface(handle, IFACE_NUM);
        libusb_close(handle);
    }
    if (ctx) {
        libusb_exit(ctx);
    }
}

static int rx_poll_once(libusb_device_handle *handle, int len)
{
    uint8_t buf[256];
    int ret;

    memset(buf, 0, sizeof(buf));

    if (len < 0 || len > (int)sizeof(buf)) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    ret = libusb_control_transfer(handle,
                                  REQTYPE_IN_CLASS_IFACE,
                                  KS959_REQ_RX,
                                  KS959_RX_WVALUE,
                                  KS959_RX_WINDEX,
                                  buf,
                                  (uint16_t)len,
                                  TIMEOUT_MS);

    if (ret < 0) {
        printf("RX poll: ERROR %s (%d)\n", libusb_error_name(ret), ret);
        return ret;
    }

    printf("RX poll: OK transferred=%d\n", ret);
    if (ret > 0) {
        hex_dump("RX data", buf, ret, 128);
    }

    return ret;
}

static int tx_probe_once(libusb_device_handle *handle,
                         uint16_t wValue,
                         const uint8_t *payload,
                         uint16_t payload_len,
                         const char *label)
{
    uint8_t tmp[256];
    int ret;

    if (payload_len > sizeof(tmp)) {
        fprintf(stderr, "payload too large\n");
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    memcpy(tmp, payload, payload_len);

    printf("\n=== %s ===\n", label);
    printf("OUT bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x0000 wLength=%u\n",
           REQTYPE_OUT_CLASS_IFACE, KS959_REQ_OUT, wValue, payload_len);

    if (payload_len > 0) {
        hex_dump("OUT payload", tmp, payload_len, 128);
    }

    ret = libusb_control_transfer(handle,
                                  REQTYPE_OUT_CLASS_IFACE,
                                  KS959_REQ_OUT,
                                  wValue,
                                  0x0000,
                                  tmp,
                                  payload_len,
                                  TIMEOUT_MS);

    if (ret < 0) {
        printf("Result: ERROR %s (%d)\n", libusb_error_name(ret), ret);
        return ret;
    }

    printf("Result: OK transferred=%d\n", ret);
    return ret;
}

int main(void)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;
    int ret;

    static const uint8_t p8_a[8] = {
        0x80, 0x25, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00
    };

    static const uint8_t p8_b[8] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    static const uint8_t p16_a[16] = {
        0x55, 0xAA, 0x55, 0xAA, 0x10, 0x20, 0x30, 0x40,
        0x50, 0x60, 0x70, 0x80, 0x81, 0x82, 0x83, 0x84
    };

    static const uint8_t p16_b[16] = {
        0xC0, 0x01, 0x7D, 0x7E, 0x00, 0xFF, 0x11, 0x22,
        0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA
    };

    static const uint8_t p32_a[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F
    };

    ret = open_ks959(&ctx, &handle);
    if (ret != 0) {
        return 1;
    }

    printf("Opened KS-959 %04X:%04X\n", KS959_VID, KS959_PID);

    (void)rx_poll_once(handle, 64);

    (void)tx_probe_once(handle, 0x0000, p8_a, sizeof(p8_a), "OUT8 known-accepted payload @ wValue=0x0000");
    (void)rx_poll_once(handle, 64);

    (void)tx_probe_once(handle, 0x0200, p8_a, sizeof(p8_a), "OUT8 known-accepted payload @ wValue=0x0200");
    (void)rx_poll_once(handle, 64);

    (void)tx_probe_once(handle, 0x0200, p8_b, sizeof(p8_b), "OUT8 all-zero payload @ wValue=0x0200");
    (void)rx_poll_once(handle, 64);

    (void)tx_probe_once(handle, 0x0010, p16_a, sizeof(p16_a), "OUT16 pattern A @ wValue=0x0010");
    (void)rx_poll_once(handle, 64);

    (void)tx_probe_once(handle, 0x0010, p16_b, sizeof(p16_b), "OUT16 pattern B @ wValue=0x0010");
    (void)rx_poll_once(handle, 64);

    (void)tx_probe_once(handle, 0x0020, p32_a, sizeof(p32_a), "OUT32 pattern @ wValue=0x0020");
    (void)rx_poll_once(handle, 64);

    close_ks959(ctx, handle);
    return 0;
}