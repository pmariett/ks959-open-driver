#include <libusb-1.0/libusb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

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

#define KS959_OUT_WVALUE_DEFAULT 0x0200
#define KS959_OUT_WINDEX         0x0000

#define RX_BUF_SIZE 255

typedef struct ks959_out8_payload {
    uint8_t param0_le[4];
    uint8_t param1;
    uint8_t reserved[3];
} ks959_out8_payload_t;

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

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

static void write_le32(uint32_t value, uint8_t out[4])
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
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

static int ks959_send_out8(libusb_device_handle *handle,
                           uint16_t wValue,
                           const ks959_out8_payload_t *payload,
                           const char *label)
{
    uint8_t tmp[8];
    int ret;

    memcpy(tmp, payload, sizeof(tmp));

    printf("\n=== %s ===\n", label);
    printf("OUT bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x%04X wLength=8\n",
           REQTYPE_OUT_CLASS_IFACE, KS959_REQ_OUT, wValue, KS959_OUT_WINDEX);
    hex_dump("OUT payload", tmp, 8, 64);

    ret = libusb_control_transfer(handle,
                                  REQTYPE_OUT_CLASS_IFACE,
                                  KS959_REQ_OUT,
                                  wValue,
                                  KS959_OUT_WINDEX,
                                  tmp,
                                  8,
                                  TIMEOUT_MS);

    if (ret < 0) {
        printf("Result: ERROR %s (%d)\n", libusb_error_name(ret), ret);
        return ret;
    }

    printf("Result: OK, transferred=%d\n", ret);
    return ret;
}

static int ks959_rx_poll(libusb_device_handle *handle, uint8_t *buf, int buf_len)
{
    return libusb_control_transfer(handle,
                                   REQTYPE_IN_CLASS_IFACE,
                                   KS959_REQ_RX,
                                   KS959_RX_WVALUE,
                                   KS959_RX_WINDEX,
                                   buf,
                                   (uint16_t)buf_len,
                                   TIMEOUT_MS);
}

static int ks959_rx_decode_chunk(uint8_t *state,
                                 const uint8_t *in_buf,
                                 int in_len,
                                 uint8_t *out_buf,
                                 int out_buf_size)
{
    int i;
    int produced = 0;

    for (i = 0; i < in_len; ++i) {
        uint8_t decoded;

        (*state)++;
        decoded = (uint8_t)(in_buf[i] ^ (*state) ^ 0x55u);

        if (*state == 0u) {
            continue;
        }

        if (produced >= out_buf_size) {
            break;
        }

        out_buf[produced++] = decoded;
    }

    return produced;
}

static void fill_out8_payload(ks959_out8_payload_t *p, uint32_t param0, uint8_t param1)
{
    memset(p, 0, sizeof(*p));
    write_le32(param0, p->param0_le);
    p->param1 = param1;
}

static void send_default_preinit_sequence(libusb_device_handle *handle)
{
    ks959_out8_payload_t p;

    /*
     * Sequence volontairement conservative:
     * - forme OUT confirmée comme acceptée
     * - plusieurs variantes plausibles déjà vues comme "OK"
     */
    fill_out8_payload(&p, 9600u, 0x03u);
    (void)ks959_send_out8(handle, 0x0200u, &p, "preinit #1 param0=9600 param1=0x03 wValue=0x0200");

    fill_out8_payload(&p, 9600u, 0x03u);
    (void)ks959_send_out8(handle, 0x0000u, &p, "preinit #2 param0=9600 param1=0x03 wValue=0x0000");
}

int main(int argc, char **argv)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;
    uint8_t raw_buf[RX_BUF_SIZE];
    uint8_t dec_buf[RX_BUF_SIZE];
    uint8_t rx_state = 0;
    int ret;
    int poll_count = 0;
    int show_idle = 0;
    int do_preinit = 1;
    int sleep_ms = 100;
    struct timespec ts;

    if (argc >= 2) {
        sleep_ms = atoi(argv[1]);
        if (sleep_ms < 0) {
            sleep_ms = 100;
        }
    }
    if (argc >= 3) {
        show_idle = atoi(argv[2]) ? 1 : 0;
    }
    if (argc >= 4) {
        do_preinit = atoi(argv[3]) ? 1 : 0;
    }

    signal(SIGINT, on_sigint);

    ret = open_ks959(&ctx, &handle);
    if (ret != 0) {
        return 1;
    }

    printf("Opened KS-959 %04X:%04X\n", KS959_VID, KS959_PID);
    printf("Monitor settings: sleep_ms=%d show_idle=%d do_preinit=%d\n",
           sleep_ms, show_idle, do_preinit);

    if (do_preinit) {
        send_default_preinit_sequence(handle);
    }

    ts.tv_sec = sleep_ms / 1000;
    ts.tv_nsec = (long)(sleep_ms % 1000) * 1000000L;

    while (!g_stop) {
        int dec_len;

        memset(raw_buf, 0, sizeof(raw_buf));
        ret = ks959_rx_poll(handle, raw_buf, sizeof(raw_buf));
        poll_count++;

        if (ret < 0) {
            printf("[poll %d] RX ERROR: %s (%d)\n",
                   poll_count, libusb_error_name(ret), ret);
            nanosleep(&ts, NULL);
            continue;
        }

        if (ret == 0) {
            if (show_idle) {
                printf("[poll %d] idle\n", poll_count);
            }
            nanosleep(&ts, NULL);
            continue;
        }

        printf("\n[poll %d] raw_len=%d rx_state_before=0x%02X\n",
               poll_count, ret, rx_state);
        hex_dump("Raw", raw_buf, ret, 128);

        dec_len = ks959_rx_decode_chunk(&rx_state, raw_buf, ret, dec_buf, sizeof(dec_buf));

        printf("[poll %d] dec_len=%d rx_state_after=0x%02X\n",
               poll_count, dec_len, rx_state);

        if (dec_len > 0) {
            hex_dump("Decoded", dec_buf, dec_len, 128);
        }

        nanosleep(&ts, NULL);
    }

    printf("\nStopping monitor.\n");
    close_ks959(ctx, handle);
    return 0;
}