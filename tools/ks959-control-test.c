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
#define KS959_DEFAULT_TIMEOUT_MS 500
#define KS959_DEFAULT_READ_ITERATIONS 10
#define KS959_MAX_BUFFER 256

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

static BOOL parse_u8(const char* s, UCHAR* out) {
    unsigned long value;
    char* end = NULL;

    if (s == NULL || out == NULL) {
        return FALSE;
    }

    value = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || value > 0xFFUL) {
        return FALSE;
    }

    *out = (UCHAR)value;
    return TRUE;
}

static BOOL parse_u16(const char* s, USHORT* out) {
    unsigned long value;
    char* end = NULL;

    if (s == NULL || out == NULL) {
        return FALSE;
    }

    value = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || value > 0xFFFFUL) {
        return FALSE;
    }

    *out = (USHORT)value;
    return TRUE;
}

static BOOL parse_hex_bytes(const char* s, UCHAR* buffer, USHORT* inOutLength) {
    char* copy = NULL;
    char* token = NULL;
    char* context = NULL;
    USHORT count = 0;

    if (s == NULL || buffer == NULL || inOutLength == NULL) {
        return FALSE;
    }

    copy = _strdup(s);
    if (copy == NULL) {
        return FALSE;
    }

    token = strtok_s(copy, ",", &context);
    while (token != NULL) {
        unsigned long value;
        char* end = NULL;

        if (count >= *inOutLength) {
            free(copy);
            return FALSE;
        }

        value = strtoul(token, &end, 16);
        if (end == token || *end != '\0' || value > 0xFFUL) {
            free(copy);
            return FALSE;
        }

        buffer[count++] = (UCHAR)value;
        token = strtok_s(NULL, ",", &context);
    }

    *inOutLength = count;
    free(copy);
    return TRUE;
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

static BOOL ks959_read_once(Ks959Device* dev, DWORD timeoutMs, DWORD iteration) {
    UCHAR buffer[KS959_MAX_BUFFER];
    OVERLAPPED ov;
    HANDLE eventHandle = NULL;
    ULONG bytesRead = 0;
    BOOL immediateResult;
    ULONG readSize;

    if (dev == NULL || dev->winUsbHandle == NULL || dev->interruptInPipe == 0) {
        return FALSE;
    }

    readSize = dev->maxPacketSize;
    if (readSize == 0 || readSize > KS959_MAX_BUFFER) {
        readSize = 8;
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
        readSize,
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

static void print_usage(const char* exeName) {
    printf("Usage:\n");
    printf("  %s <RequestType> <Request> <Value> <Index> [data-bytes]\n", exeName);
    printf("\n");
    printf("Examples:\n");
    printf("  %s 0x40 0x01 0 0\n", exeName);
    printf("  %s 0x40 0x02 0 0 01,02,03,04\n", exeName);
    printf("  %s 0xC0 0x01 0 0\n", exeName);
    printf("\n");
    printf("Notes:\n");
    printf("  - RequestType 0x40 = vendor, host->device\n");
    printf("  - RequestType 0xC0 = vendor, device->host\n");
    printf("  - data-bytes are hex bytes separated by commas, for example: 01,AA,FF\n");
}

int main(int argc, char** argv) {
    Ks959Device dev;
    UCHAR requestType = 0;
    UCHAR request = 0;
    USHORT value = 0;
    USHORT index = 0;
    UCHAR outBuffer[KS959_MAX_BUFFER];
    UCHAR inBuffer[KS959_MAX_BUFFER];
    USHORT outLength = 0;
    ULONG transferred = 0;
    DWORD i;

    ZeroMemory(&dev, sizeof(dev));
    ZeroMemory(outBuffer, sizeof(outBuffer));
    ZeroMemory(inBuffer, sizeof(inBuffer));

    if (argc < 5) {
        print_usage(argv[0]);
        return 1;
    }

    if (!parse_u8(argv[1], &requestType) ||
        !parse_u8(argv[2], &request) ||
        !parse_u16(argv[3], &value) ||
        !parse_u16(argv[4], &index)) {
        fprintf(stderr, "Invalid numeric argument.\n");
        print_usage(argv[0]);
        return 1;
    }

    if ((requestType & 0x80) == 0) {
        if (argc >= 6) {
            outLength = (USHORT)sizeof(outBuffer);
            if (!parse_hex_bytes(argv[5], outBuffer, &outLength)) {
                fprintf(stderr, "Invalid data-bytes argument.\n");
                return 1;
            }
        }
    }

    printf("KS-959 control test starting...\n");

    if (!ks959_open(&dev)) {
        fprintf(stderr, "Unable to open KS-959 device.\n");
        return 2;
    }

    printf("Sending control transfer:\n");
    printf("  RequestType : 0x%02X\n", requestType);
    printf("  Request     : 0x%02X\n", request);
    printf("  Value       : 0x%04X\n", value);
    printf("  Index       : 0x%04X\n", index);

    if ((requestType & 0x80) == 0) {
        printf("  Direction   : Host -> Device\n");
        printf("  Payload len : %u\n", outLength);
        if (outLength > 0) {
            printf("  Payload     : ");
            dump_hex(outBuffer, outLength);
        }

        if (!ks959_control_transfer(
                &dev,
                requestType,
                request,
                value,
                index,
                outBuffer,
                outLength,
                NULL,
                0,
                &transferred))
        {
            ks959_close(&dev);
            return 3;
        }

        printf("Control transfer OK, %lu byte(s) transferred.\n", transferred);
    } else {
        printf("  Direction   : Device -> Host\n");

        if (!ks959_control_transfer(
                &dev,
                requestType,
                request,
                value,
                index,
                NULL,
                0,
                inBuffer,
                64,
                &transferred))
        {
            ks959_close(&dev);
            return 3;
        }

        printf("Control transfer OK, %lu byte(s) received.\n", transferred);
        if (transferred > 0) {
            printf("Response      : ");
            dump_hex(inBuffer, transferred);
        }
    }

    printf("Listening on interrupt endpoint 0x%02X...\n", dev.interruptInPipe);
    for (i = 0; i < KS959_DEFAULT_READ_ITERATIONS; ++i) {
        if (!ks959_read_once(&dev, KS959_DEFAULT_TIMEOUT_MS, i)) {
            fprintf(stderr, "Stopping due to read error.\n");
            ks959_close(&dev);
            return 4;
        }
    }

    ks959_close(&dev);
    printf("Control test completed.\n");
    return 0;
}