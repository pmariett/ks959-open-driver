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

#define KS959_REQ_OUT 0x09
#define KS959_REQ_RX  0x01

#define KS959_RX_WVALUE 0x0200
#define KS959_RX_WINDEX 0x0000

#define MAX_PAYLOAD 64

typedef struct test_case {
    const char *name;
    uint16_t wValue;
    uint16_t wIndex;
    uint8_t payload[MAX_PAYLOAD];
    uint16_t payload_len;
    int repeat_count;
    int repeat_delay_ms;
} test_case_t;

static void msleep(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
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

static int send_out(libusb_device_handle *handle,
                    uint16_t wValue,
                    uint16_t wIndex,
                    const uint8_t *payload,
                    uint16_t payload_len)
{
    uint8_t tmp[MAX_PAYLOAD];

    if (payload_len > sizeof(tmp)) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    if (payload_len > 0 && payload) {
        memcpy(tmp, payload, payload_len);
    }

    return libusb_control_transfer(handle,
                                   REQTYPE_OUT_CLASS_IFACE,
                                   KS959_REQ_OUT,
                                   wValue,
                                   wIndex,
                                   (payload_len > 0) ? tmp : NULL,
                                   payload_len,
                                   TIMEOUT_MS);
}

static int rx_poll_once(libusb_device_handle *handle)
{
    uint8_t buf[64];
    int ret;

    memset(buf, 0, sizeof(buf));

    ret = libusb_control_transfer(handle,
                                  REQTYPE_IN_CLASS_IFACE,
                                  KS959_REQ_RX,
                                  KS959_RX_WVALUE,
                                  KS959_RX_WINDEX,
                                  buf,
                                  sizeof(buf),
                                  TIMEOUT_MS);

    if (ret < 0) {
        printf("  RX poll -> ERROR %s (%d)\n", libusb_error_name(ret), ret);
    } else {
        printf("  RX poll -> OK transferred=%d\n", ret);
        if (ret > 0) {
            hex_dump("  RX data", buf, ret, 64);
        }
    }

    return ret;
}

static void run_case(libusb_device_handle *handle, const test_case_t *tc)
{
    int i;

    printf("\n============================================================\n");
    printf("TEST: %s\n", tc->name);
    printf("wValue=0x%04X wIndex=0x%04X len=%u repeat=%d delay_ms=%d\n",
           tc->wValue, tc->wIndex, tc->payload_len, tc->repeat_count, tc->repeat_delay_ms);

    if (tc->payload_len > 0) {
        hex_dump("Payload", tc->payload, tc->payload_len, 64);
    } else {
        printf("Payload: <none>\n");
    }

    printf("Look at the webcam NOW.\n");

    for (i = 0; i < tc->repeat_count; ++i) {
        int ret = send_out(handle,
                           tc->wValue,
                           tc->wIndex,
                           tc->payload,
                           tc->payload_len);

        if (ret < 0) {
            printf("  send %d/%d -> ERROR %s (%d)\n",
                   i + 1, tc->repeat_count, libusb_error_name(ret), ret);
        } else {
            printf("  send %d/%d -> OK transferred=%d\n",
                   i + 1, tc->repeat_count, ret);
        }

        msleep(tc->repeat_delay_ms);
    }

    rx_poll_once(handle);
    printf("Pause for webcam observation...\n");
    msleep(1500);
}

static void fill_repeated(uint8_t *dst, uint16_t len, uint8_t value)
{
    uint16_t i;
    for (i = 0; i < len; ++i) {
        dst[i] = value;
    }
}

int main(void)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;
    int ret;

    test_case_t cases[12];
    size_t case_count = 0;
    size_t i;

    memset(cases, 0, sizeof(cases));

    /* 1: known accepted 8-byte 9600-ish payload */
    cases[case_count].name = "known accepted 8-byte payload @0200";
    cases[case_count].wValue = 0x0200;
    cases[case_count].wIndex = 0x0000;
    cases[case_count].payload_len = 8;
    cases[case_count].repeat_count = 40;
    cases[case_count].repeat_delay_ms = 30;
    cases[case_count].payload[0] = 0x80;
    cases[case_count].payload[1] = 0x25;
    cases[case_count].payload[2] = 0x00;
    cases[case_count].payload[3] = 0x00;
    cases[case_count].payload[4] = 0x03;
    case_count++;

    /* 2: same payload @0000 */
    cases[case_count] = cases[0];
    cases[case_count].name = "known accepted 8-byte payload @0000";
    cases[case_count].wValue = 0x0000;
    case_count++;

    /* 3: one byte FF */
    cases[case_count].name = "1-byte FF @0001";
    cases[case_count].wValue = 0x0001;
    cases[case_count].wIndex = 0x0000;
    cases[case_count].payload_len = 1;
    cases[case_count].repeat_count = 120;
    cases[case_count].repeat_delay_ms = 10;
    cases[case_count].payload[0] = 0xFF;
    case_count++;

    /* 4: one byte 00 */
    cases[case_count] = cases[2];
    cases[case_count].name = "1-byte 00 @0001";
    cases[case_count].payload[0] = 0x00;
    case_count++;

    /* 5: 16 bytes all FF */
    cases[case_count].name = "16-byte all FF @0010";
    cases[case_count].wValue = 0x0010;
    cases[case_count].wIndex = 0x0000;
    cases[case_count].payload_len = 16;
    cases[case_count].repeat_count = 60;
    cases[case_count].repeat_delay_ms = 20;
    fill_repeated(cases[case_count].payload, 16, 0xFF);
    case_count++;

    /* 6: 16 bytes all 00 */
    cases[case_count] = cases[4];
    cases[case_count].name = "16-byte all 00 @0010";
    fill_repeated(cases[case_count].payload, 16, 0x00);
    case_count++;

    /* 7: 16 bytes AA */
    cases[case_count] = cases[4];
    cases[case_count].name = "16-byte all AA @0010";
    fill_repeated(cases[case_count].payload, 16, 0xAA);
    case_count++;

    /* 8: 16 bytes 55 */
    cases[case_count] = cases[4];
    cases[case_count].name = "16-byte all 55 @0010";
    fill_repeated(cases[case_count].payload, 16, 0x55);
    case_count++;

    /* 9: 32-byte ramp */
    cases[case_count].name = "32-byte ramp @0020";
    cases[case_count].wValue = 0x0020;
    cases[case_count].wIndex = 0x0000;
    cases[case_count].payload_len = 32;
    cases[case_count].repeat_count = 40;
    cases[case_count].repeat_delay_ms = 25;
    for (i = 0; i < 32; ++i) {
        cases[case_count].payload[i] = (uint8_t)i;
    }
    case_count++;

    /* 10: no payload */
    cases[case_count].name = "no payload @0000";
    cases[case_count].wValue = 0x0000;
    cases[case_count].wIndex = 0x0000;
    cases[case_count].payload_len = 0;
    cases[case_count].repeat_count = 40;
    cases[case_count].repeat_delay_ms = 20;
    case_count++;

    /* 11: no payload @0200 */
    cases[case_count] = cases[9];
    cases[case_count].name = "no payload @0200";
    cases[case_count].wValue = 0x0200;
    case_count++;

    /* 12: known payload but wIndex=1, expected fail maybe useful */
    cases[case_count] = cases[0];
    cases[case_count].name = "known payload @0200 wIndex=0001";
    cases[case_count].wIndex = 0x0001;
    cases[case_count].repeat_count = 8;
    cases[case_count].repeat_delay_ms = 50;
    case_count++;

    ret = open_ks959(&ctx, &handle);
    if (ret != 0) {
        return 1;
    }

    printf("Opened KS-959 %04X:%04X\n", KS959_VID, KS959_PID);
    printf("IR LED finder started.\n");
    printf("Use the Windows webcam pointed at the dongle IR window.\n");
    printf("Watch for ANY white/violet flash during each test block.\n");

    for (i = 0; i < case_count; ++i) {
        run_case(handle, &cases[i]);
    }

    close_ks959(ctx, handle);
    return 0;
}