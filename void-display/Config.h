/*
 * VoidDisplay - operator configuration in a file (read by the driver at init).
 *
 * Config lives in C:\ProgramData\.voidrv\display.ini instead of the registry so an
 * unelevated controller can write it (the installer grants Users:Modify on the
 * folder). The UMDF host can read this path but cannot write any registry/file -
 * see the persistence design. Keys:
 *
 *   [render]   PreferredAdapterVendorId = 0    ; 0 auto / 0x10DE / 0x1002 / 0x8086
 *   [cursor]   HardwareCursor           = 1    ; 1 overlay, 0 bake into frames
 *   [startup]  RestoreOnStart           = 1    ; recreate [displays] at init
 *   [edid]     UseCustomEdid            = 0    ; 1 = clone display.edid
 *              PatchSerial              = 1    ; per-slot serial (multi-display safe)
 *   [modes]    Custom = WxH@Hz, WxH@Hz, ...    ; extra resolutions
 *   [displays] <slot> = WxH@Hz                 ; persisted active displays
 */

#pragma once

#include <windows.h>
#include <stdlib.h>   // wcstoul
#include <stdio.h>    // swscanf_s

#define VOID_INI_PATH  L"C:\\ProgramData\\.voidrv\\display.ini"
#define VOID_EDID_PATH L"C:\\ProgramData\\.voidrv\\display.edid"

// Read a uint (decimal or 0x-hex) from display.ini; returns def if the key is absent.
static inline DWORD VoidIniUInt(const wchar_t* section, const wchar_t* key, DWORD def)
{
    wchar_t buf[64] = {};
    if (GetPrivateProfileStringW(section, key, L"", buf, ARRAYSIZE(buf), VOID_INI_PATH) == 0) {
        return def;
    }
    return (DWORD)wcstoul(buf, nullptr, 0);  // base 0: handles "0x1002" and decimal
}

// Parse "WxH@Hz" (refresh optional, defaults 60). Returns false on malformed input.
static inline bool VoidParseModeStr(const wchar_t* s, UINT32* w, UINT32* h, UINT32* hz)
{
    unsigned a = 0, b = 0, c = 0;
    if (swscanf_s(s, L"%ux%u@%u", &a, &b, &c) >= 2 && a != 0 && b != 0) {
        *w = a; *h = b; *hz = (c != 0 ? c : 60);
        return true;
    }
    return false;
}
