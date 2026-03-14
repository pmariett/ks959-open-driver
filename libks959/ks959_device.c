#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include "ks959.h"

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usbiodef.h>
#include <initguid.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>


#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

struct ks959_device {
    HANDLE device_handle;
    WINUSB_INTERFACE_HANDLE winusb_handle;
    uint16_t vid;
    uint16_t pid;
    uint32_t last_os_error;
};

static void ks959_set_last_error(ks959_device_t *dev, DWORD err) {
    if (dev != NULL) {
        dev->last_os_error = (uint32_t)err;
    }
}

static void ks959_set_last_error_from_system(ks959_device_t *dev) {
    ks959_set_last_error(dev, GetLastError());
}

static int ks959_map_open_error(DWORD err) {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NO_MORE_ITEMS:
            return KS959_ERR_NO_DEVICE;

        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_INVALID_HANDLE:
            return KS959_ERR_OPEN_FAILED;

        default:
            return KS959_ERR_IO;
    }
}

static BOOL ks959_path_contains_vid_pid(const wchar_t *device_path,
                                        uint16_t vid,
                                        uint16_t pid) {
    wchar_t pattern[64];
    wchar_t lower_path[2048];
    size_t i = 0;

    if (device_path == NULL) {
        return FALSE;
    }

    _snwprintf(pattern, 64, L"vid_%04x&pid_%04x", vid, pid);
	pattern[63] = L'\0';

    for (; device_path[i] != L'\0' &&
           i < (sizeof(lower_path) / sizeof(lower_path[0])) - 1; ++i) {
        wchar_t c = device_path[i];
        if (c >= L'A' && c <= L'Z') {
            c = (wchar_t)(c - L'A' + L'a');
        }
        lower_path[i] = c;
    }
    lower_path[i] = L'\0';

    return wcsstr(lower_path, pattern) != NULL;
}

static int ks959_get_device_path(uint16_t vid,
                                 uint16_t pid,
                                 wchar_t **out_device_path) {
    HDEVINFO dev_info = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA interface_data;
    DWORD index = 0;

    if (out_device_path == NULL) {
        return KS959_ERR_INVALID_ARG;
    }

    *out_device_path = NULL;

    dev_info = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_USB_DEVICE,
        NULL,
        NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (dev_info == INVALID_HANDLE_VALUE) {
        return KS959_ERR_IO;
    }

    ZeroMemory(&interface_data, sizeof(interface_data));
    interface_data.cbSize = sizeof(interface_data);

    while (SetupDiEnumDeviceInterfaces(
        dev_info,
        NULL,
        &GUID_DEVINTERFACE_USB_DEVICE,
        index,
        &interface_data))
    {
        DWORD required_size = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail_data = NULL;
        int result = KS959_ERR_NO_DEVICE;

        SetupDiGetDeviceInterfaceDetailW(
            dev_info,
            &interface_data,
            NULL,
            0,
            &required_size,
            NULL
        );

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required_size == 0) {
            ++index;
            continue;
        }

        detail_data = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(required_size);
        if (detail_data == NULL) {
            SetupDiDestroyDeviceInfoList(dev_info);
            return KS959_ERR_INTERNAL;
        }

        ZeroMemory(detail_data, required_size);
        detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(
                dev_info,
                &interface_data,
                detail_data,
                required_size,
                NULL,
                NULL))
        {
            free(detail_data);
            ++index;
            continue;
        }

        if (ks959_path_contains_vid_pid(detail_data->DevicePath, vid, pid)) {
            size_t chars = wcslen(detail_data->DevicePath) + 1;
            *out_device_path = (wchar_t *)malloc(chars * sizeof(wchar_t));
            if (*out_device_path == NULL) {
                free(detail_data);
                SetupDiDestroyDeviceInfoList(dev_info);
                return KS959_ERR_INTERNAL;
            }

            wcscpy(*out_device_path, detail_data->DevicePath);
            free(detail_data);
            SetupDiDestroyDeviceInfoList(dev_info);
            return KS959_OK;
        }

        free(detail_data);
        (void)result;
        ++index;
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return KS959_ERR_NO_DEVICE;
}

int ks959_get_version(unsigned *major, unsigned *minor, unsigned *patch) {
    if (major != NULL) {
        *major = KS959_VERSION_MAJOR;
    }
    if (minor != NULL) {
        *minor = KS959_VERSION_MINOR;
    }
    if (patch != NULL) {
        *patch = KS959_VERSION_PATCH;
    }
    return KS959_OK;
}

int ks959_get_vid_pid(ks959_device_t *dev, uint16_t *vid, uint16_t *pid) {
    if (dev == NULL || vid == NULL || pid == NULL) {
        return KS959_ERR_INVALID_ARG;
    }

    *vid = dev->vid;
    *pid = dev->pid;
    return KS959_OK;
}

int ks959_open(ks959_device_t **out_dev) {
    ks959_device_t *dev = NULL;
    wchar_t *device_path = NULL;
    int result;
    HANDLE device_handle = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE winusb_handle = NULL;

    if (out_dev == NULL) {
        return KS959_ERR_INVALID_ARG;
    }

    *out_dev = NULL;

    result = ks959_get_device_path(KS959_USB_VID, KS959_USB_PID, &device_path);
    if (result != KS959_OK) {
        return result;
    }

    dev = (ks959_device_t *)calloc(1, sizeof(*dev));
    if (dev == NULL) {
        free(device_path);
        return KS959_ERR_INTERNAL;
    }

    dev->device_handle = INVALID_HANDLE_VALUE;
    dev->winusb_handle = NULL;
    dev->vid = KS959_USB_VID;
    dev->pid = KS959_USB_PID;
    dev->last_os_error = 0;

    device_handle = CreateFileW(
        device_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL
    );

    free(device_path);
    device_path = NULL;

    if (device_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        dev->last_os_error = (uint32_t)err;
        free(dev);
        return ks959_map_open_error(err);
    }

    if (!WinUsb_Initialize(device_handle, &winusb_handle)) {
        ks959_set_last_error_from_system(dev);
        CloseHandle(device_handle);
        free(dev);
        return KS959_ERR_WINUSB_INIT_FAILED;
    }

    dev->device_handle = device_handle;
    dev->winusb_handle = winusb_handle;

    *out_dev = dev;
    return KS959_OK;
}

void ks959_close(ks959_device_t *dev) {
    if (dev == NULL) {
        return;
    }

    if (dev->winusb_handle != NULL) {
        WinUsb_Free(dev->winusb_handle);
        dev->winusb_handle = NULL;
    }

    if (dev->device_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->device_handle);
        dev->device_handle = INVALID_HANDLE_VALUE;
    }

    free(dev);
}

static int ks959_validate_setup(const ks959_setup_packet_t *setup) {
    if (setup == NULL) {
        return KS959_ERR_INVALID_ARG;
    }
    return KS959_OK;
}

int ks959_ctrl_write(ks959_device_t *dev,
                     const ks959_setup_packet_t *setup,
                     const uint8_t *buffer,
                     size_t buffer_len,
                     size_t *bytes_transferred) {
    WINUSB_SETUP_PACKET packet;
    ULONG transferred = 0;
    BOOL ok;

    if (bytes_transferred != NULL) {
        *bytes_transferred = 0;
    }

    if (dev == NULL || dev->winusb_handle == NULL) {
        return KS959_ERR_INVALID_ARG;
    }

    if (ks959_validate_setup(setup) != KS959_OK) {
        return KS959_ERR_INVALID_ARG;
    }

    if ((setup->request_type & 0x80u) != 0) {
        return KS959_ERR_INVALID_ARG;
    }

    if (setup->length != (uint16_t)buffer_len) {
        return KS959_ERR_INVALID_ARG;
    }

    if (buffer_len > 0 && buffer == NULL) {
        return KS959_ERR_INVALID_ARG;
    }

    ZeroMemory(&packet, sizeof(packet));
    packet.RequestType = setup->request_type;
    packet.Request = setup->request;
    packet.Value = setup->value;
    packet.Index = setup->index;
    packet.Length = setup->length;

    ok = WinUsb_ControlTransfer(
        dev->winusb_handle,
        packet,
        (PUCHAR)buffer,
        (ULONG)buffer_len,
        &transferred,
        NULL
    );

    if (!ok) {
        ks959_set_last_error_from_system(dev);
        return KS959_ERR_IO;
    }

    if (bytes_transferred != NULL) {
        *bytes_transferred = (size_t)transferred;
    }

    return KS959_OK;
}

int ks959_ctrl_read(ks959_device_t *dev,
                    const ks959_setup_packet_t *setup,
                    uint8_t *buffer,
                    size_t buffer_len,
                    size_t *bytes_transferred) {
    WINUSB_SETUP_PACKET packet;
    ULONG transferred = 0;
    BOOL ok;

    if (bytes_transferred != NULL) {
        *bytes_transferred = 0;
    }

    if (dev == NULL || dev->winusb_handle == NULL) {
        return KS959_ERR_INVALID_ARG;
    }

    if (ks959_validate_setup(setup) != KS959_OK) {
        return KS959_ERR_INVALID_ARG;
    }

    if ((setup->request_type & 0x80u) == 0) {
        return KS959_ERR_INVALID_ARG;
    }

    if (setup->length != (uint16_t)buffer_len) {
        return KS959_ERR_INVALID_ARG;
    }

    if (buffer_len > 0 && buffer == NULL) {
        return KS959_ERR_INVALID_ARG;
    }

    ZeroMemory(&packet, sizeof(packet));
    packet.RequestType = setup->request_type;
    packet.Request = setup->request;
    packet.Value = setup->value;
    packet.Index = setup->index;
    packet.Length = setup->length;

    ok = WinUsb_ControlTransfer(
        dev->winusb_handle,
        packet,
        buffer,
        (ULONG)buffer_len,
        &transferred,
        NULL
    );

    if (!ok) {
        ks959_set_last_error_from_system(dev);
        return KS959_ERR_IO;
    }

    if (bytes_transferred != NULL) {
        *bytes_transferred = (size_t)transferred;
    }

    return KS959_OK;
}

int ks959_last_os_error(ks959_device_t *dev, uint32_t *out_error_code) {
    if (dev == NULL || out_error_code == NULL) {
        return KS959_ERR_INVALID_ARG;
    }

    *out_error_code = dev->last_os_error;
    return KS959_OK;
}

/*
 * Protocol-layer functions not implemented yet.
 * They belong in future source files once the ks959-sir logic is mapped.
 */

int ks959_encode_tx_payload(const uint8_t *ir_frame,
                            size_t ir_frame_len,
                            uint8_t *usb_payload,
                            size_t *usb_payload_len) {
    (void)ir_frame;
    (void)ir_frame_len;
    (void)usb_payload;
    (void)usb_payload_len;
    return KS959_ERR_NOT_SUPPORTED;
}

int ks959_decode_rx_payload(const uint8_t *usb_payload,
                            size_t usb_payload_len,
                            uint8_t *ir_frame,
                            size_t *ir_frame_len) {
    (void)usb_payload;
    (void)usb_payload_len;
    (void)ir_frame;
    (void)ir_frame_len;
    return KS959_ERR_NOT_SUPPORTED;
}

int ks959_send_ir_frame(ks959_device_t *dev,
                        const uint8_t *ir_frame,
                        size_t ir_frame_len) {
    (void)dev;
    (void)ir_frame;
    (void)ir_frame_len;
    return KS959_ERR_NOT_SUPPORTED;
}

int ks959_receive_ir_frame(ks959_device_t *dev,
                           uint8_t *ir_frame,
                           size_t *ir_frame_len) {
    (void)dev;
    (void)ir_frame;
    (void)ir_frame_len;
    return KS959_ERR_NOT_SUPPORTED;
}

int ks959_set_baud_rate(ks959_device_t *dev, ks959_baud_rate_t baud) {
    (void)dev;
    (void)baud;
    return KS959_ERR_NOT_SUPPORTED;
}