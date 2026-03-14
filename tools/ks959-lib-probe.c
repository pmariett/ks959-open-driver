#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "ks959.h"

static void dump_hex(const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

static int parse_u8(const char *s, uint8_t *out) {
    unsigned long value;
    char *end = NULL;

    if (s == NULL || out == NULL) {
        return 0;
    }

    value = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || value > 0xFFUL) {
        return 0;
    }

    *out = (uint8_t)value;
    return 1;
}

static int parse_u16(const char *s, uint16_t *out) {
    unsigned long value;
    char *end = NULL;

    if (s == NULL || out == NULL) {
        return 0;
    }

    value = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || value > 0xFFFFUL) {
        return 0;
    }

    *out = (uint16_t)value;
    return 1;
}

static void print_usage(const char *exe) {
    printf("Usage:\n");
    printf("  %s\n", exe);
    printf("  %s <request_type> <request> <value> <index> <length>\n", exe);
    printf("\n");
    printf("Examples:\n");
    printf("  %s\n", exe);
    printf("  %s 0xA3 0x00 0x0000 0x0001 4\n", exe);
    printf("  %s 0xC0 0x00 0x0000 0x0000 4\n", exe);
    printf("\n");
    printf("Without arguments, the program only opens the device and prints info.\n");
}

int main(int argc, char **argv) {
    ks959_device_t *dev = NULL;
    int rc;
    unsigned ver_major = 0, ver_minor = 0, ver_patch = 0;
    uint16_t vid = 0, pid = 0;
    uint32_t os_error = 0;

    printf("ks959-lib-probe starting...\n");

    rc = ks959_get_version(&ver_major, &ver_minor, &ver_patch);
    if (rc == KS959_OK) {
        printf("Library version: %u.%u.%u\n", ver_major, ver_minor, ver_patch);
    }

    rc = ks959_open(&dev);
    if (rc != KS959_OK) {
        printf("ks959_open failed: %d\n", rc);
        return 1;
    }

    printf("ks959_open succeeded.\n");

    rc = ks959_get_vid_pid(dev, &vid, &pid);
    if (rc == KS959_OK) {
        printf("Device VID:PID = %04X:%04X\n", vid, pid);
    }

    if (argc == 1) {
        printf("No control transfer requested.\n");
        ks959_close(dev);
        printf("Done.\n");
        return 0;
    }

    if (argc != 6) {
        print_usage(argv[0]);
        ks959_close(dev);
        return 2;
    }

    {
        ks959_setup_packet_t setup;
        uint8_t buffer[256];
        size_t transferred = 0;

        memset(&setup, 0, sizeof(setup));
        memset(buffer, 0, sizeof(buffer));

        if (!parse_u8(argv[1], &setup.request_type) ||
            !parse_u8(argv[2], &setup.request) ||
            !parse_u16(argv[3], &setup.value) ||
            !parse_u16(argv[4], &setup.index) ||
            !parse_u16(argv[5], &setup.length)) {
            printf("Invalid numeric argument.\n");
            print_usage(argv[0]);
            ks959_close(dev);
            return 3;
        }

        if (setup.length > sizeof(buffer)) {
            printf("Length too large. Max supported by this probe: %zu\n", sizeof(buffer));
            ks959_close(dev);
            return 4;
        }

        printf("Control transfer request:\n");
        printf("  RequestType : 0x%02X\n", setup.request_type);
        printf("  Request     : 0x%02X\n", setup.request);
        printf("  Value       : 0x%04X\n", setup.value);
        printf("  Index       : 0x%04X\n", setup.index);
        printf("  Length      : %u\n", setup.length);

        if ((setup.request_type & 0x80u) != 0) {
            rc = ks959_ctrl_read(dev, &setup, buffer, setup.length, &transferred);
            if (rc != KS959_OK) {
                printf("ks959_ctrl_read failed: %d\n", rc);
                if (ks959_last_os_error(dev, &os_error) == KS959_OK) {
                    printf("Last OS error: %lu\n", (unsigned long)os_error);
                }
                ks959_close(dev);
                return 5;
            }

            printf("Read OK, %zu byte(s): ", transferred);
            dump_hex(buffer, transferred);
        } else {
            rc = ks959_ctrl_write(dev, &setup, NULL, 0, &transferred);
            if (setup.length != 0) {
                printf("This probe only supports zero-length OUT transfers for now.\n");
                ks959_close(dev);
                return 6;
            }

            if (rc != KS959_OK) {
                printf("ks959_ctrl_write failed: %d\n", rc);
                if (ks959_last_os_error(dev, &os_error) == KS959_OK) {
                    printf("Last OS error: %lu\n", (unsigned long)os_error);
                }
                ks959_close(dev);
                return 7;
            }

            printf("Write OK, transferred=%zu\n", transferred);
        }
    }

    ks959_close(dev);
    printf("Done.\n");
    return 0;
}