#include "Edid.h"
#include "Config.h"

// A structurally valid EDID 1.4 base block branded for VoidDisplay.
//
//   bytes  8- 9 : manufacturer id "VVD" -> 0x5A 0xC4
//   bytes 10-11 : product code 0x00A1
//   bytes 12-15 : serial number (patched per monitor by VoidBuildEdid)
//   byte     17 : year = 2026 - 1990 = 36 (0x24)
//   bytes 54-71 : preferred detailed timing = 1920x1080 @ 60
//   bytes 72-89 : monitor name descriptor "VoidVDA"
//   byte    127 : checksum (recomputed by VoidBuildEdid)
//
// The full advertised mode list is provided through the IddCx mode callbacks,
// not through this block, so only identity + structural validity matter here.
static const UINT8 s_VoidEdidTemplate[VOIDDISPLAY_EDID_SIZE] = {
    // header
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    // manufacturer "VVD", product 0x00A1, serial (placeholder)
    0x5A, 0xC4, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x00,
    // week 1, year 2026, EDID 1.4
    0x01, 0x24, 0x01, 0x04,
    // digital input (DisplayPort, 8 bpc), 53x30 cm, gamma 2.2, features
    0xA5, 0x35, 0x1E, 0x78, 0x06,
    // chromaticity (structurally valid sRGB-ish)
    0xEE, 0x95, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54,
    // established timings 1/2/3 (none; modes come from the descriptor callbacks)
    0x00, 0x00, 0x00,
    // standard timings 1..8 (unused)
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // detailed timing descriptor 1: 1920x1080 @ 60 (preferred)
    0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C,
    0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1E,
    // descriptor 2: monitor name "VoidVDA" (7 chars + 0x0A terminator + 0x20 padding)
    0x00, 0x00, 0x00, 0xFC, 0x00,
    'V', 'o', 'i', 'd', 'V', 'D', 'A', 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20,
    // descriptor 3: monitor range limits (24-240 Hz, 15-160 kHz, 600 MHz)
    0x00, 0x00, 0x00, 0xFD, 0x00,
    0x18, 0xF0, 0x0F, 0xA0, 0x3C, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    // descriptor 4: unused (dummy)
    0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // extension count, checksum (recomputed)
    0x00, 0x00,
};

void VoidBuildEdid(UINT8 out[VOIDDISPLAY_EDID_SIZE], UINT32 serial)
{
    for (int i = 0; i < VOIDDISPLAY_EDID_SIZE; ++i) {
        out[i] = s_VoidEdidTemplate[i];
    }

    // Patch serial number (little-endian) so each monitor's EDID is distinct.
    out[12] = (UINT8)(serial & 0xFF);
    out[13] = (UINT8)((serial >> 8) & 0xFF);
    out[14] = (UINT8)((serial >> 16) & 0xFF);
    out[15] = (UINT8)((serial >> 24) & 0xFF);

    // Recompute the checksum so the whole block sums to 0 (mod 256).
    UINT32 sum = 0;
    for (int i = 0; i < VOIDDISPLAY_EDID_SIZE - 1; ++i) {
        sum += out[i];
    }
    out[VOIDDISPLAY_EDID_SIZE - 1] = (UINT8)((256 - (sum & 0xFF)) & 0xFF);
}

static bool BlockChecksumOk(const UINT8* block)
{
    UINT8 sum = 0;
    for (int i = 0; i < 128; ++i) {
        sum = (UINT8)(sum + block[i]);
    }
    return sum == 0;
}

UINT32 VoidLoadCustomEdid(UINT8 out[VOIDDISPLAY_EDID_MAX], UINT32 serial, bool patchSerial)
{
    HANDLE hf = CreateFileW(VOID_EDID_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        return 0;  // no custom EDID -> caller uses the built-in template
    }

    UINT8 buf[VOIDDISPLAY_EDID_MAX] = {};
    DWORD br = 0;
    BOOL ok = ReadFile(hf, buf, sizeof(buf), &br, nullptr);
    CloseHandle(hf);
    if (!ok || (br != 128 && br != 256)) {
        return 0;
    }

    // Header 00 FF FF FF FF FF FF 00 + base-block checksum.
    static const UINT8 kHdr[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    for (int i = 0; i < 8; ++i) {
        if (buf[i] != kHdr[i]) {
            return 0;
        }
    }
    if (!BlockChecksumOk(buf)) {
        return 0;
    }
    if (br == 256 && !BlockChecksumOk(buf + 128)) {
        return 0;  // bad extension-block checksum
    }

    if (patchSerial) {
        buf[12] = (UINT8)(serial & 0xFF);
        buf[13] = (UINT8)((serial >> 8) & 0xFF);
        buf[14] = (UINT8)((serial >> 16) & 0xFF);
        buf[15] = (UINT8)((serial >> 24) & 0xFF);
        UINT32 sum = 0;
        for (int i = 0; i < 127; ++i) {
            sum += buf[i];
        }
        buf[127] = (UINT8)((256 - (sum & 0xFF)) & 0xFF);
    }

    for (DWORD i = 0; i < br; ++i) {
        out[i] = buf[i];
    }
    return br;
}
