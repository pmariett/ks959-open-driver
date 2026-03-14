#include <libusb-1.0/libusb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KS959_VID 0x07D0
#define KS959_PID 0x4959

#define KS959_REQTYPE_OUT_CLASS_IFACE 0x21
#define KS959_REQTYPE_IN_CLASS_IFACE  0xA1

#define KS959_REQ_RECV 0x01
#define KS959_REQ_SEND 0x09

#define KS959_RX_WVALUE 0x0200
#define KS959_RX_WINDEX 0x0000
#define KS959_RX_LENGTH 2048

#define KS959_SPEED_WVALUE 0x0200
#define KS959_SPEED_WINDEX 0x0001
#define KS959_SPEED_LENGTH 8

#define KS959_SPEED_FLAGS_8N1 0x03

struct ks959_speed_params {
    uint8_t baud_le[4];
    uint8_t flags;
    uint8_t reserved[3];
};

static void write_le32(uint32_t value, uint8_t out[4])
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void hex_dump(const char *title, const uint8_t *buf, int len, int max_show)
{
    int i;
    int shown = (len < max_show) ? len : max_show;

    printf("%s (%d bytes):\n", title, len);
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

static int ks959_set_speed(libusb_device_handle *handle, uint32_t baud)
{
    struct ks959_speed_params params;
    int ret;

    memset(&params, 0, sizeof(params));
    write_le32(baud, params.baud_le);
    params.flags = KS959_SPEED_FLAGS_8N1;

    ret = libusb_control_transfer(
        handle,
        KS959_REQTYPE_OUT_CLASS_IFACE,
        KS959_REQ_SEND,
        KS959_SPEED_WVALUE,
        KS959_SPEED_WINDEX,
        (unsigned char *)&params,
        sizeof(params),
        1000
    );

    if (ret < 0) {
        fprintf(stderr, "set_speed(%u) failed: %s\n", baud, libusb_error_name(ret));
        return ret;
    }

    printf("set_speed(%u) ok, transferred=%d\n", baud, ret);
    return 0;
}

static int ks959_poll_rx_raw(libusb_device_handle *handle, uint8_t *buf, int buf_size)
{
    int ret;

    ret = libusb_control_transfer(
        handle,
        KS959_REQTYPE_IN_CLASS_IFACE,
        KS959_REQ_RECV,
        KS959_RX_WVALUE,
        KS959_RX_WINDEX,
        buf,
        (uint16_t)buf_size,
        1000
    );

    if (ret < 0) {
        fprintf(stderr, "rx poll failed: %s\n", libusb_error_name(ret));
        return ret;
    }

    return ret;
}

static void ks959_rx_decode_inplace(uint8_t *buf, int len, uint8_t *state, int *decoded_len)
{
    int i;
    int out = 0;

    for (i = 0; i < len; ++i) {
        uint8_t decoded;
        (*state)++;
        decoded = (uint8_t)(buf[i] ^ (*state) ^ 0x55u);

        if (*state == 0u) {
            continue;
        }

        buf[out++] = decoded;
    }

    *decoded_len = out;
}

int main(int argc, char **argv)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;
    uint8_t *rxbuf = NULL;
    uint8_t rx_state = 0;
    uint32_t baud = 9600;
    int polls = 10;
    int ret;
    int i;

    if (argc >= 2) {
        baud = (uint32_t)strtoul(argv[1], NULL, 0);
    }
    if (argc >= 3) {
        polls = atoi(argv[2]);
    }

    ret = libusb_init(&ctx);
    if (ret != 0) {
        fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(ret));
        return 1;
    }

    handle = libusb_open_device_with_vid_pid(ctx, KS959_VID, KS959_PID);
    if (!handle) {
        fprintf(stderr, "KS-959 not found\n");
        libusb_exit(ctx);
        return 1;
    }

    if (libusb_kernel_driver_active(handle, 0) == 1) {
        ret = libusb_detach_kernel_driver(handle, 0);
        if (ret != 0) {
            fprintf(stderr, "detach_kernel_driver failed: %s\n", libusb_error_name(ret));
        }
    }

    ret = libusb_claim_interface(handle, 0);
    if (ret != 0) {
        fprintf(stderr, "claim_interface failed: %s\n", libusb_error_name(ret));
        libusb_close(handle);
        libusb_exit(ctx);
        return 1;
    }

    printf("Opened KS-959 %04X:%04X\n", KS959_VID, KS959_PID);

    ret = ks959_set_speed(handle, baud);
    if (ret != 0) {
        goto cleanup;
    }

    rxbuf = (uint8_t *)malloc(KS959_RX_LENGTH);
    if (!rxbuf) {
        fprintf(stderr, "malloc failed\n");
        ret = -1;
        goto cleanup;
    }

    for (i = 0; i < polls; ++i) {
        int raw_len;
        int dec_len;

        raw_len = ks959_poll_rx_raw(handle, rxbuf, KS959_RX_LENGTH);
        if (raw_len < 0) {
            ret = raw_len;
            goto cleanup;
        }

        printf("\nPoll %d: raw_len=%d\n", i + 1, raw_len);
        if (raw_len > 0) {
            hex_dump("Raw", rxbuf, raw_len, 64);
            ks959_rx_decode_inplace(rxbuf, raw_len, &rx_state, &dec_len);
            printf("Decoded len=%d, rx_state=0x%02X\n", dec_len, rx_state);
            if (dec_len > 0) {
                hex_dump("Decoded", rxbuf, dec_len, 64);
            }
        }
    }

    ret = 0;

cleanup:
    free(rxbuf);
    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(ctx);

    return (ret == 0) ? 0 : 1;
}