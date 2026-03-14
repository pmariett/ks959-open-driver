#include <libusb-1.0/libusb.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define KS959_VID 0x07D0
#define KS959_PID 0x4959

#define IFACE_NUM 0
#define EP_INT_IN 0x81

#define TIMEOUT_MS 1000

#define REQTYPE_OUT_CLASS_IFACE 0x21
#define KS959_REQ_OUT 0x09

static volatile sig_atomic_t stop_flag = 0;

typedef struct {
    uint8_t baud_le[4];
    uint8_t flags;
    uint8_t reserved[3];
} ks959_out8_payload;

static void on_sigint(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static void write_le32(uint32_t v, uint8_t out[4])
{
    out[0] = v & 0xFF;
    out[1] = (v >> 8) & 0xFF;
    out[2] = (v >> 16) & 0xFF;
    out[3] = (v >> 24) & 0xFF;
}

static void hex_dump(const uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        if ((i % 16) == 0)
            printf("  %04x :", i);
        printf(" %02X", buf[i]);
        if ((i % 16) == 15 || i == len - 1)
            printf("\n");
    }
}

static int send_preinit(libusb_device_handle *h)
{
    ks959_out8_payload p;
    memset(&p, 0, sizeof(p));

    write_le32(9600, p.baud_le);
    p.flags = 0x03;

    printf("Sending pre-init command\n");

    int r = libusb_control_transfer(
        h,
        REQTYPE_OUT_CLASS_IFACE,
        KS959_REQ_OUT,
        0x0200,
        0x0000,
        (uint8_t *)&p,
        sizeof(p),
        TIMEOUT_MS
    );

    if (r < 0)
        printf("preinit failed: %s\n", libusb_error_name(r));
    else
        printf("preinit OK (%d bytes)\n", r);

    return r;
}

int main(int argc, char **argv)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;

    int do_preinit = 1;

    if (argc > 1)
        do_preinit = atoi(argv[1]);

    signal(SIGINT, on_sigint);

    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "libusb_init failed\n");
        return 1;
    }

    handle = libusb_open_device_with_vid_pid(ctx, KS959_VID, KS959_PID);
    if (!handle) {
        fprintf(stderr, "KS-959 not found\n");
        libusb_exit(ctx);
        return 1;
    }

    if (libusb_kernel_driver_active(handle, IFACE_NUM) == 1)
        libusb_detach_kernel_driver(handle, IFACE_NUM);

    if (libusb_claim_interface(handle, IFACE_NUM) != 0) {
        fprintf(stderr, "claim_interface failed\n");
        libusb_close(handle);
        libusb_exit(ctx);
        return 1;
    }

    printf("Opened KS-959\n");

    if (do_preinit)
        send_preinit(handle);

    printf("Listening on interrupt endpoint 0x81\n");
    printf("Point an IR remote at the dongle...\n");

    uint8_t buf[64];

    while (!stop_flag) {

        int transferred = 0;

        int r = libusb_interrupt_transfer(
            handle,
            EP_INT_IN,
            buf,
            sizeof(buf),
            &transferred,
            TIMEOUT_MS
        );

        if (r == LIBUSB_ERROR_TIMEOUT)
            continue;

        if (r < 0) {
            printf("interrupt error: %s\n", libusb_error_name(r));
            continue;
        }

        if (transferred > 0) {
            printf("\nINT data (%d bytes):\n", transferred);
            hex_dump(buf, transferred);
        }
    }

    printf("Stopping monitor\n");

    libusb_release_interface(handle, IFACE_NUM);
    libusb_close(handle);
    libusb_exit(ctx);

    return 0;
}