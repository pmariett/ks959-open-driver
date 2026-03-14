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
#define KS959_MAX_BUFFER 256
#define KS959_STATUS_LEN 4
#define KS959_MONITOR_READS 20
#define KS959_MONITOR_TIMEOUT_MS 500

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

static void dump_hex(const UCHAR* data, ULONG length) {
    ULONG i;
    for (i = 0; i < length; ++i) {
        printf("%02X ", data[i]);
    }
    printf("\n");
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

static BOOL ks959_control_transfer(
    Ks959Device* dev,
    UCHAR requestType,
    UCHAR request,
    USHORT value,
    USHORT index,
    const UCHAR* outBuffer,
    USHORT outLength,
    UCHAR* inBuffer,
    USHORT inBufferLength,
    ULONG* bytesTransferred)
{
    WINUSB_SETUP_PACKET setupPacket;
    BOOL ok;

    if (dev == NULL || dev->winUsbHandle == NULL) {
        return FALSE;
    }

    ZeroMemory(&setupPacket, sizeof(setupPacket));
    setupPacket.RequestType = requestType;
    setupPacket.Request = request;
    setupPacket.Value = value;
    setupPacket.Index = index;

    if ((requestType & 0x80) != 0) {
        setupPacket.Length = inBufferLength;
        ok = WinUsb_ControlTransfer(
            dev->winUsbHandle,
            setupPacket,
            inBuffer,
            inBufferLength,
            bytesTransferred,
            NULL
        );
    } else {
        setupPacket.Length = outLength;
        ok = WinUsb_ControlTransfer(
            dev->winUsbHandle,
            setupPacket,
            (PUCHAR)outBuffer,
            outLength,
            bytesTransferred,
            NULL
        );
    }

    if (!ok) {
        print_last_error("WinUsb_ControlTransfer");
        return FALSE;
    }

    return TRUE;
}

static BOOL ks959_get_status(Ks959Device* dev, UCHAR outStatus[KS959_STATUS_LEN]) {
    ULONG transferred = 0;

    if (!ks959_control_transfer(
            dev,
            0xA3,
            0x00,
            0x0000,
            0x0001,
            NULL,
            0,
            outStatus,
            KS959_STATUS_LEN,
            &transferred))
    {
        return FALSE;
    }

    if (transferred != KS959_STATUS_LEN) {
        fprintf(stderr, "Unexpected status length: %lu\n", transferred);
        return FALSE;
    }

    return TRUE;
}

static BOOL ks959_cmd_no_data(Ks959Device* dev, UCHAR request, USHORT value, USHORT index) {
    ULONG transferred = 0;

    if (!ks959_control_transfer(
            dev,
            0x23,
            request,
            value,
            index,
            NULL,
            0,
            NULL,
            0,
            &transferred))
    {
        return FALSE;
    }

    return TRUE;
}

static BOOL ks959_read_once(Ks959Device* dev, DWORD timeoutMs, DWORD iteration) {
    UCHAR buffer[KS959_MAX_BUFFER];
    ULONG bytesRead = 0;
    ULONG readSize;

    if (dev == NULL || dev->winUsbHandle == NULL || dev->interruptInPipe == 0) {
        return FALSE;
    }

    readSize = dev->maxPacketSize;
    if (readSize == 0 || readSize > KS959_MAX_BUFFER) {
        readSize = 8;
    }

    ZeroMemory(buffer, sizeof(buffer));

    if (!WinUsb_SetPipePolicy(
            dev->winUsbHandle,
            dev->interruptInPipe,
            PIPE_TRANSFER_TIMEOUT,
            sizeof(timeoutMs),
            &timeoutMs))
    {
        print_last_error("WinUsb_SetPipePolicy");
        return FALSE;
    }

    if (!WinUsb_ReadPipe(
            dev->winUsbHandle,
            dev->interruptInPipe,
            buffer,
            readSize,
            &bytesRead,
            NULL))
    {
        DWORD err = GetLastError();

        if (err == ERROR_SEM_TIMEOUT) {
            printf("[%lu] timeout (%lu ms), no data.\n", iteration, timeoutMs);
            return TRUE;
        }

        fprintf(stderr, "[%lu] WinUsb_ReadPipe failed.\n", iteration);
        print_last_error("WinUsb_ReadPipe");
        return FALSE;
    }

    if (bytesRead == 0) {
        printf("[%lu] zero-length packet.\n", iteration);
    } else {
        printf("[%lu] RX %lu byte(s): ", iteration, bytesRead);
        dump_hex(buffer, bytesRead);
    }

    return TRUE;
}

int main(void) {
    Ks959Device dev;
    UCHAR status[KS959_STATUS_LEN];
    int i;

    typedef struct {
        UCHAR requestType;
        UCHAR request;
        USHORT value;
        USHORT index;
        USHORT length;
        const char* label;
    } InProbe;

    typedef struct {
        UCHAR requestType;
        UCHAR request;
        USHORT value;
        USHORT index;
        const char* label;
    } OutProbe;

    const InProbe inProbes[] = {
        {0xA3, 0x00, 0x0000, 0x0001, 4, "GET status A3/idx1"},
        {0xA3, 0x00, 0x0000, 0x0000, 4, "GET status A3/idx0"},
        {0xA1, 0x00, 0x0000, 0x0001, 4, "GET status A1/idx1"},
        {0xA1, 0x00, 0x0000, 0x0000, 4, "GET status A1/idx0"},
        {0xC0, 0x00, 0x0000, 0x0001, 4, "GET status C0/idx1"},
        {0xC0, 0x00, 0x0000, 0x0000, 4, "GET status C0/idx0"},
    };

    const OutProbe outProbes[] = {
        {0x23, 0x01, 0x0010, 0x0001, "SET 23 01 0010 idx1"},
        {0x23, 0x01, 0x0010, 0x0000, "SET 23 01 0010 idx0"},
        {0x21, 0x01, 0x0010, 0x0001, "SET 21 01 0010 idx1"},
        {0x21, 0x01, 0x0010, 0x0000, "SET 21 01 0010 idx0"},

        {0x23, 0x01, 0x0011, 0x0001, "SET 23 01 0011 idx1"},
        {0x23, 0x01, 0x0011, 0x0000, "SET 23 01 0011 idx0"},
        {0x21, 0x01, 0x0011, 0x0001, "SET 21 01 0011 idx1"},
        {0x21, 0x01, 0x0011, 0x0000, "SET 21 01 0011 idx0"},

        {0x23, 0x03, 0x0008, 0x0001, "SET 23 03 0008 idx1"},
        {0x23, 0x03, 0x0008, 0x0000, "SET 23 03 0008 idx0"},
        {0x21, 0x03, 0x0008, 0x0001, "SET 21 03 0008 idx1"},
        {0x21, 0x03, 0x0008, 0x0000, "SET 21 03 0008 idx0"},
    };

    ULONG transferred = 0;

    printf("KS-959 setup probe starting...\n");

    if (!ks959_open(&dev)) {
        fprintf(stderr, "Unable to open KS-959 device.\n");
        return 1;
    }

    printf("\n=== IN probes ===\n");
    for (i = 0; i < (int)(sizeof(inProbes) / sizeof(inProbes[0])); ++i) {
        ZeroMemory(status, sizeof(status));
        transferred = 0;

        printf("\n[%d] %s\n", i, inProbes[i].label);

        if (ks959_control_transfer(
                &dev,
                inProbes[i].requestType,
                inProbes[i].request,
                inProbes[i].value,
                inProbes[i].index,
                NULL,
                0,
                status,
                inProbes[i].length,
                &transferred))
        {
            printf("  OK, %lu byte(s): ", transferred);
            dump_hex(status, transferred);
        } else {
            printf("  FAILED\n");
        }

        Sleep(50);
    }

    printf("\n=== OUT probes ===\n");
    for (i = 0; i < (int)(sizeof(outProbes) / sizeof(outProbes[0])); ++i) {
        transferred = 0;

        printf("\n[%d] %s\n", i, outProbes[i].label);

        if (ks959_control_transfer(
                &dev,
                outProbes[i].requestType,
                outProbes[i].request,
                outProbes[i].value,
                outProbes[i].index,
                NULL,
                0,
                NULL,
                0,
                &transferred))
        {
            printf("  OK, transferred=%lu\n", transferred);
        } else {
            printf("  FAILED\n");
        }

        Sleep(50);
    }

    printf("\n=== Interrupt listen ===\n");
    for (i = 0; i < 10; ++i) {
        if (!ks959_read_once(&dev, 500, (DWORD)i)) {
            printf("Interrupt read failed.\n");
            break;
        }
    }

    ks959_close(&dev);
    printf("\nProbe completed.\n");
    return 0;
}