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

#define RX_BUF_SIZE 255

static volatile sig_atomic_t g_stop = 0;

typedef struct ks959_out8_payload {
    uint8_t param0_le[4];
    uint8_t param1;
    uint8_t reserved[3];
} ks959_out8_payload_t;

typedef struct ks959_seq_step {
    uint16_t wValue;
    ks959_out8_payload_t payload;
    const char *label;
} ks959_seq_step_t;

typedef struct ks959_sequence {
    const char *name;
    const ks959_seq_step_t *steps;
    size_t step_count;
} ks959_sequence_t;

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

static void fill_payload(ks959_out8_payload_t *p, uint32_t param0, uint8_t param1)
{
    memset(p, 0, sizeof(*p));
    write_le32(param0, p->param0_le);
    p->param1 = param1;
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

static int send_out8(libusb_device_handle *handle,
                     uint16_t wValue,
                     const ks959_out8_payload_t *payload,
                     const char *label)
{
    uint8_t tmp[8];
    int ret;

    memcpy(tmp, payload, sizeof(tmp));

    printf("  OUT %-24s wValue=0x%04X payload=", label, wValue);
    for (int i = 0; i < 8; ++i) {
        printf("%02X", tmp[i]);
        if (i != 7) {
            printf(" ");
        }
    }
    printf("\n");

    ret = libusb_control_transfer(handle,
                                  REQTYPE_OUT_CLASS_IFACE,
                                  KS959_REQ_OUT,
                                  wValue,
                                  0x0000,
                                  tmp,
                                  8,
                                  TIMEOUT_MS);

    if (ret < 0) {
        printf("    -> ERROR %s (%d)\n", libusb_error_name(ret), ret);
    } else {
        printf("    -> OK transferred=%d\n", ret);
    }

    return ret;
}

static int rx_poll_once(libusb_device_handle *handle, uint8_t *buf, int buf_len)
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

static int monitor_after_sequence(libusb_device_handle *handle,
                                  int polls,
                                  int sleep_ms)
{
    uint8_t raw[RX_BUF_SIZE];
    struct timespec ts;
    int i;

    ts.tv_sec = sleep_ms / 1000;
    ts.tv_nsec = (long)(sleep_ms % 1000) * 1000000L;

    for (i = 0; i < polls && !g_stop; ++i) {
        int ret;

        memset(raw, 0, sizeof(raw));
        ret = rx_poll_once(handle, raw, sizeof(raw));

        if (ret < 0) {
            printf("    poll %d -> ERROR %s (%d)\n", i + 1, libusb_error_name(ret), ret);
        } else if (ret == 0) {
            printf("    poll %d -> idle\n", i + 1);
        } else {
            printf("    poll %d -> DATA len=%d\n", i + 1, ret);
            hex_dump("    Raw", raw, ret, 128);
            return 1;
        }

        nanosleep(&ts, NULL);
    }

    return 0;
}

static int run_sequence(libusb_device_handle *handle,
                        const ks959_sequence_t *seq,
                        int polls,
                        int sleep_ms)
{
    size_t i;

    printf("\n============================================================\n");
    printf("Sequence: %s\n", seq->name);
    printf("============================================================\n");

    for (i = 0; i < seq->step_count && !g_stop; ++i) {
        (void)send_out8(handle, seq->steps[i].wValue, &seq->steps[i].payload, seq->steps[i].label);
    }

    printf("  Monitoring after sequence...\n");
    return monitor_after_sequence(handle, polls, sleep_ms);
}

int main(int argc, char **argv)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;
    int polls = 20;
    int sleep_ms = 100;
    int ret;

    ks959_out8_payload_t p_9600_03, p_9600_00, p_2400_03, p_zero_00, p_19200_03;
    fill_payload(&p_9600_03, 9600u, 0x03u);
    fill_payload(&p_9600_00, 9600u, 0x00u);
    fill_payload(&p_2400_03, 2400u, 0x03u);
    fill_payload(&p_zero_00, 0u, 0x00u);
    fill_payload(&p_19200_03, 19200u, 0x03u);

    const ks959_seq_step_t seq1_steps[] = {
        {0x0200u, { {0,0,0,0}, 0, {0,0,0} }, "9600/03 @0200"},
    };
    const ks959_seq_step_t seq2_steps[] = {
        {0x0000u, { {0,0,0,0}, 0, {0,0,0} }, "9600/03 @0000"},
    };
    const ks959_seq_step_t seq3_steps[] = {
        {0x0200u, { {0,0,0,0}, 0, {0,0,0} }, "9600/03 @0200"},
        {0x0000u, { {0,0,0,0}, 0, {0,0,0} }, "9600/03 @0000"},
    };
    const ks959_seq_step_t seq4_steps[] = {
        {0x0000u, { {0,0,0,0}, 0, {0,0,0} }, "zero/00 @0000"},
        {0x0200u, { {0,0,0,0}, 0, {0,0,0} }, "9600/03 @0200"},
    };
    const ks959_seq_step_t seq5_steps[] = {
        {0x0200u, { {0,0,0,0}, 0, {0,0,0} }, "2400/03 @0200"},
        {0x0200u, { {0,0,0,0}, 0, {0,0,0} }, "9600/03 @0200"},
    };
    const ks959_seq_step_t seq6_steps[] = {
        {0x0200u, { {0,0,0,0}, 0, {0,0,0} }, "9600/00 @0200"},
        {0x0200u, { {0,0,0,0}, 0, {0,0,0} }, "9600/03 @0200"},
    };
    const ks959_seq_step_t seq7_steps[] = {
        {0x0200u, { {0,0,0,0}, 0, {0,0,0} }, "19200/03 @0200"},
        {0x0200u, { {0,0,0,0}, 0, {0,0,0} }, "9600/03 @0200"},
        {0x0000u, { {0,0,0,0}, 0, {0,0,0} }, "9600/03 @0000"},
    };

    ks959_sequence_t sequences[] = {
        {"single 9600/03 @0200", (ks959_seq_step_t *)seq1_steps, 1},
        {"single 9600/03 @0000", (ks959_seq_step_t *)seq2_steps, 1},
        {"double 9600/03 @0200 then @0000", (ks959_seq_step_t *)seq3_steps, 2},
        {"zero then 9600/03", (ks959_seq_step_t *)seq4_steps, 2},
        {"2400 then 9600", (ks959_seq_step_t *)seq5_steps, 2},
        {"9600 flags00 then flags03", (ks959_seq_step_t *)seq6_steps, 2},
        {"19200 then 9600 then @0000", (ks959_seq_step_t *)seq7_steps, 3},
    };

    memcpy((void *)&seq1_steps[0].payload, &p_9600_03, sizeof(p_9600_03));
    memcpy((void *)&seq2_steps[0].payload, &p_9600_03, sizeof(p_9600_03));
    memcpy((void *)&seq3_steps[0].payload, &p_9600_03, sizeof(p_9600_03));
    memcpy((void *)&seq3_steps[1].payload, &p_9600_03, sizeof(p_9600_03));
    memcpy((void *)&seq4_steps[0].payload, &p_zero_00, sizeof(p_zero_00));
    memcpy((void *)&seq4_steps[1].payload, &p_9600_03, sizeof(p_9600_03));
    memcpy((void *)&seq5_steps[0].payload, &p_2400_03, sizeof(p_2400_03));
    memcpy((void *)&seq5_steps[1].payload, &p_9600_03, sizeof(p_9600_03));
    memcpy((void *)&seq6_steps[0].payload, &p_9600_00, sizeof(p_9600_00));
    memcpy((void *)&seq6_steps[1].payload, &p_9600_03, sizeof(p_9600_03));
    memcpy((void *)&seq7_steps[0].payload, &p_19200_03, sizeof(p_19200_03));
    memcpy((void *)&seq7_steps[1].payload, &p_9600_03, sizeof(p_9600_03));
    memcpy((void *)&seq7_steps[2].payload, &p_9600_03, sizeof(p_9600_03));

    if (argc >= 2) {
        polls = atoi(argv[1]);
        if (polls <= 0) {
            polls = 20;
        }
    }
    if (argc >= 3) {
        sleep_ms = atoi(argv[2]);
        if (sleep_ms < 0) {
            sleep_ms = 100;
        }
    }

    signal(SIGINT, on_sigint);

    ret = open_ks959(&ctx, &handle);
    if (ret != 0) {
        return 1;
    }

    printf("Opened KS-959 %04X:%04X\n", KS959_VID, KS959_PID);
    printf("Arm scan settings: polls=%d sleep_ms=%d\n", polls, sleep_ms);
    printf("Important: emit IR continuously toward the dongle during the scan.\n");

    for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]) && !g_stop; ++i) {
        int got_data = run_sequence(handle, &sequences[i], polls, sleep_ms);
        if (got_data) {
            printf("\n*** SUCCESS: data observed after sequence \"%s\" ***\n", sequences[i].name);
            break;
        }
    }

    close_ks959(ctx, handle);
    return 0;
}