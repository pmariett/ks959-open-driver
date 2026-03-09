#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usb.h>
#include <usbiodef.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

#define KS959_VID 0x07D0
#define KS959_PID 0x4959

typedef struct Ks959ProbeResult {
    BOOL foundDevice;
    BOOL openedDevice;
    BOOL initializedWinUsb;
    BOOL foundExpectedEndpoint;
    UCHAR expectedPipeId;
} Ks959ProbeResult;

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
    swprintf(pattern, 64, L"vid_%04x&pid_%04x", vid, pid);

    wchar_t lowerPath[2048];
    size_t i = 0;
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
            fprintf(stderr, "Out of memory while allocating detail buffer.\n");
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

    if (!found && GetLastError() != ERROR_NO_MORE_ITEMS) {
        /* benign in many enumeration loops, so don't fail loudly here */
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return found;
}

static BOOL probe_ks959(Ks959ProbeResult* result) {
    wchar_t* devicePath = NULL;
    HANDLE deviceHandle = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE winUsbHandle = NULL;
    USB_INTERFACE_DESCRIPTOR ifaceDesc;
    BOOL ok = FALSE;

    if (result == NULL) {
        return FALSE;
    }

    ZeroMemory(result, sizeof(*result));
    result->expectedPipeId = 0x81;

    printf("KS-959 probe starting...\n");
    printf("Searching for device interface for VID_%04X PID_%04X...\n", KS959_VID, KS959_PID);

    if (!get_ks959_device_path(&devicePath)) {
        fprintf(stderr, "KS-959 device not found.\n");
        return FALSE;
    }

    result->foundDevice = TRUE;

    wprintf(L"Device path: %ls\n", devicePath);

    deviceHandle = CreateFileW(
        devicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (deviceHandle == INVALID_HANDLE_VALUE) {
        print_last_error("CreateFileW");
        goto cleanup;
    }

    result->openedDevice = TRUE;
    printf("CreateFileW succeeded.\n");

    if (!WinUsb_Initialize(deviceHandle, &winUsbHandle)) {
        print_last_error("WinUsb_Initialize");
        goto cleanup;
    }

    result->initializedWinUsb = TRUE;
    printf("WinUSB initialized successfully.\n");

    ZeroMemory(&ifaceDesc, sizeof(ifaceDesc));
    if (!WinUsb_QueryInterfaceSettings(winUsbHandle, 0, &ifaceDesc)) {
        print_last_error("WinUsb_QueryInterfaceSettings");
        goto cleanup;
    }

    printf("Interface 0 information:\n");
    printf("  bInterfaceNumber   : %u\n", ifaceDesc.bInterfaceNumber);
    printf("  bAlternateSetting  : %u\n", ifaceDesc.bAlternateSetting);
    printf("  bNumEndpoints      : %u\n", ifaceDesc.bNumEndpoints);
    printf("  bInterfaceClass    : 0x%02X\n", ifaceDesc.bInterfaceClass);
    printf("  bInterfaceSubClass : 0x%02X\n", ifaceDesc.bInterfaceSubClass);
    printf("  bInterfaceProtocol : 0x%02X\n", ifaceDesc.bInterfaceProtocol);

    for (UCHAR i = 0; i < ifaceDesc.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipeInfo;
        ZeroMemory(&pipeInfo, sizeof(pipeInfo));

        if (!WinUsb_QueryPipe(winUsbHandle, 0, i, &pipeInfo)) {
            print_last_error("WinUsb_QueryPipe");
            goto cleanup;
        }

        printf("Pipe %u:\n", i);
        printf("  PipeType          : %s\n", pipe_type_to_string(pipeInfo.PipeType));
        printf("  PipeId            : 0x%02X\n", pipeInfo.PipeId);
        printf("  MaximumPacketSize : %u\n", pipeInfo.MaximumPacketSize);
        printf("  Interval          : %u\n", pipeInfo.Interval);

        if (pipeInfo.PipeId == result->expectedPipeId &&
            pipeInfo.PipeType == UsbdPipeTypeInterrupt) {
            result->foundExpectedEndpoint = TRUE;
        }
    }

    if (result->foundExpectedEndpoint) {
        printf("Found expected interrupt IN endpoint 0x%02X.\n", result->expectedPipeId);
    } else {
        printf("Expected interrupt IN endpoint 0x%02X was NOT found.\n", result->expectedPipeId);
    }

    ok = TRUE;

cleanup:
    if (winUsbHandle != NULL) {
        WinUsb_Free(winUsbHandle);
    }

    if (deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(deviceHandle);
    }

    if (devicePath != NULL) {
        free(devicePath);
    }

    return ok;
}

int main(void) {
    Ks959ProbeResult result;
    ZeroMemory(&result, sizeof(result));

    if (!probe_ks959(&result)) {
        fprintf(stderr, "Probe failed.\n");
        return 1;
    }

    printf("\nSummary:\n");
    printf("  Device found            : %s\n", result.foundDevice ? "yes" : "no");
    printf("  Device opened           : %s\n", result.openedDevice ? "yes" : "no");
    printf("  WinUSB initialized      : %s\n", result.initializedWinUsb ? "yes" : "no");
    printf("  Expected endpoint found : %s\n", result.foundExpectedEndpoint ? "yes" : "no");

    if (result.foundDevice &&
        result.openedDevice &&
        result.initializedWinUsb) {
        printf("Probe completed successfully.\n");
        return 0;
    }

    printf("Probe completed with errors.\n");
    return 2;
}