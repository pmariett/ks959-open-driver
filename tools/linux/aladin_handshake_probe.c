#include <libusb-1.0/libusb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define RX_BUF_SIZE 255
#define TX_BUF_SIZE 256

typedef struct ks959_rx_state {
    uint8_t xormask;
} ks959_rx_state_t;

typedef struct ks959_out8_payload {
    uint8_t baud_le[4];
    uint8_t flags;
    uint8_t reserved[3];
} ks959_out8_payload_t;

typedef struct test_frame {
    const char *name;
    const uint8_t *data;
    size_t len;
} test_frame_t;

static void msleep(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void write_le32(uint32_t v, uint8_t out[4])
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
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

static size_t ks959_tx_padded_length(size_t clear_len)
{
    return ((clear_len + 7u) & ~(size_t)7u) + 16u;
}

static int ks959_tx_obfuscate(const uint8_t *clear_buf,
                              size_t clear_len,
                              uint8_t *out_buf,
                              size_t out_buf_size,
                              size_t *out_len)
{
    static const uint8_t lookup[] = "wangshuofei19710";
    uint8_t mask;
    size_t padded_len;
    size_t i;

    if (!clear_buf || !out_buf || !out_len || clear_len == 0) {
        return -1;
    }

    padded_len = ks959_tx_padded_length(clear_len);
    if (padded_len > out_buf_size) {
        return -1;
    }

    memset(out_buf, 0, padded_len);
    mask = (uint8_t)(lookup[(clear_len & 0x0Fu) ^ 0x06u] ^ 0x55u);

    for (i = 0; i < clear_len; ++i) {
        out_buf[i] = (uint8_t)(clear_buf[i] ^ mask);
    }

    *out_len = padded_len;
    return 0;
}

static int ks959_send_out8_9600(libusb_device_handle *handle, uint16_t wValue, const char *label)
{
    ks959_out8_payload_t p;
    int ret;

    memset(&p, 0, sizeof(p));
    write_le32(9600u, p.baud_le);
    p.flags = 0x03;

    printf("\n=== %s ===\n", label);
    printf("OUT8 9600 request: wValue=0x%04X wIndex=0x0000\n", wValue);
    hex_dump("OUT8 payload", (const uint8_t *)&p, sizeof(p), 64);

    ret = libusb_control_transfer(handle,
                                  REQTYPE_OUT_CLASS_IFACE,
                                  KS959_REQ_OUT,
                                  wValue,
                                  0x0000,
                                  (uint8_t *)&p,
                                  (uint16_t)sizeof(p),
                                  TIMEOUT_MS);

    if (ret < 0) {
        printf("OUT8 result: ERROR %s (%d)\n", libusb_error_name(ret), ret);
    } else {
        printf("OUT8 result: OK transferred=%d\n", ret);
    }

    return ret;
}

static int ks959_send_fragment(libusb_device_handle *handle,
                               const uint8_t *clear_buf,
                               size_t clear_len,
                               const char *label)
{
    uint8_t tx_buf[TX_BUF_SIZE];
    size_t tx_len = 0;
    int ret;

    if (ks959_tx_obfuscate(clear_buf, clear_len, tx_buf, sizeof(tx_buf), &tx_len) != 0) {
        fprintf(stderr, "obfuscation failed for %s\n", label);
        return -1;
    }

    printf("\n=== TX %s ===\n", label);
    printf("clear_len=%zu padded_len=%zu wValue=0x%04X\n",
           clear_len, tx_len, (unsigned)clear_len);
    hex_dump("Clear", clear_buf, (int)clear_len, 128);

    ret = libusb_control_transfer(handle,
                                  REQTYPE_OUT_CLASS_IFACE,
                                  KS959_REQ_OUT,
                                  (uint16_t)clear_len,
                                  0x0000,
                                  tx_buf,
                                  (uint16_t)tx_len,
                                  TIMEOUT_MS);

    if (ret < 0) {
        printf("TX result: ERROR %s (%d)\n", libusb_error_name(ret), ret);
    } else {
        printf("TX result: OK transferred=%d\n", ret);
    }

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

static int ks959_rx_decode_chunk(ks959_rx_state_t *state,
                                 const uint8_t *in_buf,
                                 int in_len,
                                 uint8_t *out_buf,
                                 int out_buf_size)
{
    int i;
    int produced = 0;

    for (i = 0; i < in_len; ++i) {
        uint8_t decoded;

        state->xormask++;
        decoded = (uint8_t)(in_buf[i] ^ state->xormask ^ 0x55u);

        if (state->xormask == 0u) {
            continue;
        }
        if (produced >= out_buf_size) {
            break;
        }

        out_buf[produced++] = decoded;
    }

    return produced;
}

static void poll_after_step(libusb_device_handle *handle,
                            ks959_rx_state_t *rx_state,
                            int step_no,
                            int polls,
                            int sleep_ms)
{
    uint8_t raw[RX_BUF_SIZE];
    uint8_t dec[RX_BUF_SIZE];
    int i;

    for (i = 0; i < polls; ++i) {
        int ret;
        int dec_len;

        memset(raw, 0, sizeof(raw));
        ret = ks959_rx_poll(handle, raw, sizeof(raw));

        if (ret < 0) {
            printf("[step %d poll %d] RX ERROR %s (%d)\n",
                   step_no, i + 1, libusb_error_name(ret), ret);
            msleep(sleep_ms);
            continue;
        }

        if (ret == 0) {
            printf("[step %d poll %d] idle\n", step_no, i + 1);
            msleep(sleep_ms);
            continue;
        }

        printf("[step %d poll %d] raw_len=%d state_before=0x%02X\n",
               step_no, i + 1, ret, rx_state->xormask);
        hex_dump("Raw", raw, ret, 128);

        dec_len = ks959_rx_decode_chunk(rx_state, raw, ret, dec, sizeof(dec));
        printf("[step %d poll %d] dec_len=%d state_after=0x%02X\n",
               step_no, i + 1, dec_len, rx_state->xormask);
        if (dec_len > 0) {
            hex_dump("Decoded", dec, dec_len, 128);
        }

        msleep(sleep_ms);
    }
}

int main(int argc, char **argv)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;
    ks959_rx_state_t rx_state = {0};
    int polls = 12;
    int sleep_ms = 150;
    int ret;
    int step = 0;
    size_t i;

    static const uint8_t frame_a[] = { 0x1B };
    static const uint8_t frame_b[] = { 0x1B, 0x1B };
    static const uint8_t frame_c[] = { 0x00 };
    static const uint8_t frame_d[] = { 0x00, 0x00 };
    static const uint8_t frame_e[] = { 0x10, 0x00 };
    static const uint8_t frame_f[] = { 0x10, 0x01 };

    static const test_frame_t frames[] = {
        { "frame_a", frame_a, sizeof(frame_a) },
        { "frame_b", frame_b, sizeof(frame_b) },
        { "frame_c", frame_c, sizeof(frame_c) },
        { "frame_d", frame_d, sizeof(frame_d) },
        { "frame_e", frame_e, sizeof(frame_e) },
        { "frame_f", frame_f, sizeof(frame_f) }
    };

    if (argc >= 2) {
        polls = atoi(argv[1]);
        if (polls <= 0) {
            polls = 12;
        }
    }
    if (argc >= 3) {
        sleep_ms = atoi(argv[2]);
        if (sleep_ms < 0) {
            sleep_ms = 150;
        }
    }

    ret = open_ks959(&ctx, &handle);
    if (ret != 0) {
        return 1;
    }

    printf("Opened KS-959 %04X:%04X\n", KS959_VID, KS959_PID);
    printf("LOW/9600-focused handshake probe: polls=%d sleep_ms=%d\n", polls, sleep_ms);
    printf("Put the Aladin Prime in LOW mode and align it carefully.\n");

    step++;
    (void)ks959_send_out8_9600(handle, 0x0200u, "STEP 1 - preinit 9600 @ wValue=0x0200");
    poll_after_step(handle, &rx_state, step, polls, sleep_ms);

    step++;
    (void)ks959_send_out8_9600(handle, 0x0000u, "STEP 2 - preinit 9600 @ wValue=0x0000");
    poll_after_step(handle, &rx_state, step, polls, sleep_ms);

    for (i = 0; i < sizeof(frames) / sizeof(frames[0]); ++i) {
        char label[128];

        step++;
        snprintf(label, sizeof(label), "STEP %d - %s", step, frames[i].name);
        (void)ks959_send_fragment(handle, frames[i].data, frames[i].len, label);
        poll_after_step(handle, &rx_state, step, polls, sleep_ms);
    }

    close_ks959(ctx, handle);
    return 0;
}