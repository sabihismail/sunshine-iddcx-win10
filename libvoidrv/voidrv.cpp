#include "voidrv.h"

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <new>
#include <wchar.h>
#include <stdio.h>

// Both control-interface headers are self-contained and each #include <winioctl.h>,
// whose device-interface GUIDs are emitted on EVERY include (outside its include
// guard). Pulling the headers here - before INITGUID - leaves all of those GUIDs as
// plain extern declarations. We then allocate storage for ONLY the two interface
// GUIDs we actually use. (Including either Public.h after INITGUID would re-emit
// winioctl's GUIDs with initializers and allocate them twice -> C2374.) Both files
// are named Public.h, so void-input is included by explicit relative path.
#include "Public.h"                  // void-display control interface
#include "../void-input/Public.h"    // void-input control interface

#include <initguid.h>
DEFINE_GUID(GUID_DEVINTERFACE_VOIDDISPLAY,
    0x40255101, 0xa910, 0x441c, 0x84, 0xd6, 0x9f, 0x02, 0x71, 0x97, 0xfa, 0x70);
DEFINE_GUID(GUID_DEVINTERFACE_VOIDINPUT,
    0x7b0c8d49, 0x54fc, 0x45ca, 0xab, 0x47, 0x6a, 0x57, 0x2f, 0x2f, 0xf5, 0x10);

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

struct VoidrvDisplay {
    HANDLE Device;
};

// Enumerate the device interface and open the first instance. VoidInput opens
// overlapped (overlapped=true) so its background event reader and input writes do
// not serialize on the one file handle; VoidDisplay opens synchronous.
static HANDLE OpenInterface(const GUID* interfaceGuid, bool overlapped = false)
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
                                 nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0),
                                 nullptr);
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

// Overlapped-handle variants for VoidInput (its handle is opened FILE_FLAG_OVERLAPPED).
// Issue the I/O and wait for completion, so the device handle is never serialized.
static bool InputControl(HANDLE device, DWORD code,
                         void* in, DWORD inLen, void* out, DWORD outLen, DWORD* bytes)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        return false;
    }
    DWORD br = 0;
    BOOL ok = DeviceIoControl(device, code, in, inLen, out, outLen, &br, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(device, &ov, &br, TRUE);
    }
    CloseHandle(ov.hEvent);
    if (bytes) {
        *bytes = br;
    }
    return ok != FALSE;
}

static bool InputWrite(HANDLE device, const void* buffer, DWORD length)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        return false;
    }
    DWORD bw = 0;
    BOOL ok = WriteFile(device, buffer, length, &bw, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(device, &ov, &bw, TRUE);
    }
    CloseHandle(ov.hEvent);
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
static void PersistDisplay(uint32_t index, const VoidrvDisplayMode* mode, bool present);

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

    // Persist for restore-on-start. Record the effective mode (the driver's
    // default when the request was zeroed).
    VoidrvDisplayMode eff = { 1920, 1080, 60 };
    if (mode && mode->Width && mode->Height && mode->RefreshHz) {
        eff = *mode;
    }
    PersistDisplay((uint32_t)index, &eff, true);
    return (int)index;
}

bool VoidrvDisplayRemove(VoidrvDisplayHandle handle, uint32_t index)
{
    if (!handle) {
        return false;
    }
    ULONG i = index;
    bool ok = Control(handle->Device, IOCTL_VOIDDISPLAY_REMOVE, &i, sizeof(i), nullptr, 0, nullptr);
    if (ok) {
        PersistDisplay(index, nullptr, false);
    }
    return ok;
}

// Apply a mode to the index-th active VoidDisplay monitor via the GDI display
// config. Windows remembers the per-monitor mode in the registry, so the driver's
// advertised mode is not enough on a re-add - we force the requested mode here.
// Identifies VoidDisplay GDI devices by the ADAPTER name ("Void Virtual Display
// Adapter"), NOT the EDID manufacturer - a custom EDID (display.edid) changes the
// monitor id but not the adapter, so adapter matching stays correct.
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
        if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
            wcsstr(dd.DeviceString, L"Void Virtual Display")) {
            lstrcpynW(devices[count], dd.DeviceName, 32);
            ++count;
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
    bool applied = VoidApplyMode(index, mode);

    // Persist the new mode so restore-on-start brings it back.
    PersistDisplay(index, mode, true);
    return applied;
}

bool VoidrvDisplaySetModeDynamic(VoidrvDisplayHandle handle, uint32_t index,
                                 const VoidrvDisplayMode* mode)
{
    if (!handle || !mode) {
        return false;
    }

    // Round to even (hardware encoders and the OS prefer it) and clamp to a sane
    // range: <= 4096x2160, 24..165 Hz.
    VoidrvDisplayMode m = *mode;
    m.Width  &= ~1u;
    m.Height &= ~1u;
    if (m.Width  < 320u)   m.Width  = 320u;
    if (m.Height < 240u)   m.Height = 240u;
    if (m.Width  > 4096u)  m.Width  = 4096u;
    if (m.Height > 2160u)  m.Height = 2160u;
    if (m.RefreshHz < 24u)  m.RefreshHz = 24u;
    if (m.RefreshHz > 165u) m.RefreshHz = 165u;

    // Advertise + refresh the live monitor so the OS accepts the mode...
    VOIDDISPLAY_SET_MODE wire;
    ZeroMemory(&wire, sizeof(wire));
    wire.Index = index;
    wire.Mode.Width = m.Width;
    wire.Mode.Height = m.Height;
    wire.Mode.RefreshHz = m.RefreshHz;
    if (!Control(handle->Device, IOCTL_VOIDDISPLAY_SET_MODE_DYNAMIC, &wire, sizeof(wire),
                 nullptr, 0, nullptr)) {
        return false;
    }

    // ...then switch the OS resolution. If the driver re-plugged the monitor to expose
    // a new mode, the re-arrived display takes a moment to enumerate, so retry briefly.
    // Ephemeral: not persisted on purpose.
    for (int tries = 0; tries < 12; ++tries) {
        if (VoidApplyMode(index, &m)) {
            return true;
        }
        Sleep(150);
    }
    return false;
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

// VoidDisplay config file. The driver reads it at adapter init; the SDK writes it so
// an unelevated controller can persist changes (the installer grants Users:Modify on
// the folder). Best-effort: a write failure does not undo the live (IOCTL) change.
static const wchar_t kVoidIniPath[] = L"C:\\ProgramData\\.voidrv\\display.ini";

// Mirror a custom-mode add/remove into [modes]Custom (comma-separated WxH@Hz) so it
// survives a device restart / reboot.
static void PersistCustomMode(const VoidrvDisplayMode* mode, bool add)
{
    wchar_t cur[1024] = {};
    GetPrivateProfileStringW(L"modes", L"Custom", L"", cur, ARRAYSIZE(cur), kVoidIniPath);

    // Rebuild the list, dropping any existing copy of this mode (so add re-appends at
    // the end, and remove simply omits it).
    VoidrvDisplayMode list[VOIDRV_MAX_MODES];
    DWORD count = 0;
    wchar_t* ctx = nullptr;
    for (wchar_t* tok = wcstok_s(cur, L",;", &ctx); tok && count < VOIDRV_MAX_MODES;
         tok = wcstok_s(nullptr, L",;", &ctx)) {
        while (*tok == L' ') {
            ++tok;
        }
        unsigned w = 0, h = 0, hz = 0;
        if (swscanf_s(tok, L"%ux%u@%u", &w, &h, &hz) >= 2 && w && h) {
            if (hz == 0) hz = 60;
            if (w == mode->Width && h == mode->Height && hz == mode->RefreshHz) {
                continue;  // drop the existing copy
            }
            list[count].Width = w; list[count].Height = h; list[count].RefreshHz = hz;
            ++count;
        }
    }
    if (add && count < VOIDRV_MAX_MODES) {
        list[count++] = *mode;
    }

    wchar_t out[1024] = {};
    for (DWORD i = 0; i < count; ++i) {
        wchar_t one[40];
        swprintf_s(one, ARRAYSIZE(one), L"%ux%u@%u",
                   list[i].Width, list[i].Height, list[i].RefreshHz);
        if (i) {
            wcscat_s(out, ARRAYSIZE(out), L",");
        }
        wcscat_s(out, ARRAYSIZE(out), one);
    }
    WritePrivateProfileStringW(L"modes", L"Custom", count ? out : nullptr, kVoidIniPath);
}

// Mirror a display add / setmode / remove into [displays] (<slot> = WxH@Hz) so the
// driver recreates it at init (RestoreOnStart).
static void PersistDisplay(uint32_t index, const VoidrvDisplayMode* mode, bool present)
{
    wchar_t key[16];
    swprintf_s(key, ARRAYSIZE(key), L"%u", index);
    if (present) {
        wchar_t val[40];
        swprintf_s(val, ARRAYSIZE(val), L"%ux%u@%u", mode->Width, mode->Height, mode->RefreshHz);
        WritePrivateProfileStringW(L"displays", key, val, kVoidIniPath);
    } else {
        WritePrivateProfileStringW(L"displays", key, nullptr, kVoidIniPath);  // delete the key
    }
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

bool VoidrvDisplayPersistenceWritable(void)
{
    // Can we write the config file? Opening it for write (no side effects -
    // OPEN_EXISTING does not modify) succeeds only when the folder ACL grants this
    // (possibly unelevated) caller write access.
    HANDLE h = CreateFileW(kVoidIniPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
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

// ===================== VoidInput =====================================

// Report ids - match the VoidInput HID descriptors (void-input/Devices.cpp).
#define VOIDRV_MOUSE_RID_RELATIVE 1
#define VOIDRV_MOUSE_RID_ABSOLUTE 2
#define VOIDRV_KBD_RID_KEYBOARD   1
#define VOIDRV_KBD_RID_CONSUMER   2
#define VOIDRV_TOUCH_RID_TOUCH    1
#define VOIDRV_TOUCH_RID_MAXCOUNT 2
#define VOIDRV_TOUCH_RID_PEN      3
#define VOIDRV_TOUCH_MAX_CONTACTS 10

struct VoidrvInput {
    HANDLE          Device;
    VoidrvInputType Type;

    // Stateful-tier mouse state.
    uint8_t  MouseButtons;          // held VOIDRV_MB_* mask
    int32_t  BoundsLeft, BoundsTop; // absolute-pixel mapping rect; W<=0 => primary display
    int32_t  BoundsW, BoundsH;

    // Stateful-tier keyboard state.
    uint8_t  KbdModifiers;          // held VOIDRV_KMOD_* mask
    uint8_t  KbdKeys[6];            // held HID key usages (0 = empty)

    // Touch digitizer state: the active-contact prefix (Windows requires the live
    // contacts to be contiguous). TouchCount slots in [0,TouchCount) are valid.
    struct { uint8_t Id; uint8_t Tip; uint16_t X, Y; } TouchSlots[VOIDRV_TOUCH_MAX_CONTACTS];
    uint8_t  TouchCount;
    uint16_t TouchScanTime;
    // Pen state. PenFlags: bit0 tip, bit1 barrel, bit2 eraser, bit3 invert, bit4 in-range.
    uint16_t PenX, PenY, PenPressure;
    int8_t   PenTiltX, PenTiltY;
    uint8_t  PenFlags;

    // Gamepad/digitizer output/feature event reader (drains the event channel).
    VoidrvRumbleCallback RumbleCb;
    void*                RumbleCtx;
    HANDLE               RumbleThread;
    volatile LONG        RumbleStop;
};

// Pad output/feature event reader (defined below; started for pad handles).
static DWORD WINAPI VoidrvPadEventReader(LPVOID param);

VoidrvStatus VoidrvInputQueryStatus(void)
{
    HANDLE h = OpenInterface(&GUID_DEVINTERFACE_VOIDINPUT, true);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        return VOIDRV_STATUS_NOT_INSTALLED;
    }
    CloseHandle(h);
    return VOIDRV_STATUS_OK;
}

uint32_t VoidrvInputVersion(void)
{
    HANDLE h = OpenInterface(&GUID_DEVINTERFACE_VOIDINPUT, true);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        return 0;
    }
    ULONG version = 0;
    bool ok = InputControl(h, IOCTL_VOIDINPUT_VERSION, nullptr, 0, &version, sizeof(version), nullptr);
    CloseHandle(h);
    return ok ? version : 0;
}

VoidrvInputHandle VoidrvInputCreate(VoidrvInputType type)
{
    HANDLE h = OpenInterface(&GUID_DEVINTERFACE_VOIDINPUT, true);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        return nullptr;
    }

    VOIDINPUT_CREATE req;
    ZeroMemory(&req, sizeof(req));
    req.Type = (UINT32)type;
    if (!InputControl(h, IOCTL_VOIDINPUT_CREATE, &req, sizeof(req), nullptr, 0, nullptr)) {
        CloseHandle(h);
        return nullptr;
    }

    // The HID device has just arrived, but the OS opens its input read-pipe a beat
    // later; reports submitted before that are dropped (VHF has no pending read to
    // complete). Let it settle so the caller's FIRST report is not lost - otherwise
    // e.g. the first keystroke after create goes missing. One-time, at creation.
    Sleep(150);

    auto* d = new (std::nothrow) VoidrvInput();   // value-init zeroes the state fields
    if (!d) {
        CloseHandle(h);   // also removes the device
        return nullptr;
    }
    d->Device = h;
    d->Type   = type;

    // Pads and the touch digitizer need the output/feature reader running from the
    // start: DS4/DS5 answer feature requests during enumeration, pads receive
    // rumble, and the touch screen answers the contact-count-maximum feature read.
    if (type == VOIDRV_INPUT_XBOXONE || type == VOIDRV_INPUT_DS4 ||
        type == VOIDRV_INPUT_DS5 || type == VOIDRV_INPUT_TOUCH) {
        d->RumbleStop   = 0;
        d->RumbleThread = CreateThread(nullptr, 0, VoidrvPadEventReader, d, 0, nullptr);
        // If the thread fails to start, the device still works for input.
    }
    return d;
}

void VoidrvInputClose(VoidrvInputHandle handle)
{
    if (!handle) {
        return;
    }
    if (handle->RumbleThread) {
        // Unblock the reader's pending GET_EVENT with CancelIoEx (CloseHandle alone
        // does NOT cancel an in-flight synchronous IOCTL on another thread, so the
        // join would hang). Join first, then close the handle (removes the device).
        InterlockedExchange(&handle->RumbleStop, 1);
        if (handle->Device && handle->Device != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle->Device, nullptr);
        }
        WaitForSingleObject(handle->RumbleThread, INFINITE);
        CloseHandle(handle->RumbleThread);
        handle->RumbleThread = nullptr;
    }
    if (handle->Device && handle->Device != INVALID_HANDLE_VALUE) {
        CloseHandle(handle->Device);   // closing the handle removes the device
    }
    delete handle;
}

// ---- internal report packers ----

// One 8-byte mouse report (report-id + buttons + X + Y + wheels). X/Y are
// little-endian 16-bit; the driver reads them per the report id (relative =
// signed delta, absolute = 0..32767).
static bool MouseSubmit(VoidrvInputHandle handle, uint8_t reportId, uint8_t buttons,
                        uint16_t x, uint16_t y, int8_t wheel, int8_t hwheel)
{
    if (!handle || handle->Type != VOIDRV_INPUT_MOUSE ||
        handle->Device == INVALID_HANDLE_VALUE) {
        return false;
    }
    uint8_t report[8];
    report[0] = reportId;
    report[1] = buttons;
    report[2] = (uint8_t)(x & 0xFF);
    report[3] = (uint8_t)(x >> 8);
    report[4] = (uint8_t)(y & 0xFF);
    report[5] = (uint8_t)(y >> 8);
    report[6] = (uint8_t)wheel;
    report[7] = (uint8_t)hwheel;

    return InputWrite(handle->Device, report, sizeof(report));
}

// One 9-byte boot-keyboard report from the handle's held modifier + key state.
static bool KeyboardSubmit(VoidrvInputHandle handle)
{
    if (!handle || handle->Type != VOIDRV_INPUT_KEYBOARD ||
        handle->Device == INVALID_HANDLE_VALUE) {
        return false;
    }
    uint8_t report[9];
    report[0] = VOIDRV_KBD_RID_KEYBOARD;
    report[1] = handle->KbdModifiers;
    report[2] = 0;   // reserved
    for (int i = 0; i < 6; ++i) {
        report[3 + i] = handle->KbdKeys[i];
    }
    return InputWrite(handle->Device, report, sizeof(report));
}

static int16_t ClampI16(int32_t v)
{
    if (v < -32767) return -32767;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

// Map a desktop pixel coordinate to the absolute 0..32767 range, within the
// handle's bounds rect or (default) the primary display. Re-queried each call so
// it tracks Void's own display add/remove/resize.
static void MapPixels(VoidrvInputHandle handle, int32_t px, int32_t py,
                      uint16_t* outX, uint16_t* outY)
{
    int32_t left = handle->BoundsLeft, top = handle->BoundsTop;
    int32_t w = handle->BoundsW, h = handle->BoundsH;
    if (w <= 0 || h <= 0) {
        left = 0;
        top  = 0;
        w = GetSystemMetrics(SM_CXSCREEN);   // primary display
        h = GetSystemMetrics(SM_CYSCREEN);
    }
    if (w < 2) w = 2;
    if (h < 2) h = 2;

    int32_t rx = px - left;
    int32_t ry = py - top;
    if (rx < 0) rx = 0; else if (rx > w - 1) rx = w - 1;
    if (ry < 0) ry = 0; else if (ry > h - 1) ry = h - 1;

    *outX = (uint16_t)(((int64_t)rx * 32767) / (w - 1));
    *outY = (uint16_t)(((int64_t)ry * 32767) / (h - 1));
}

// ---- raw report tier ----

bool VoidrvInputMouseReportRelative(VoidrvInputHandle handle, uint8_t buttons,
                                    int16_t dx, int16_t dy, int8_t wheel, int8_t hwheel)
{
    return MouseSubmit(handle, VOIDRV_MOUSE_RID_RELATIVE, buttons,
                       (uint16_t)dx, (uint16_t)dy, wheel, hwheel);
}

bool VoidrvInputMouseReportAbsolute(VoidrvInputHandle handle, uint8_t buttons,
                                    uint16_t x, uint16_t y, int8_t wheel, int8_t hwheel)
{
    return MouseSubmit(handle, VOIDRV_MOUSE_RID_ABSOLUTE, buttons, x, y, wheel, hwheel);
}

bool VoidrvInputKeyboardReport(VoidrvInputHandle handle, uint8_t modifiers,
                               const uint8_t keys[6])
{
    if (!handle || handle->Type != VOIDRV_INPUT_KEYBOARD ||
        handle->Device == INVALID_HANDLE_VALUE) {
        return false;
    }
    uint8_t report[9];
    report[0] = VOIDRV_KBD_RID_KEYBOARD;
    report[1] = modifiers;
    report[2] = 0;
    for (int i = 0; i < 6; ++i) {
        report[3 + i] = keys ? keys[i] : 0;
    }
    return InputWrite(handle->Device, report, sizeof(report));
}

bool VoidrvInputConsumerReport(VoidrvInputHandle handle, uint16_t usage)
{
    if (!handle || handle->Type != VOIDRV_INPUT_KEYBOARD ||
        handle->Device == INVALID_HANDLE_VALUE) {
        return false;
    }
    uint8_t report[3];
    report[0] = VOIDRV_KBD_RID_CONSUMER;
    report[1] = (uint8_t)(usage & 0xFF);
    report[2] = (uint8_t)(usage >> 8);
    return InputWrite(handle->Device, report, sizeof(report));
}

// ---- stateful event tier ----

bool VoidrvInputMouseMove(VoidrvInputHandle handle, bool relative, int32_t x, int32_t y)
{
    if (!handle || handle->Type != VOIDRV_INPUT_MOUSE) {
        return false;
    }
    if (relative) {
        return MouseSubmit(handle, VOIDRV_MOUSE_RID_RELATIVE, handle->MouseButtons,
                           (uint16_t)ClampI16(x), (uint16_t)ClampI16(y), 0, 0);
    }
    uint16_t ax = (uint16_t)(x < 0 ? 0 : (x > 32767 ? 32767 : x));
    uint16_t ay = (uint16_t)(y < 0 ? 0 : (y > 32767 ? 32767 : y));
    return MouseSubmit(handle, VOIDRV_MOUSE_RID_ABSOLUTE, handle->MouseButtons, ax, ay, 0, 0);
}

bool VoidrvInputMouseMoveAbsolutePixels(VoidrvInputHandle handle, int32_t px, int32_t py)
{
    if (!handle || handle->Type != VOIDRV_INPUT_MOUSE) {
        return false;
    }
    uint16_t ax = 0, ay = 0;
    MapPixels(handle, px, py, &ax, &ay);
    return MouseSubmit(handle, VOIDRV_MOUSE_RID_ABSOLUTE, handle->MouseButtons, ax, ay, 0, 0);
}

bool VoidrvInputMouseSetBounds(VoidrvInputHandle handle, int32_t left, int32_t top,
                               int32_t width, int32_t height)
{
    if (!handle || handle->Type != VOIDRV_INPUT_MOUSE) {
        return false;
    }
    handle->BoundsLeft = left;
    handle->BoundsTop  = top;
    handle->BoundsW    = width;
    handle->BoundsH    = height;
    return true;
}

bool VoidrvInputMouseButton(VoidrvInputHandle handle, uint8_t button, bool down)
{
    if (!handle || handle->Type != VOIDRV_INPUT_MOUSE) {
        return false;
    }
    if (down) {
        handle->MouseButtons |= button;
    } else {
        handle->MouseButtons = (uint8_t)(handle->MouseButtons & ~button);
    }
    // Carry the button change on a relative report with no motion, so the cursor
    // does not jump regardless of how it was last positioned.
    return MouseSubmit(handle, VOIDRV_MOUSE_RID_RELATIVE, handle->MouseButtons, 0, 0, 0, 0);
}

bool VoidrvInputMouseWheel(VoidrvInputHandle handle, int8_t dv, int8_t dh)
{
    if (!handle || handle->Type != VOIDRV_INPUT_MOUSE) {
        return false;
    }
    return MouseSubmit(handle, VOIDRV_MOUSE_RID_RELATIVE, handle->MouseButtons, 0, 0, dv, dh);
}

bool VoidrvInputKey(VoidrvInputHandle handle, uint16_t hidUsage, bool down)
{
    if (!handle || handle->Type != VOIDRV_INPUT_KEYBOARD) {
        return false;
    }
    if (hidUsage >= 0xE0 && hidUsage <= 0xE7) {
        uint8_t bit = (uint8_t)(1u << (hidUsage - 0xE0));
        if (down) {
            handle->KbdModifiers |= bit;
        } else {
            handle->KbdModifiers = (uint8_t)(handle->KbdModifiers & ~bit);
        }
    } else if (hidUsage != 0 && hidUsage <= 0xFF) {
        uint8_t u = (uint8_t)hidUsage;
        int found = -1, empty = -1;
        for (int i = 0; i < 6; ++i) {
            if (handle->KbdKeys[i] == u) { found = i; break; }
            if (handle->KbdKeys[i] == 0 && empty < 0) { empty = i; }
        }
        if (down) {
            if (found < 0 && empty >= 0) {
                handle->KbdKeys[empty] = u;   // rollover full -> drop the extra key
            }
        } else if (found >= 0) {
            handle->KbdKeys[found] = 0;
        }
    } else {
        return false;   // outside the boot-keyboard usage range
    }
    return KeyboardSubmit(handle);
}

bool VoidrvInputConsumer(VoidrvInputHandle handle, uint16_t usage)
{
    return VoidrvInputConsumerReport(handle, usage);
}

bool VoidrvInputReset(VoidrvInputHandle handle)
{
    if (!handle) {
        return false;
    }
    if (handle->Type == VOIDRV_INPUT_MOUSE) {
        handle->MouseButtons = 0;
        return MouseSubmit(handle, VOIDRV_MOUSE_RID_RELATIVE, 0, 0, 0, 0, 0);
    }
    if (handle->Type == VOIDRV_INPUT_KEYBOARD) {
        handle->KbdModifiers = 0;
        for (int i = 0; i < 6; ++i) {
            handle->KbdKeys[i] = 0;
        }
        return KeyboardSubmit(handle);
    }
    return false;
}

// ---- gamepad ----

// Pack a VoidrvPadState into the 17-byte Xbox One HID report (no report id).
static void PackXboxPad(const VoidrvPadState* s, uint8_t r[17])
{
    auto put16 = [](uint8_t* p, int32_t v) {
        if (v < 0) v = 0; else if (v > 0xFFFF) v = 0xFFFF;
        p[0] = (uint8_t)(v & 0xFF);
        p[1] = (uint8_t)((v >> 8) & 0xFF);
    };
    memset(r, 0, 17);
    put16(r + 0, (int32_t)s->ThumbLX + 0x8000);    // X: right positive
    put16(r + 2, 0x8000 - (int32_t)s->ThumbLY);    // Y: HID down positive (invert)
    put16(r + 4, (int32_t)s->ThumbRX + 0x8000);
    put16(r + 6, 0x8000 - (int32_t)s->ThumbRY);
    put16(r + 8,  (int32_t)s->LeftTrigger  << 2);  // 8-bit -> 10-bit range
    put16(r + 10, (int32_t)s->RightTrigger << 2);

    uint16_t b = s->Buttons;
    uint8_t b0 = 0, b1 = 0;
    if (b & VOIDRV_PAD_A)          b0 |= 0x01;
    if (b & VOIDRV_PAD_B)          b0 |= 0x02;
    if (b & VOIDRV_PAD_X)          b0 |= 0x04;
    if (b & VOIDRV_PAD_Y)          b0 |= 0x08;
    if (b & VOIDRV_PAD_LSHOULDER)  b0 |= 0x10;
    if (b & VOIDRV_PAD_RSHOULDER)  b0 |= 0x20;
    if (b & VOIDRV_PAD_BACK)       b0 |= 0x40;
    if (b & VOIDRV_PAD_START)      b0 |= 0x80;
    if (b & VOIDRV_PAD_LTHUMB)     b1 |= 0x01;
    if (b & VOIDRV_PAD_RTHUMB)     b1 |= 0x02;
    r[12] = b0;
    r[13] = b1;

    bool up = (b & VOIDRV_PAD_DPAD_UP)    != 0;
    bool dn = (b & VOIDRV_PAD_DPAD_DOWN)  != 0;
    bool lf = (b & VOIDRV_PAD_DPAD_LEFT)  != 0;
    bool rg = (b & VOIDRV_PAD_DPAD_RIGHT) != 0;
    uint8_t hat = 0;   // 0 = neutral (null state)
    if      (up && rg) hat = 2;
    else if (rg && dn) hat = 4;
    else if (dn && lf) hat = 6;
    else if (lf && up) hat = 8;
    else if (up)       hat = 1;
    else if (rg)       hat = 3;
    else if (dn)       hat = 5;
    else if (lf)       hat = 7;
    r[14] = (uint8_t)(hat & 0x0F);

    if (b & VOIDRV_PAD_GUIDE) r[15] |= 0x01;
    r[16] = 0xFF;   // battery: full
}

// Pack a VoidrvPadState into the 64-byte DualShock 4 input report (report id 1).
// Sticks are 8-bit (0x80 centered, Y inverted), triggers 8-bit, dpad as a hat,
// face buttons mapped A=Cross B=Circle X=Square Y=Triangle. Gyro/accel/touch are
// left neutral (a later refinement).
static void PackDs4Pad(const VoidrvPadState* s, uint8_t r[64])
{
    auto axis8 = [](int16_t v) -> uint8_t {
        int x = ((int)v + 32768) >> 8;
        if (x < 0) x = 0; else if (x > 255) x = 255;
        return (uint8_t)x;
    };
    memset(r, 0, 64);
    r[0] = 0x01;                                   // report id
    r[1] = axis8(s->ThumbLX);
    r[2] = (uint8_t)(255 - axis8(s->ThumbLY));     // Y: up = low
    r[3] = axis8(s->ThumbRX);
    r[4] = (uint8_t)(255 - axis8(s->ThumbRY));

    uint16_t b = s->Buttons;
    bool up = (b & VOIDRV_PAD_DPAD_UP)    != 0;
    bool dn = (b & VOIDRV_PAD_DPAD_DOWN)  != 0;
    bool lf = (b & VOIDRV_PAD_DPAD_LEFT)  != 0;
    bool rg = (b & VOIDRV_PAD_DPAD_RIGHT) != 0;
    uint8_t hat = 8;   // 8 = neutral
    if      (up && rg) hat = 1;
    else if (rg && dn) hat = 3;
    else if (dn && lf) hat = 5;
    else if (lf && up) hat = 7;
    else if (up)       hat = 0;
    else if (rg)       hat = 2;
    else if (dn)       hat = 4;
    else if (lf)       hat = 6;

    uint8_t b5 = (uint8_t)(hat & 0x0F);
    if (b & VOIDRV_PAD_X) b5 |= 0x10;   // Square
    if (b & VOIDRV_PAD_A) b5 |= 0x20;   // Cross
    if (b & VOIDRV_PAD_B) b5 |= 0x40;   // Circle
    if (b & VOIDRV_PAD_Y) b5 |= 0x80;   // Triangle
    r[5] = b5;

    uint8_t b6 = 0;
    if (b & VOIDRV_PAD_LSHOULDER) b6 |= 0x01;   // L1
    if (b & VOIDRV_PAD_RSHOULDER) b6 |= 0x02;   // R1
    if (s->LeftTrigger  > 0)      b6 |= 0x04;   // L2
    if (s->RightTrigger > 0)      b6 |= 0x08;   // R2
    if (b & VOIDRV_PAD_BACK)      b6 |= 0x10;   // Share
    if (b & VOIDRV_PAD_START)     b6 |= 0x20;   // Options
    if (b & VOIDRV_PAD_LTHUMB)    b6 |= 0x40;   // L3
    if (b & VOIDRV_PAD_RTHUMB)    b6 |= 0x80;   // R3
    r[6] = b6;

    if (b & VOIDRV_PAD_GUIDE) r[7] |= 0x01;     // PS button (counter in high bits)
    r[8] = s->LeftTrigger;
    r[9] = s->RightTrigger;
    r[12] = 0xFF;   // battery: wired/full
}

// Pack a VoidrvPadState into the 64-byte DualSense input report (report id 1).
// The layout differs from the DualShock 4: the two triggers come right after the
// sticks, then a sequence byte, then three button bytes. Sticks are 8-bit (0x80
// centered, Y inverted); face buttons map A=Cross B=Circle X=Square Y=Triangle;
// the dpad packs into a hat. Gyro/accel/touch are left neutral (a later
// refinement); a plausible wired-full battery is set.
static void PackDs5Pad(const VoidrvPadState* s, uint8_t r[64])
{
    auto axis8 = [](int16_t v) -> uint8_t {
        int x = ((int)v + 32768) >> 8;
        if (x < 0) x = 0; else if (x > 255) x = 255;
        return (uint8_t)x;
    };
    memset(r, 0, 64);
    r[0] = 0x01;                                   // report id
    r[1] = axis8(s->ThumbLX);
    r[2] = (uint8_t)(255 - axis8(s->ThumbLY));     // Y: up = low
    r[3] = axis8(s->ThumbRX);
    r[4] = (uint8_t)(255 - axis8(s->ThumbRY));
    r[5] = s->LeftTrigger;                         // L2 analog
    r[6] = s->RightTrigger;                        // R2 analog
    r[7] = 0;                                      // sequence counter (unused)

    uint16_t b = s->Buttons;
    bool up = (b & VOIDRV_PAD_DPAD_UP)    != 0;
    bool dn = (b & VOIDRV_PAD_DPAD_DOWN)  != 0;
    bool lf = (b & VOIDRV_PAD_DPAD_LEFT)  != 0;
    bool rg = (b & VOIDRV_PAD_DPAD_RIGHT) != 0;
    uint8_t hat = 8;   // 8 = neutral
    if      (up && rg) hat = 1;
    else if (rg && dn) hat = 3;
    else if (dn && lf) hat = 5;
    else if (lf && up) hat = 7;
    else if (up)       hat = 0;
    else if (rg)       hat = 2;
    else if (dn)       hat = 4;
    else if (lf)       hat = 6;

    uint8_t b8 = (uint8_t)(hat & 0x0F);
    if (b & VOIDRV_PAD_X) b8 |= 0x10;   // Square
    if (b & VOIDRV_PAD_A) b8 |= 0x20;   // Cross
    if (b & VOIDRV_PAD_B) b8 |= 0x40;   // Circle
    if (b & VOIDRV_PAD_Y) b8 |= 0x80;   // Triangle
    r[8] = b8;

    uint8_t b9 = 0;
    if (b & VOIDRV_PAD_LSHOULDER) b9 |= 0x01;   // L1
    if (b & VOIDRV_PAD_RSHOULDER) b9 |= 0x02;   // R1
    if (s->LeftTrigger  > 0)      b9 |= 0x04;   // L2
    if (s->RightTrigger > 0)      b9 |= 0x08;   // R2
    if (b & VOIDRV_PAD_BACK)      b9 |= 0x10;   // Create/Share
    if (b & VOIDRV_PAD_START)     b9 |= 0x20;   // Options
    if (b & VOIDRV_PAD_LTHUMB)    b9 |= 0x40;   // L3
    if (b & VOIDRV_PAD_RTHUMB)    b9 |= 0x80;   // R3
    r[9] = b9;

    uint8_t b10 = 0;
    if (b & VOIDRV_PAD_GUIDE) b10 |= 0x01;      // PS button (bit1 touchpad, bit2 mute)
    r[10] = b10;

    r[53] = 0x2A;   // battery: wired, full (BatteryState 0x2 << 4 | BatteryPercent 0xA)
}

// DS4 feature-report handshake responses (neutral calibration, firmware id,
// pairing/MAC). Each array begins with its report id, matching what the OS reads
// back. Returns the byte count written, or 0 for an unhandled feature report.
static const uint8_t k_Ds4Calibration[] = {
    0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         // gyro pitch/yaw/roll bias
    0x10, 0x27, 0xF0, 0xD8, 0x10, 0x27, 0xF0, 0xD8, 0x10, 0x27, 0xF0, 0xD8,
    0xF4, 0x01, 0xF4, 0x01,                     // gyro speed +/-
    0x10, 0x27, 0xF0, 0xD8, 0x10, 0x27, 0xF0, 0xD8, 0x10, 0x27, 0xF0, 0xD8,
};
static const uint8_t k_Ds4Firmware[] = {
    0xA3, 0x41, 0x75, 0x67, 0x20, 0x20, 0x33, 0x20,
    0x32, 0x30, 0x31, 0x33, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x30, 0x37, 0x3A, 0x30, 0x31, 0x3A, 0x31,
    0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x31, 0x03, 0x00, 0x00,
    0x00, 0x49, 0x00, 0x05, 0x00, 0x00, 0x80, 0x03, 0x00,
};
static const uint8_t k_Ds4Mac[6] = { 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F };

static uint32_t BuildDs4Feature(uint8_t reportId, uint8_t* out)
{
    if (reportId == 0x02) {
        memcpy(out, k_Ds4Calibration, sizeof(k_Ds4Calibration));
        return (uint32_t)sizeof(k_Ds4Calibration);
    }
    if (reportId == 0xA3) {
        memcpy(out, k_Ds4Firmware, sizeof(k_Ds4Firmware));
        return (uint32_t)sizeof(k_Ds4Firmware);
    }
    if (reportId == 0x12) {            // pairing info: id + MAC (reversed)
        out[0] = 0x12;
        for (int i = 0; i < 6; ++i) {
            out[1 + i] = k_Ds4Mac[5 - i];
        }
        return 7;
    }
    return 0;                          // unhandled -> empty response
}

// DualSense feature-report handshake. Same convention as DS4: byte 0 of each
// response is the report id (the HID stack expects the report-id byte at the head
// of a numbered feature buffer). Neutral gyro/accel calibration, a plausible
// firmware-info block, and the pairing report carrying our MAC (reversed).
static const uint8_t k_Ds5Calibration[] = {
    0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         // gyro pitch/yaw/roll bias
    0x10, 0x27, 0xF0, 0xD8, 0x10, 0x27, 0xF0, 0xD8, 0x10, 0x27, 0xF0, 0xD8,  // gyro range
    0xF4, 0x01, 0xF4, 0x01,                     // gyro speed +/-
    0x10, 0x27, 0xF0, 0xD8, 0x10, 0x27, 0xF0, 0xD8, 0x10, 0x27, 0xF0, 0xD8,  // accel range
};
static const uint8_t k_Ds5Firmware[] = {
    0x20, 0x4A, 0x61, 0x6E, 0x20, 0x32, 0x39, 0x20,
    0x32, 0x30, 0x32, 0x34, 0x30, 0x39, 0x3A, 0x31,
    0x33, 0x3A, 0x35, 0x39, 0x02, 0x00, 0x04, 0x00,
    0x14, 0x04, 0x00, 0x00, 0x0A, 0x00, 0x0C, 0x01,
    0x51, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x58, 0x04, 0x00, 0x00,
    0x2A, 0x00, 0x01, 0x00, 0x09, 0x00, 0x02, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t k_Ds5Mac[6] = { 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F };

static uint32_t BuildDs5Feature(uint8_t reportId, uint8_t* out)
{
    if (reportId == 0x05) {            // calibration
        memcpy(out, k_Ds5Calibration, sizeof(k_Ds5Calibration));
        return (uint32_t)sizeof(k_Ds5Calibration);
    }
    if (reportId == 0x20) {            // firmware info (byte 0 is the report id)
        memcpy(out, k_Ds5Firmware, sizeof(k_Ds5Firmware));
        return (uint32_t)sizeof(k_Ds5Firmware);
    }
    if (reportId == 0x09) {            // pairing info: id + MAC (reversed) + padding
        memset(out, 0, 20);
        out[0] = 0x09;
        for (int i = 0; i < 6; ++i) {
            out[1 + i] = k_Ds5Mac[5 - i];
        }
        return 20;
    }
    return 0;                          // unhandled -> empty response
}

bool VoidrvInputPadReport(VoidrvInputHandle handle, const VoidrvPadState* state)
{
    if (!handle || !state || handle->Device == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (handle->Type == VOIDRV_INPUT_XBOXONE) {
        uint8_t report[17];
        PackXboxPad(state, report);
        return InputWrite(handle->Device, report, sizeof(report));
    }
    if (handle->Type == VOIDRV_INPUT_DS4) {
        uint8_t report[64];
        PackDs4Pad(state, report);
        return InputWrite(handle->Device, report, sizeof(report));
    }
    if (handle->Type == VOIDRV_INPUT_DS5) {
        uint8_t report[64];
        PackDs5Pad(state, report);
        return InputWrite(handle->Device, report, sizeof(report));
    }
    return false;
}

// ---- touch + pen digitizer ----

// Index of the active slot holding contactId, or -1.
static int TouchFindSlot(VoidrvInputHandle h, uint8_t id)
{
    for (uint8_t i = 0; i < h->TouchCount; ++i) {
        if (h->TouchSlots[i].Id == id) {
            return (int)i;
        }
    }
    return -1;
}

// Build the 64-byte touch report (id 1) from the active-contact prefix:
// 10 fingers x {flags, id, X(2), Y(2)} + contact count + scan time.
static void PackTouchReport(VoidrvInputHandle h, uint8_t r[64])
{
    memset(r, 0, 64);
    r[0] = VOIDRV_TOUCH_RID_TOUCH;
    for (uint8_t i = 0; i < h->TouchCount && i < VOIDRV_TOUCH_MAX_CONTACTS; ++i) {
        uint8_t* f = r + 1 + i * 6;
        f[0] = h->TouchSlots[i].Tip ? 0x03 : 0x00;   // tip switch + confidence
        f[1] = h->TouchSlots[i].Id;
        f[2] = (uint8_t)(h->TouchSlots[i].X & 0xFF);
        f[3] = (uint8_t)(h->TouchSlots[i].X >> 8);
        f[4] = (uint8_t)(h->TouchSlots[i].Y & 0xFF);
        f[5] = (uint8_t)(h->TouchSlots[i].Y >> 8);
    }
    r[61] = h->TouchCount;
    h->TouchScanTime = (uint16_t)(h->TouchScanTime + 10);   // monotonic (100us units)
    r[62] = (uint8_t)(h->TouchScanTime & 0xFF);
    r[63] = (uint8_t)(h->TouchScanTime >> 8);
}

static bool TouchSubmit(VoidrvInputHandle h)
{
    uint8_t report[64];
    PackTouchReport(h, report);
    return InputWrite(h->Device, report, sizeof(report));
}

// Drop the slot at index s, compacting the active prefix (Windows wants the live
// contacts contiguous in the next report).
static void TouchReleaseSlot(VoidrvInputHandle h, int s)
{
    if (s < 0 || (uint8_t)s >= h->TouchCount) {
        return;
    }
    for (uint8_t i = (uint8_t)s; i + 1 < h->TouchCount; ++i) {
        h->TouchSlots[i] = h->TouchSlots[i + 1];
    }
    h->TouchCount--;
}

// Submit the pen report (id 3) from the handle's pen state.
static bool PenSubmit(VoidrvInputHandle h)
{
    uint8_t r[10];
    memset(r, 0, sizeof(r));
    r[0] = VOIDRV_TOUCH_RID_PEN;
    r[1] = h->PenFlags;
    r[2] = (uint8_t)(h->PenX & 0xFF);        r[3] = (uint8_t)(h->PenX >> 8);
    r[4] = (uint8_t)(h->PenY & 0xFF);        r[5] = (uint8_t)(h->PenY >> 8);
    r[6] = (uint8_t)(h->PenPressure & 0xFF); r[7] = (uint8_t)(h->PenPressure >> 8);
    r[8] = (uint8_t)h->PenTiltX;
    r[9] = (uint8_t)h->PenTiltY;
    return InputWrite(h->Device, r, sizeof(r));
}

bool VoidrvInputTouchContact(VoidrvInputHandle handle, uint8_t contactId, uint8_t action,
                             int32_t px, int32_t py, uint16_t pressure, uint8_t size)
{
    (void)pressure; (void)size;   // contact area is implied; not carried by the report
    if (!handle || handle->Type != VOIDRV_INPUT_TOUCH ||
        handle->Device == INVALID_HANDLE_VALUE) {
        return false;
    }

    uint16_t ax = 0, ay = 0;
    MapPixels(handle, px, py, &ax, &ay);

    int slot = TouchFindSlot(handle, contactId);
    if (slot < 0) {
        if (action == VOIDRV_TOUCH_UP || action == VOIDRV_TOUCH_CANCEL) {
            return true;   // lifting an unknown contact - nothing to do
        }
        if (handle->TouchCount >= VOIDRV_TOUCH_MAX_CONTACTS) {
            return false;  // contact table full
        }
        slot = handle->TouchCount++;
        handle->TouchSlots[slot].Id = contactId;
    }

    handle->TouchSlots[slot].X = ax;
    handle->TouchSlots[slot].Y = ay;
    handle->TouchSlots[slot].Tip =
        (action == VOIDRV_TOUCH_DOWN || action == VOIDRV_TOUCH_MOVE) ? (uint8_t)1 : (uint8_t)0;

    bool ok = TouchSubmit(handle);   // the lift frame carries tip=0 for this contact
    if (action == VOIDRV_TOUCH_UP || action == VOIDRV_TOUCH_CANCEL) {
        TouchReleaseSlot(handle, slot);
    }
    return ok;
}

bool VoidrvInputTouchCancelAll(VoidrvInputHandle handle)
{
    if (!handle || handle->Type != VOIDRV_INPUT_TOUCH ||
        handle->Device == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = true;
    for (uint8_t i = 0; i < handle->TouchCount; ++i) {
        handle->TouchSlots[i].Tip = 0;
    }
    if (handle->TouchCount > 0) {
        ok = TouchSubmit(handle);   // final frame: every contact lifted
    }
    handle->TouchCount = 0;
    if (handle->PenFlags & 0x10) {  // pen still in range -> lift it out
        handle->PenFlags = 0;
        PenSubmit(handle);
    }
    return ok;
}

bool VoidrvInputTouchSetBounds(VoidrvInputHandle handle, int32_t left, int32_t top,
                               int32_t width, int32_t height)
{
    if (!handle || handle->Type != VOIDRV_INPUT_TOUCH) {
        return false;
    }
    handle->BoundsLeft = left;
    handle->BoundsTop  = top;
    handle->BoundsW    = width;
    handle->BoundsH    = height;
    return true;
}

bool VoidrvInputPen(VoidrvInputHandle handle, uint8_t action, int32_t px, int32_t py,
                    uint16_t pressure, int8_t tiltX, int8_t tiltY, bool barrel, bool eraser)
{
    if (!handle || handle->Type != VOIDRV_INPUT_TOUCH ||
        handle->Device == INVALID_HANDLE_VALUE) {
        return false;
    }

    uint16_t ax = 0, ay = 0;
    MapPixels(handle, px, py, &ax, &ay);
    handle->PenX        = ax;
    handle->PenY        = ay;
    handle->PenPressure = pressure > 1023 ? 1023 : pressure;
    handle->PenTiltX    = tiltX;
    handle->PenTiltY    = tiltY;

    bool tip = false, inRange = true;
    switch (action) {
        case VOIDRV_TOUCH_DOWN:
        case VOIDRV_TOUCH_MOVE:   tip = true;  inRange = true;  break;
        case VOIDRV_TOUCH_UP:                                            // lifted, hovering
        case VOIDRV_TOUCH_HOVER:  tip = false; inRange = true;  break;
        case VOIDRV_TOUCH_CANCEL: tip = false; inRange = false; break;   // out of range
        default: return false;
    }
    uint8_t flags = 0;
    if (tip)     flags |= 0x01;
    if (barrel)  flags |= 0x02;
    if (eraser)  flags |= 0x04 | 0x08;   // eraser implies invert
    if (inRange) flags |= 0x10;
    handle->PenFlags = flags;

    return PenSubmit(handle);
}

// Touch digitizer feature: report the maximum contact count so Windows enumerates
// it as a 10-point multitouch screen. The buffer leads with the report id (our
// feature convention). Returns the byte count, or 0 for an unhandled feature.
static uint32_t BuildTouchFeature(uint8_t reportId, uint8_t* out)
{
    if (reportId == VOIDRV_TOUCH_RID_MAXCOUNT) {
        out[0] = VOIDRV_TOUCH_RID_MAXCOUNT;
        out[1] = VOIDRV_TOUCH_MAX_CONTACTS;
        return 2;
    }
    return 0;
}

// Pad output/feature event reader. Drains the GET_EVENT channel and services
// each event: output reports -> rumble callback (Xbox or DS4), DS4 GET_FEATURE ->
// the canned handshake response, everything else -> succeed. Runs for the life of
// a pad handle (started at create), so DS4/DS5 feature requests are answered even
// before a rumble callback is set.
static DWORD WINAPI VoidrvPadEventReader(LPVOID param)
{
    auto* h = (VoidrvInputHandle)param;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        return 0;
    }
    for (;;) {
        if (InterlockedCompareExchange(&h->RumbleStop, 0, 0) != 0) {
            break;
        }
        VOIDINPUT_EVENT ev;
        DWORD br = 0;
        ResetEvent(ov.hEvent);
        BOOL ok = DeviceIoControl(h->Device, IOCTL_VOIDINPUT_GET_EVENT, nullptr, 0,
                                  &ev, sizeof(ev), &br, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ok = GetOverlappedResult(h->Device, &ov, &br, TRUE);
        }
        if (!ok) {
            break;   // cancelled on close (CancelIoEx) or error
        }

        VOIDINPUT_EVENT_COMPLETE done;
        ZeroMemory(&done, sizeof(done));
        done.RequestId = ev.RequestId;
        done.Status    = 0;

        if (ev.Type == VoidInputEventWriteReport) {
            if (h->Type == VOIDRV_INPUT_XBOXONE && ev.DataLength >= 8 && h->RumbleCb) {
                uint8_t en = ev.Data[0];
                h->RumbleCb(h->RumbleCtx,
                            (en & 0x1) ? ev.Data[3] : 0,    // left  (low-freq)
                            (en & 0x2) ? ev.Data[4] : 0,    // right (high-freq)
                            (en & 0x4) ? ev.Data[1] : 0,    // left trigger
                            (en & 0x8) ? ev.Data[2] : 0);   // right trigger
            } else if (h->Type == VOIDRV_INPUT_DS4 && ev.DataLength >= 8 && h->RumbleCb) {
                // Numbered report: ev.Data[0] = id (5), the report body follows.
                uint8_t flags = ev.Data[1];   // RumbleValid:1, LedValid:1, ...
                if (flags & 0x1) {
                    h->RumbleCb(h->RumbleCtx, ev.Data[4], ev.Data[3], 0, 0);  // left, right
                }
            } else if (h->Type == VOIDRV_INPUT_DS5 && ev.DataLength >= 5 && h->RumbleCb) {
                // Output report id 0x02: ev.Data[0]=id, [1]=flags0, [2]=flags1,
                // [3]=right motor, [4]=left motor.
                uint8_t flags0 = ev.Data[1];   // bit0/bit1 = compatible vibration
                if (flags0 & 0x3) {
                    h->RumbleCb(h->RumbleCtx, ev.Data[4], ev.Data[3], 0, 0);  // left, right
                }
            }
        }
        else if (ev.Type == VoidInputEventGetFeature) {
            if (h->Type == VOIDRV_INPUT_DS4) {
                done.DataLength = BuildDs4Feature(ev.ReportId, done.Data);
            } else if (h->Type == VOIDRV_INPUT_DS5) {
                done.DataLength = BuildDs5Feature(ev.ReportId, done.Data);
            } else if (h->Type == VOIDRV_INPUT_TOUCH) {
                done.DataLength = BuildTouchFeature(ev.ReportId, done.Data);
            }
        }
        // SetFeature (and unhandled GetFeature) just succeed with no data.

        InputControl(h->Device, IOCTL_VOIDINPUT_COMPLETE_EVENT, &done, sizeof(done),
                     nullptr, 0, nullptr);
    }
    CloseHandle(ov.hEvent);
    return 0;
}

bool VoidrvInputPadSetRumbleCallback(VoidrvInputHandle handle, VoidrvRumbleCallback callback, void* context)
{
    if (!handle || (handle->Type != VOIDRV_INPUT_XBOXONE &&
                    handle->Type != VOIDRV_INPUT_DS4 &&
                    handle->Type != VOIDRV_INPUT_DS5)) {
        return false;
    }
    // The event reader is already running (started at create for pads); just set
    // the callback it invokes.
    handle->RumbleCb  = callback;
    handle->RumbleCtx = context;
    return true;
}

} // extern "C"
