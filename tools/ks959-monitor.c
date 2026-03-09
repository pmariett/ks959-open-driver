#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usbiodef.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

#define KS959_VID 0x07D0
#define KS959_PID 0x4959
#define KS959_EXPECTED_PIPE_ID 0x81
#define KS959_READ_SIZE 8
#define KS959_READ_ITERATIONS 50

typedef struct Ks959Device {
    HANDLE deviceHandle;
    WINUSB_INTERFACE_HANDLE winUsbHandle;
    UCHAR interruptInPipe;
    USHORT maxPacketSize;
} Ks959Device;

static void print_last_error(const char* context) {
    DWORD err = GetLastError();
    LPSTR buffer = NULL;

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buffer,
        0,
        NULL
    );

    if (buffer != NULL) {
        fprintf(stderr, "%s failed. GetLastError() = %lu: %s\n", context, err, buffer);
        LocalFree(buffer);
    } else {
        fprintf(stderr, "%s failed. GetLastError() = %lu\n", context, err);
    }
}

static const char* pipe_type_to_string(USBD_PIPE_TYPE type) {
    switch (type) {
        case UsbdPipeTypeControl:     return "Control";
        case UsbdPipeTypeIsochronous: return "Isochronous";
        case UsbdPipeTypeBulk:        return "Bulk";
        case UsbdPipeTypeInterrupt:   return "Interrupt";
        default:                      return "Unknown";
    }
}

static BOOL path_contains_vid_pid(const wchar_t* devicePath, USHORT vid, USHORT pid) {
    wchar_t pattern[64];
    wchar_t lowerPath[2048];
    size_t i = 0;

    swprintf(pattern, 64, L"vid_%04x&pid_%04x", vid, pid);

    for (; devicePath[i] != L'\0' && i < (sizeof(lowerPath) / sizeof(lowerPath[0])) - 1; ++i) {
        wchar_t c = devicePath[i];
        if (c >= L'A' && c <= L'Z') {
            c = (wchar_t)(c - L'A' + L'a');
        }
        lowerPath[i] = c;
    }
    lowerPath[i] = L'\0';

    return wcsstr(lowerPath, pattern) != NULL;
}

static BOOL get_ks959_device_path(wchar_t** outDevicePath) {
    HDEVINFO deviceInfoSet = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    DWORD index = 0;
    BOOL found = FALSE;

    if (outDevicePath == NULL) {
        return FALSE;
    }

    *outDevicePath = NULL;

    deviceInfoSet = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_USB_DEVICE,
        NULL,
        NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        print_last_error("SetupDiGetClassDevsW");
        return FALSE;
    }

    ZeroMemory(&interfaceData, sizeof(interfaceData));
    interfaceData.cbSize = sizeof(interfaceData);

    while (SetupDiEnumDeviceInterfaces(
        deviceInfoSet,
        NULL,
        &GUID_DEVINTERFACE_USB_DEVICE,
        index,
        &interfaceData))
    {
        DWORD requiredSize = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData = NULL;

        SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet,
            &interfaceData,
            NULL,
            0,
            &requiredSize,
            NULL
        );

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || requiredSize == 0) {
            ++index;
            continue;
        }

        detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(requiredSize);
        if (detailData == NULL) {
            fprintf(stderr, "Out of memory while allocating interface detail buffer.\n");
            break;
        }

        ZeroMemory(detailData, requiredSize);
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(
                deviceInfoSet,
                &interfaceData,
                detailData,
                requiredSize,
                NULL,
                NULL))
        {
            free(detailData);
            ++index;
            continue;
        }

        if (path_contains_vid_pid(detailData->DevicePath, KS959_VID, KS959_PID)) {
            size_t chars = wcslen(detailData->DevicePath) + 1;
            *outDevicePath = (wchar_t*)malloc(chars * sizeof(wchar_t));
            if (*outDevicePath == NULL) {
                fprintf(stderr, "Out of memory while copying device path.\n");
                free(detailData);
                break;
            }

            wcscpy(*outDevicePath, detailData->DevicePath);
            found = TRUE;
            free(detailData);
            break;
        }

        free(detailData);
        ++index;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return found;
}

static void ks959_close(Ks959Device* dev) {
    if (dev == NULL) {
        return;
    }

    if (dev->winUsbHandle != NULL) {
        WinUsb_Free(dev->winUsbHandle);
        dev->winUsbHandle = NULL;
    }

    if (dev->deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->deviceHandle);
        dev->deviceHandle = INVALID_HANDLE_VALUE;
    }

    dev->interruptInPipe = 0;
    dev->maxPacketSize = 0;
}

static BOOL ks959_open(Ks959Device* dev) {
    wchar_t* devicePath = NULL;
    USB_INTERFACE_DESCRIPTOR ifaceDesc;
    BOOL ok = FALSE;

    if (dev == NULL) {
        return FALSE;
    }

    ZeroMemory(dev, sizeof(*dev));
    dev->deviceHandle = INVALID_HANDLE_VALUE;

    if (!get_ks959_device_path(&devicePath)) {
        fprintf(stderr, "KS-959 device not found.\n");
        return FALSE;
    }

    wprintf(L"Device path: %ls\n", devicePath);

    dev->deviceHandle = CreateFileW(
        devicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL
    );

    free(devicePath);
    devicePath = NULL;

    if (dev->deviceHandle == INVALID_HANDLE_VALUE) {
        print_last_error("CreateFileW");
        goto cleanup;
    }

    if (!WinUsb_Initialize(dev->deviceHandle, &dev->winUsbHandle)) {
        print_last_error("WinUsb_Initialize");
        goto cleanup;
    }

    ZeroMemory(&ifaceDesc, sizeof(ifaceDesc));
    if (!WinUsb_QueryInterfaceSettings(dev->winUsbHandle, 0, &ifaceDesc)) {
        print_last_error("WinUsb_QueryInterfaceSettings");
        goto cleanup;
    }

    printf("Interface 0:\n");
    printf("  bInterfaceNumber   : %u\n", ifaceDesc.bInterfaceNumber);
    printf("  bAlternateSetting  : %u\n", ifaceDesc.bAlternateSetting);
    printf("  bNumEndpoints      : %u\n", ifaceDesc.bNumEndpoints);
    printf("  bInterfaceClass    : 0x%02X\n", ifaceDesc.bInterfaceClass);
    printf("  bInterfaceSubClass : 0x%02X\n", ifaceDesc.bInterfaceSubClass);
    printf("  bInterfaceProtocol : 0x%02X\n", ifaceDesc.bInterfaceProtocol);

    for (UCHAR i = 0; i < ifaceDesc.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipeInfo;
        ZeroMemory(&pipeInfo, sizeof(pipeInfo));

        if (!WinUsb_QueryPipe(dev->winUsbHandle, 0, i, &pipeInfo)) {
            print_last_error("WinUsb_QueryPipe");
            goto cleanup;
        }

        printf("Pipe %u:\n", i);
        printf("  PipeType          : %s\n", pipe_type_to_string(pipeInfo.PipeType));
        printf("  PipeId            : 0x%02X\n", pipeInfo.PipeId);
        printf("  MaximumPacketSize : %u\n", pipeInfo.MaximumPacketSize);
        printf("  Interval          : %u\n", pipeInfo.Interval);

        if (pipeInfo.PipeId == KS959_EXPECTED_PIPE_ID &&
            pipeInfo.PipeType == UsbdPipeTypeInterrupt) {
            dev->interruptInPipe = pipeInfo.PipeId;
            dev->maxPacketSize = pipeInfo.MaximumPacketSize;
        }
    }

    if (dev->interruptInPipe == 0) {
        fprintf(stderr, "Expected interrupt IN pipe 0x%02X not found.\n", KS959_EXPECTED_PIPE_ID);
        goto cleanup;
    }

    printf("Using interrupt IN pipe 0x%02X, max packet size %u.\n",
           dev->interruptInPipe, dev->maxPacketSize);

    ok = TRUE;

cleanup:
    if (!ok) {
        ks959_close(dev);
    }

    return ok;
}

static void dump_hex(const UCHAR* data, ULONG length) {
    ULONG i;
    for (i = 0; i < length; ++i) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

static BOOL ks959_read_once(Ks959Device* dev, DWORD timeoutMs, DWORD iteration) {
    UCHAR buffer[KS959_READ_SIZE];
    OVERLAPPED ov;
    HANDLE eventHandle = NULL;
    ULONG bytesRead = 0;
    BOOL immediateResult;

    if (dev == NULL || dev->winUsbHandle == NULL || dev->interruptInPipe == 0) {
        return FALSE;
    }

    ZeroMemory(buffer, sizeof(buffer));
    ZeroMemory(&ov, sizeof(ov));

    eventHandle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (eventHandle == NULL) {
        print_last_error("CreateEventW");
        return FALSE;
    }

    ov.hEvent = eventHandle;

    immediateResult = WinUsb_ReadPipe(
        dev->winUsbHandle,
        dev->interruptInPipe,
        buffer,
        (ULONG)sizeof(buffer),
        NULL,
        &ov
    );

    if (!immediateResult) {
        DWORD err = GetLastError();

        if (err != ERROR_IO_PENDING) {
            fprintf(stderr, "[%lu] WinUsb_ReadPipe failed immediately.\n", iteration);
            print_last_error("WinUsb_ReadPipe");
            CloseHandle(eventHandle);
            return FALSE;
        }

        switch (WaitForSingleObject(eventHandle, timeoutMs)) {
            case WAIT_OBJECT_0:
                break;

            case WAIT_TIMEOUT:
                WinUsb_AbortPipe(dev->winUsbHandle, dev->interruptInPipe);
                printf("[%lu] timeout (%lu ms), no data.\n", iteration, timeoutMs);
                CloseHandle(eventHandle);
                return TRUE;

            default:
                print_last_error("WaitForSingleObject");
                CloseHandle(eventHandle);
                return FALSE;
        }

        if (!WinUsb_GetOverlappedResult(dev->winUsbHandle, &ov, &bytesRead, FALSE)) {
            DWORD ovErr = GetLastError();

            if (ovErr == ERROR_OPERATION_ABORTED) {
                printf("[%lu] read aborted after timeout.\n", iteration);
                CloseHandle(eventHandle);
                return TRUE;
            }

            fprintf(stderr, "[%lu] WinUsb_GetOverlappedResult failed.\n", iteration);
            print_last_error("WinUsb_GetOverlappedResult");
            CloseHandle(eventHandle);
            return FALSE;
        }
    } else {
        if (!WinUsb_GetOverlappedResult(dev->winUsbHandle, &ov, &bytesRead, FALSE)) {
            print_last_error("WinUsb_GetOverlappedResult");
            CloseHandle(eventHandle);
            return FALSE;
        }
    }

    if (bytesRead == 0) {
        printf("[%lu] zero-length packet.\n", iteration);
    } else {
        printf("[%lu] RX %lu byte(s): ", iteration, bytesRead);
        dump_hex(buffer, bytesRead);
    }

    CloseHandle(eventHandle);
    return TRUE;
}

int main(void) {
    Ks959Device dev;
    DWORD i;

    printf("KS-959 monitor starting...\n");

    if (!ks959_open(&dev)) {
        fprintf(stderr, "Unable to open KS-959 device.\n");
        return 1;
    }

    printf("Monitoring interrupt endpoint 0x%02X...\n", dev.interruptInPipe);
    printf("Press Ctrl+C to stop.\n");

    for (i = 0; i < KS959_READ_ITERATIONS; ++i) {
        if (!ks959_read_once(&dev, 500, i)) {
            fprintf(stderr, "Stopping monitor due to read error.\n");
            ks959_close(&dev);
            return 2;
        }
    }

    ks959_close(&dev);
    printf("Monitor completed.\n");
    return 0;
}