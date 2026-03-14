#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
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

typedef struct read_probe {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    const char *label;
} read_probe_t;

typedef struct write_probe {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    const char *label;
} write_probe_t;

static void run_read_probe(ks959_device_t *dev, const read_probe_t *p) {
    ks959_setup_packet_t setup;
    uint8_t buffer[256];
    size_t transferred = 0;
    uint32_t os_error = 0;
    int rc;

    memset(&setup, 0, sizeof(setup));
    memset(buffer, 0, sizeof(buffer));

    setup.request_type = p->request_type;
    setup.request = p->request;
    setup.value = p->value;
    setup.index = p->index;
    setup.length = p->length;

    printf("[READ ] %s\n", p->label);
    printf("        RT=0x%02X R=0x%02X V=0x%04X I=0x%04X L=%u\n",
           setup.request_type, setup.request, setup.value, setup.index, setup.length);

    rc = ks959_ctrl_read(dev, &setup, buffer, setup.length, &transferred);
    if (rc == KS959_OK) {
        printf("        OK, %zu byte(s): ", transferred);
        dump_hex(buffer, transferred);
    } else {
        printf("        FAIL rc=%d", rc);
        if (ks959_last_os_error(dev, &os_error) == KS959_OK) {
            printf(" os=%lu", (unsigned long)os_error);
        }
        printf("\n");
    }
}

static void run_write_probe(ks959_device_t *dev, const write_probe_t *p) {
    ks959_setup_packet_t setup;
    size_t transferred = 0;
    uint32_t os_error = 0;
    int rc;

    memset(&setup, 0, sizeof(setup));

    setup.request_type = p->request_type;
    setup.request = p->request;
    setup.value = p->value;
    setup.index = p->index;
    setup.length = p->length;

    printf("[WRITE] %s\n", p->label);
    printf("        RT=0x%02X R=0x%02X V=0x%04X I=0x%04X L=%u\n",
           setup.request_type, setup.request, setup.value, setup.index, setup.length);

    rc = ks959_ctrl_write(dev, &setup, NULL, 0, &transferred);
    if (rc == KS959_OK) {
        printf("        OK, transferred=%zu\n", transferred);
    } else {
        printf("        FAIL rc=%d", rc);
        if (ks959_last_os_error(dev, &os_error) == KS959_OK) {
            printf(" os=%lu", (unsigned long)os_error);
        }
        printf("\n");
    }
}

int main(void) {
    ks959_device_t *dev = NULL;
    int rc;
    uint16_t vid = 0, pid = 0;
    unsigned major = 0, minor = 0, patch = 0;
    size_t i;

    const read_probe_t read_probes[] = {
        {0xC0, 0x00, 0x0000, 0x0000, 4, "GET C0 idx0"},
        {0xC0, 0x00, 0x0000, 0x0001, 4, "GET C0 idx1"},
        {0xA1, 0x00, 0x0000, 0x0000, 4, "GET A1 idx0"},
        {0xA1, 0x00, 0x0000, 0x0001, 4, "GET A1 idx1"},
        {0xA3, 0x00, 0x0000, 0x0000, 4, "GET A3 idx0"},
        {0xA3, 0x00, 0x0000, 0x0001, 4, "GET A3 idx1"},
    };

    const write_probe_t write_probes[] = {
        {0x40, 0x01, 0x0010, 0x0000, 0, "SET 40 01 0010 idx0"},
        {0x40, 0x01, 0x0010, 0x0001, 0, "SET 40 01 0010 idx1"},
        {0x21, 0x01, 0x0010, 0x0000, 0, "SET 21 01 0010 idx0"},
        {0x21, 0x01, 0x0010, 0x0001, 0, "SET 21 01 0010 idx1"},
        {0x23, 0x01, 0x0010, 0x0000, 0, "SET 23 01 0010 idx0"},
        {0x23, 0x01, 0x0010, 0x0001, 0, "SET 23 01 0010 idx1"},

        {0x40, 0x01, 0x0011, 0x0000, 0, "SET 40 01 0011 idx0"},
        {0x40, 0x01, 0x0011, 0x0001, 0, "SET 40 01 0011 idx1"},
        {0x21, 0x01, 0x0011, 0x0000, 0, "SET 21 01 0011 idx0"},
        {0x21, 0x01, 0x0011, 0x0001, 0, "SET 21 01 0011 idx1"},
        {0x23, 0x01, 0x0011, 0x0000, 0, "SET 23 01 0011 idx0"},
        {0x23, 0x01, 0x0011, 0x0001, 0, "SET 23 01 0011 idx1"},

        {0x40, 0x03, 0x0008, 0x0000, 0, "SET 40 03 0008 idx0"},
        {0x40, 0x03, 0x0008, 0x0001, 0, "SET 40 03 0008 idx1"},
        {0x21, 0x03, 0x0008, 0x0000, 0, "SET 21 03 0008 idx0"},
        {0x21, 0x03, 0x0008, 0x0001, 0, "SET 21 03 0008 idx1"},
        {0x23, 0x03, 0x0008, 0x0000, 0, "SET 23 03 0008 idx0"},
        {0x23, 0x03, 0x0008, 0x0001, 0, "SET 23 03 0008 idx1"},
    };

    printf("ks959-ctrl-matrix starting...\n");

    ks959_get_version(&major, &minor, &patch);
    printf("Library version: %u.%u.%u\n", major, minor, patch);

    rc = ks959_open(&dev);
    if (rc != KS959_OK) {
        printf("ks959_open failed: %d\n", rc);
        return 1;
    }

    if (ks959_get_vid_pid(dev, &vid, &pid) == KS959_OK) {
        printf("Device VID:PID = %04X:%04X\n", vid, pid);
    }

    printf("\n=== READ PROBES ===\n");
    for (i = 0; i < sizeof(read_probes) / sizeof(read_probes[0]); ++i) {
        run_read_probe(dev, &read_probes[i]);
    }

    printf("\n=== WRITE PROBES ===\n");
    for (i = 0; i < sizeof(write_probes) / sizeof(write_probes[0]); ++i) {
        run_write_probe(dev, &write_probes[i]);
    }

    ks959_close(dev);
    printf("\nDone.\n");
    return 0;
}