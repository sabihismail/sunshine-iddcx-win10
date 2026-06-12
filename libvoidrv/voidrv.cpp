#include "voidrv.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <new>

// Allocate GUID_DEVINTERFACE_VOIDDISPLAY and pull in the driver wire format.
#include <initguid.h>
#include "Public.h"   // from ..\void-display (added to the include path)

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

struct VoidrvDisplay {
    HANDLE Device;
};

// Enumerate the device interface and open the first instance.
static HANDLE OpenInterface(const GUID* interfaceGuid)
{
    HANDLE handle = INVALID_HANDLE_VALUE;

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        interfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    SP_DEVICE_INTERFACE_DATA ifData;
    ZeroMemory(&ifData, sizeof(ifData));
    ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, interfaceGuid, i, &ifData); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &needed, nullptr);
        if (needed == 0) {
            continue;
        }

        auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(malloc(needed));
        if (!detail) {
            continue;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, &needed, nullptr)) {
            handle = CreateFileW(detail->DevicePath,
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        free(detail);

        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return handle;
}

static bool Control(HANDLE device, DWORD code,
                    void* in, DWORD inLen, void* out, DWORD outLen, DWORD* bytes)
{
    DWORD br = 0;
    BOOL ok = DeviceIoControl(device, code, in, inLen, out, outLen, &br, nullptr);
    if (bytes) {
        *bytes = br;
    }
    return ok != FALSE;
}

extern "C" {

VoidrvStatus VoidrvDisplayQueryStatus(void)
{
    HANDLE h = OpenInterface(&GUID_DEVINTERFACE_VOIDDISPLAY);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        // Distinguish "no interface" is not trivial here; report not-installed.
        return VOIDRV_STATUS_NOT_INSTALLED;
    }
    CloseHandle(h);
    return VOIDRV_STATUS_OK;
}

VoidrvDisplayHandle VoidrvDisplayOpen(void)
{
    HANDLE h = OpenInterface(&GUID_DEVINTERFACE_VOIDDISPLAY);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        return nullptr;
    }
    auto* d = new (std::nothrow) VoidrvDisplay();
    if (!d) {
        CloseHandle(h);
        return nullptr;
    }
    d->Device = h;
    return d;
}

void VoidrvDisplayClose(VoidrvDisplayHandle handle)
{
    if (!handle) {
        return;
    }
    if (handle->Device && handle->Device != INVALID_HANDLE_VALUE) {
        CloseHandle(handle->Device);
    }
    delete handle;
}

uint32_t VoidrvDisplayVersion(VoidrvDisplayHandle handle)
{
    if (!handle) {
        return 0;
    }
    ULONG version = 0;
    if (!Control(handle->Device, IOCTL_VOIDDISPLAY_VERSION, nullptr, 0,
                 &version, sizeof(version), nullptr)) {
        return 0;
    }
    return version;
}

int VoidrvDisplayAdd(VoidrvDisplayHandle handle, const VoidrvDisplayMode* mode)
{
    if (!handle) {
        return -1;
    }
    VOIDDISPLAY_MODE wire;
    ZeroMemory(&wire, sizeof(wire));
    if (mode) {
        wire.Width = mode->Width;
        wire.Height = mode->Height;
        wire.RefreshHz = mode->RefreshHz;
    }
    ULONG index = 0;
    if (!Control(handle->Device, IOCTL_VOIDDISPLAY_ADD, &wire, sizeof(wire),
                 &index, sizeof(index), nullptr)) {
        return -1;
    }
    return (int)index;
}

bool VoidrvDisplayRemove(VoidrvDisplayHandle handle, uint32_t index)
{
    if (!handle) {
        return false;
    }
    ULONG i = index;
    return Control(handle->Device, IOCTL_VOIDDISPLAY_REMOVE, &i, sizeof(i), nullptr, 0, nullptr);
}

bool VoidrvDisplaySetMode(VoidrvDisplayHandle handle, uint32_t index, const VoidrvDisplayMode* mode)
{
    if (!handle || !mode) {
        return false;
    }
    VOIDDISPLAY_SET_MODE wire;
    ZeroMemory(&wire, sizeof(wire));
    wire.Index = index;
    wire.Mode.Width = mode->Width;
    wire.Mode.Height = mode->Height;
    wire.Mode.RefreshHz = mode->RefreshHz;
    return Control(handle->Device, IOCTL_VOIDDISPLAY_SET_MODE, &wire, sizeof(wire), nullptr, 0, nullptr);
}

bool VoidrvDisplayList(VoidrvDisplayHandle handle, VoidrvDisplayState* state)
{
    if (!handle || !state) {
        return false;
    }
    VOIDDISPLAY_STATE wire;
    ZeroMemory(&wire, sizeof(wire));
    if (!Control(handle->Device, IOCTL_VOIDDISPLAY_LIST, nullptr, 0,
                 &wire, sizeof(wire), nullptr)) {
        return false;
    }

    state->Count = wire.Count;
    for (uint32_t i = 0; i < VOIDRV_MAX_DISPLAYS && i < VOIDDISPLAY_MAX_DISPLAYS; ++i) {
        state->Entries[i].InUse = wire.Entries[i].InUse;
        state->Entries[i].Mode.Width = wire.Entries[i].Mode.Width;
        state->Entries[i].Mode.Height = wire.Entries[i].Mode.Height;
        state->Entries[i].Mode.RefreshHz = wire.Entries[i].Mode.RefreshHz;
    }
    return true;
}

} // extern "C"
