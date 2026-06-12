#include "voidrv.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <new>
#include <wchar.h>

// Allocate GUID_DEVINTERFACE_VOIDDISPLAY and pull in the driver wire format.
#include <initguid.h>
#include "Public.h"   // from ..\void-display (added to the include path)

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

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

static bool VoidApplyMode(uint32_t index, const VoidrvDisplayMode* mode);

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

    // If the display auto-activates, force the requested mode over whatever the OS
    // remembered for this monitor identity. Best-effort: poll briefly for it to go
    // active (it does nothing if the display stays inactive).
    if (mode && mode->Width && mode->Height && mode->RefreshHz) {
        for (int tries = 0; tries < 10; ++tries) {
            if (VoidApplyMode((uint32_t)index, mode)) {
                break;
            }
            Sleep(150);
        }
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

// Apply a mode to the index-th active VoidDisplay monitor via the GDI display
// config. Windows remembers the per-monitor mode in the registry, so the driver's
// advertised mode is not enough on a re-add - we force the requested mode here.
// Identifies VoidDisplay GDI devices by the "VVD" EDID id on their monitor child.
// (Index is matched against active VoidDisplay devices in enumeration order; exact
//  for the common single-display case.)
static bool VoidApplyMode(uint32_t index, const VoidrvDisplayMode* mode)
{
    WCHAR devices[VOIDRV_MAX_DISPLAYS][32];
    int count = 0;

    DISPLAY_DEVICEW dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    for (DWORD i = 0; count < VOIDRV_MAX_DISPLAYS && EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            DISPLAY_DEVICEW mon;
            ZeroMemory(&mon, sizeof(mon));
            mon.cb = sizeof(mon);
            if (EnumDisplayDevicesW(dd.DeviceName, 0, &mon, 0) && wcsstr(mon.DeviceID, L"VVD")) {
                lstrcpynW(devices[count], dd.DeviceName, 32);
                ++count;
            }
        }
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
    }

    if (index >= (uint32_t)count) {
        return false;  // that VoidDisplay isn't active (no GDI device to drive)
    }

    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(devices[index], ENUM_CURRENT_SETTINGS, &dm)) {
        return false;
    }
    dm.dmPelsWidth        = mode->Width;
    dm.dmPelsHeight       = mode->Height;
    dm.dmDisplayFrequency = mode->RefreshHz;
    dm.dmFields           = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    LONG rc = ChangeDisplaySettingsExW(devices[index], &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    return rc == DISP_CHANGE_SUCCESSFUL;
}

bool VoidrvDisplaySetMode(VoidrvDisplayHandle handle, uint32_t index, const VoidrvDisplayMode* mode)
{
    if (!handle || !mode) {
        return false;
    }
    // Update the driver's stored mode (keeps LIST accurate)...
    VOIDDISPLAY_SET_MODE wire;
    ZeroMemory(&wire, sizeof(wire));
    wire.Index = index;
    wire.Mode.Width = mode->Width;
    wire.Mode.Height = mode->Height;
    wire.Mode.RefreshHz = mode->RefreshHz;
    Control(handle->Device, IOCTL_VOIDDISPLAY_SET_MODE, &wire, sizeof(wire), nullptr, 0, nullptr);

    // ...then apply the actual OS-level resolution change.
    return VoidApplyMode(index, mode);
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

// The driver re-reads its custom-mode list from this key at adapter init, so the
// IOCTL only changes the live list - we mirror the change here for persistence.
static const wchar_t kVoidParamsKey[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\VoidDisplay\\Parameters";

// Mirror a custom-mode add/remove into the driver's Parameters key so it survives
// a device restart / reboot. Stored as a REG_BINARY array of packed
// VoidrvDisplayMode triples. Best-effort: writing HKLM needs elevation, and a
// failure here does not undo the live (IOCTL) change.
static void PersistCustomMode(const VoidrvDisplayMode* mode, bool add)
{
    VoidrvDisplayMode list[VOIDRV_MAX_MODES];
    DWORD cb = sizeof(list);
    DWORD count = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kVoidParamsKey, L"CustomModes",
                     RRF_RT_REG_BINARY, nullptr, list, &cb) == ERROR_SUCCESS) {
        count = cb / (DWORD)sizeof(VoidrvDisplayMode);
    }

    DWORD found = count;
    for (DWORD i = 0; i < count; ++i) {
        if (list[i].Width == mode->Width && list[i].Height == mode->Height &&
            list[i].RefreshHz == mode->RefreshHz) {
            found = i;
            break;
        }
    }

    if (add) {
        if (found != count || count >= VOIDRV_MAX_MODES) {
            return;  // already present, or full
        }
        list[count++] = *mode;
    } else {
        if (found == count) {
            return;  // not present
        }
        for (DWORD i = found + 1; i < count; ++i) {
            list[i - 1] = list[i];
        }
        --count;
    }

    RegSetKeyValueW(HKEY_LOCAL_MACHINE, kVoidParamsKey, L"CustomModes",
                    REG_BINARY, list, count * (DWORD)sizeof(VoidrvDisplayMode));
}

bool VoidrvDisplayAddMode(VoidrvDisplayHandle handle, const VoidrvDisplayMode* mode)
{
    if (!handle || !mode) {
        return false;
    }
    VOIDDISPLAY_MODE wire;
    ZeroMemory(&wire, sizeof(wire));
    wire.Width = mode->Width;
    wire.Height = mode->Height;
    wire.RefreshHz = mode->RefreshHz;
    if (!Control(handle->Device, IOCTL_VOIDDISPLAY_ADD_MODE, &wire, sizeof(wire), nullptr, 0, nullptr)) {
        return false;
    }
    PersistCustomMode(mode, true);
    return true;
}

bool VoidrvDisplayRemoveMode(VoidrvDisplayHandle handle, const VoidrvDisplayMode* mode)
{
    if (!handle || !mode) {
        return false;
    }
    VOIDDISPLAY_MODE wire;
    ZeroMemory(&wire, sizeof(wire));
    wire.Width = mode->Width;
    wire.Height = mode->Height;
    wire.RefreshHz = mode->RefreshHz;
    if (!Control(handle->Device, IOCTL_VOIDDISPLAY_REMOVE_MODE, &wire, sizeof(wire), nullptr, 0, nullptr)) {
        return false;
    }
    PersistCustomMode(mode, false);
    return true;
}

bool VoidrvDisplayListModes(VoidrvDisplayHandle handle, VoidrvModeList* list)
{
    if (!handle || !list) {
        return false;
    }
    VOIDDISPLAY_MODE_LIST wire;
    ZeroMemory(&wire, sizeof(wire));
    if (!Control(handle->Device, IOCTL_VOIDDISPLAY_LIST_MODES, nullptr, 0,
                 &wire, sizeof(wire), nullptr)) {
        return false;
    }

    list->Count = wire.Count;
    for (uint32_t i = 0; i < VOIDRV_MAX_MODES && i < VOIDDISPLAY_MAX_MODES; ++i) {
        list->Modes[i].Width = wire.Modes[i].Width;
        list->Modes[i].Height = wire.Modes[i].Height;
        list->Modes[i].RefreshHz = wire.Modes[i].RefreshHz;
    }
    return true;
}

} // extern "C"
