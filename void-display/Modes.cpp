#include "Modes.h"

// Default mode set advertised on every VoidDisplay monitor. The first entry is
// the default (1920x1080 @ 60) and stays first - it is reported as the preferred
// mode. Refresh-rate variants are separate entries so each is individually
// selectable. The wide-gamut resolutions get a 60/120/144/165 refresh ladder;
// 3:2 and laptop-panel resolutions are 60 Hz only.
const VOID_MODE_DESC g_VoidDefaultModes[] = {
    // 16:9 FHD - default first (index 0 == preferred mode).
    { 1920, 1080,  60 }, { 1920, 1080, 120 }, { 1920, 1080, 144 }, { 1920, 1080, 165 },

    // -- 4K / DCI 4K --
    { 4096, 2160,  60 }, { 4096, 2160, 120 }, { 4096, 2160, 144 }, { 4096, 2160, 165 },
    { 3840, 2160,  60 }, { 3840, 2160, 120 }, { 3840, 2160, 144 }, { 3840, 2160, 165 },

    // -- Ultrawide / super-ultrawide --
    { 3840, 1600,  60 }, { 3840, 1600, 120 }, { 3840, 1600, 144 }, { 3840, 1600, 165 },
    { 3840, 1080,  60 }, { 3840, 1080, 120 }, { 3840, 1080, 144 }, { 3840, 1080, 165 },
    { 3440, 1440,  60 }, { 3440, 1440, 120 }, { 3440, 1440, 144 }, { 3440, 1440, 165 },
    { 2560, 1080,  60 }, { 2560, 1080, 120 }, { 2560, 1080, 144 }, { 2560, 1080, 165 },

    // -- 3K / QHD+ / QHD --
    { 3200, 1800,  60 }, { 3200, 1800, 120 }, { 3200, 1800, 144 }, { 3200, 1800, 165 },
    { 2880, 1620,  60 }, { 2880, 1620, 120 }, { 2880, 1620, 144 }, { 2880, 1620, 165 },
    { 2560, 1600,  60 }, { 2560, 1600, 120 }, { 2560, 1600, 144 }, { 2560, 1600, 165 },
    { 2560, 1440,  60 }, { 2560, 1440, 120 }, { 2560, 1440, 144 }, { 2560, 1440, 165 },
    { 2048, 1152,  60 }, { 2048, 1152, 120 }, { 2048, 1152, 144 }, { 2048, 1152, 165 },

    // -- FHD family / HD+ --
    { 1920, 1200,  60 }, { 1920, 1200, 120 }, { 1920, 1200, 144 }, { 1920, 1200, 165 },
    { 1680, 1050,  60 }, { 1680, 1050, 120 }, { 1680, 1050, 144 }, { 1680, 1050, 165 },
    { 1600, 1200,  60 }, { 1600, 1200, 120 }, { 1600, 1200, 144 }, { 1600, 1200, 165 },
    { 1600,  900,  60 }, { 1600,  900, 120 }, { 1600,  900, 144 }, { 1600,  900, 165 },
    { 1440,  900,  60 }, { 1440,  900, 120 }, { 1440,  900, 144 }, { 1440,  900, 165 },
    { 1366,  768,  60 }, { 1366,  768, 120 }, { 1366,  768, 144 }, { 1366,  768, 165 },
    { 1280,  800,  60 }, { 1280,  800, 120 }, { 1280,  800, 144 }, { 1280,  800, 165 },
    { 1280,  720,  60 }, { 1280,  720, 120 }, { 1280,  720, 144 }, { 1280,  720, 165 },

    // -- 3:2 and laptop panels (60 Hz only) --
    { 3240, 2160,  60 },
    { 3000, 2000,  60 },
    { 2880, 1800,  60 },
    { 2736, 1824,  60 },
    { 2496, 1664,  60 },
    { 2256, 1504,  60 },
    { 1800, 1200,  60 },
};

const unsigned g_VoidDefaultModeCount =
    sizeof(g_VoidDefaultModes) / sizeof(g_VoidDefaultModes[0]);

DISPLAYCONFIG_VIDEO_SIGNAL_INFO VoidCreateSignalInfo(UINT32 width, UINT32 height,
                                                     UINT32 vsync, UINT32 vSyncFreqDivider)
{
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO info = {};

    // Zero-blanking timing (totalSize == activeSize), matching Parsec's VDD and
    // the IddCx samples: pixelRate = w*h*vsync, hSync = h*vsync.
    const UINT64 totalPixels = (UINT64)width * (UINT64)height;

    info.pixelRate              = totalPixels * (UINT64)vsync;
    info.hSyncFreq.Numerator    = (UINT32)((UINT64)height * (UINT64)vsync);
    info.hSyncFreq.Denominator  = 1;
    info.vSyncFreq.Numerator    = vsync;
    info.vSyncFreq.Denominator  = 1;
    info.activeSize.cx          = width;
    info.activeSize.cy          = height;
    info.totalSize.cx           = width;
    info.totalSize.cy           = height;

    // videoStandard = D3DKMDT_VSS_OTHER (255). vSyncFreqDivider must be 0 for a
    // monitor mode and non-zero for a target mode (caller decides).
    info.AdditionalSignalInfo.videoStandard    = 255;
    info.AdditionalSignalInfo.vSyncFreqDivider = vSyncFreqDivider;

    info.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return info;
}

unsigned VoidFindModeIndex(UINT32 width, UINT32 height, UINT32 vsync)
{
    for (unsigned i = 0; i < g_VoidDefaultModeCount; ++i) {
        if (g_VoidDefaultModes[i].Width == width &&
            g_VoidDefaultModes[i].Height == height &&
            g_VoidDefaultModes[i].RefreshHz == vsync) {
            return i;
        }
    }
    return 0;
}

// --- Runtime advertised-mode store ---------------------------------------

// Room for user-added custom modes on top of the built-in defaults; the two
// together must stay within VOIDDISPLAY_MAX_MODES (the IOCTL list / callback
// buffer cap).
#define VOID_MAX_CUSTOM_MODES 32

static SRWLOCK       s_modeLock = SRWLOCK_INIT;
static VOID_MODE_DESC s_customModes[VOID_MAX_CUSTOM_MODES];
static unsigned      s_customCount = 0;

static bool SameMode(const VOID_MODE_DESC& a, UINT32 w, UINT32 h, UINT32 hz)
{
    return a.Width == w && a.Height == h && a.RefreshHz == hz;
}

static bool IsDefaultMode(UINT32 w, UINT32 h, UINT32 hz)
{
    for (unsigned i = 0; i < g_VoidDefaultModeCount; ++i) {
        if (SameMode(g_VoidDefaultModes[i], w, h, hz)) {
            return true;
        }
    }
    return false;
}

bool VoidModesAdd(UINT32 width, UINT32 height, UINT32 hz)
{
    if (width == 0 || height == 0 || hz == 0) {
        return false;
    }
    if (IsDefaultMode(width, height, hz)) {
        return true;  // already advertised
    }

    bool ok = false;
    AcquireSRWLockExclusive(&s_modeLock);
    bool dup = false;
    for (unsigned i = 0; i < s_customCount; ++i) {
        if (SameMode(s_customModes[i], width, height, hz)) { dup = true; break; }
    }
    if (dup) {
        ok = true;
    } else if (s_customCount < VOID_MAX_CUSTOM_MODES) {
        s_customModes[s_customCount].Width = width;
        s_customModes[s_customCount].Height = height;
        s_customModes[s_customCount].RefreshHz = hz;
        s_customCount++;
        ok = true;
    }
    ReleaseSRWLockExclusive(&s_modeLock);
    return ok;
}

bool VoidModesRemove(UINT32 width, UINT32 height, UINT32 hz)
{
    bool removed = false;
    AcquireSRWLockExclusive(&s_modeLock);
    for (unsigned i = 0; i < s_customCount; ++i) {
        if (SameMode(s_customModes[i], width, height, hz)) {
            for (unsigned j = i + 1; j < s_customCount; ++j) {
                s_customModes[j - 1] = s_customModes[j];
            }
            s_customCount--;
            removed = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_modeLock);
    return removed;
}

unsigned VoidModesGet(VOID_MODE_DESC* out, unsigned cap)
{
    unsigned n = 0;
    for (unsigned i = 0; i < g_VoidDefaultModeCount && n < cap; ++i) {
        out[n++] = g_VoidDefaultModes[i];
    }
    AcquireSRWLockShared(&s_modeLock);
    for (unsigned i = 0; i < s_customCount && n < cap; ++i) {
        out[n++] = s_customModes[i];
    }
    ReleaseSRWLockShared(&s_modeLock);
    return n;
}

unsigned VoidModesCount(void)
{
    AcquireSRWLockShared(&s_modeLock);
    unsigned n = g_VoidDefaultModeCount + s_customCount;
    ReleaseSRWLockShared(&s_modeLock);
    return n;
}

// Load custom modes the SDK persisted under the driver's WUDF service Parameters
// key so they survive a device restart or reboot. The value is a REG_BINARY
// array of packed {Width, Height, RefreshHz} UINT32 triples (one VOID_MODE_DESC
// each). Read at adapter init, before any monitor arrives.
void VoidModesLoadPersisted(void)
{
    VOID_MODE_DESC buf[VOID_MAX_CUSTOM_MODES];
    DWORD cb = sizeof(buf);
    LSTATUS rs = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\VoidDisplay\\Parameters",
        L"CustomModes",
        RRF_RT_REG_BINARY, NULL, buf, &cb);
    if (rs != ERROR_SUCCESS) {
        return;  // none persisted
    }
    unsigned n = (unsigned)(cb / sizeof(VOID_MODE_DESC));
    for (unsigned i = 0; i < n; ++i) {
        VoidModesAdd(buf[i].Width, buf[i].Height, buf[i].RefreshHz);
    }
}
