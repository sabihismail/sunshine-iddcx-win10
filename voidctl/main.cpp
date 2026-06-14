/*
 * voidctl - command-line control + smoke-test tool for the Void drivers.
 *
 * Usage:
 *   voidctl display status
 *   voidctl display version
 *   voidctl display list
 *   voidctl display add [WxH@Hz]
 *   voidctl display remove <index>
 *   voidctl display setmode <index> <WxH@Hz>
 *   voidctl mouse status | version
 *   voidctl mouse move <dx> <dy>
 *   voidctl mouse demo [seconds]
 */

#include "voidrv.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>

static void PrintLastError(const char* what)
{
    std::printf("error: %s (Win32 %lu)\n", what, GetLastError());
}

static int Usage()
{
    std::printf(
        "voidctl - Void driver control\n"
        "\n"
        "  voidctl display status\n"
        "  voidctl display version\n"
        "  voidctl display list\n"
        "  voidctl display add [WxH@Hz]      (e.g. add 1920x1080@60; omit for default)\n"
        "  voidctl display remove <index>\n"
        "  voidctl display setmode <index> <WxH@Hz>\n"
        "  voidctl display setdyn <index> <WxH@Hz>   (advertise on the fly; not persisted)\n"
        "  voidctl display modes             (list advertised modes)\n"
        "  voidctl display addmode <WxH@Hz>\n"
        "  voidctl display removemode <WxH@Hz>\n"
        "\n"
        "  voidctl mouse status\n"
        "  voidctl mouse version\n"
        "  voidctl mouse move <dx> <dy>      (one relative move)\n"
        "  voidctl mouse demo [seconds]      (trace a circle; default 5s)\n"
        "\n"
        "  voidctl kbd type <text...>        (type text into the focused window)\n"
        "  voidctl kbd tap <hidUsage>        (tap one HID usage, hex e.g. 0x04 = 'a')\n"
        "\n"
        "  voidctl pad demo [xbox|ds4|ds5] [s]   (animate a pad; rumble prints)\n"
        "  voidctl pad hold [xbox|ds4|ds5] [s]   (hold a fixed state; for joy.cpl/tests)\n"
        "\n"
        "  voidctl touch tap [x y]           (tap at a pixel; default screen center)\n"
        "  voidctl touch hold [x y] [s]      (hold a contact; for digitizer tests)\n"
        "  voidctl touch demo [s]            (two-finger animation)\n"
        "  voidctl pen demo [s]              (pen stroke with pressure + tilt)\n");
    return 1;
}

// Parse "1920x1080@60" or "1920x1080" (refresh defaults to 60).
static bool ParseMode(const char* s, VoidrvDisplayMode* m)
{
    unsigned w = 0, h = 0, hz = 60;
    int n = sscanf_s(s, "%ux%u@%u", &w, &h, &hz);
    if (n < 2 || w == 0 || h == 0) {
        return false;
    }
    m->Width = w;
    m->Height = h;
    m->RefreshHz = hz;
    return true;
}

static int CmdStatus()
{
    VoidrvStatus s = VoidrvDisplayQueryStatus();
    const char* text =
        s == VOIDRV_STATUS_OK            ? "ok (present)" :
        s == VOIDRV_STATUS_NOT_INSTALLED ? "not installed" :
                                           "inaccessible";
    std::printf("VoidDisplay: %s\n", text);
    return s == VOIDRV_STATUS_OK ? 0 : 1;
}

static int CmdVersion()
{
    VoidrvDisplayHandle h = VoidrvDisplayOpen();
    if (!h) {
        std::printf("error: cannot open VoidDisplay (is it installed?)\n");
        return 1;
    }
    SetLastError(0);
    uint32_t ver = VoidrvDisplayVersion(h);
    if (ver == 0) {
        PrintLastError("version query failed/returned 0");
    } else {
        std::printf("VoidDisplay interface version: %u\n", ver);
    }
    VoidrvDisplayClose(h);
    return 0;
}

static int CmdList()
{
    VoidrvDisplayHandle h = VoidrvDisplayOpen();
    if (!h) {
        std::printf("error: cannot open VoidDisplay\n");
        return 1;
    }
    VoidrvDisplayState st;
    std::memset(&st, 0, sizeof(st));
    if (!VoidrvDisplayList(h, &st)) {
        PrintLastError("list failed");
        VoidrvDisplayClose(h);
        return 1;
    }
    std::printf("displays in use: %u\n", st.Count);
    for (uint32_t i = 0; i < VOIDRV_MAX_DISPLAYS; ++i) {
        if (st.Entries[i].InUse) {
            std::printf("  [%u] %ux%u@%u\n", i,
                        st.Entries[i].Mode.Width, st.Entries[i].Mode.Height,
                        st.Entries[i].Mode.RefreshHz);
        }
    }
    VoidrvDisplayClose(h);
    return 0;
}

static int CmdAdd(int argc, char** argv)
{
    VoidrvDisplayMode mode;
    std::memset(&mode, 0, sizeof(mode));
    bool haveMode = false;
    if (argc > 0) {
        if (!ParseMode(argv[0], &mode)) {
            std::printf("error: bad mode '%s' (expected WxH@Hz)\n", argv[0]);
            return 1;
        }
        haveMode = true;
    }
    VoidrvDisplayHandle h = VoidrvDisplayOpen();
    if (!h) {
        std::printf("error: cannot open VoidDisplay\n");
        return 1;
    }
    int idx = VoidrvDisplayAdd(h, haveMode ? &mode : nullptr);
    if (idx < 0) {
        std::printf("error: add failed\n");
        VoidrvDisplayClose(h);
        return 1;
    }
    std::printf("added display at index %d\n", idx);
    VoidrvDisplayClose(h);
    return 0;
}

static int CmdRemove(int argc, char** argv)
{
    if (argc < 1) {
        std::printf("error: remove needs <index>\n");
        return 1;
    }
    uint32_t index = (uint32_t)std::strtoul(argv[0], nullptr, 10);
    VoidrvDisplayHandle h = VoidrvDisplayOpen();
    if (!h) {
        std::printf("error: cannot open VoidDisplay\n");
        return 1;
    }
    bool ok = VoidrvDisplayRemove(h, index);
    std::printf(ok ? "removed display %u\n" : "error: remove failed\n", index);
    VoidrvDisplayClose(h);
    return ok ? 0 : 1;
}

static int CmdSetMode(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("error: setmode needs <index> <WxH@Hz>\n");
        return 1;
    }
    uint32_t index = (uint32_t)std::strtoul(argv[0], nullptr, 10);
    VoidrvDisplayMode mode;
    std::memset(&mode, 0, sizeof(mode));
    if (!ParseMode(argv[1], &mode)) {
        std::printf("error: bad mode '%s'\n", argv[1]);
        return 1;
    }
    VoidrvDisplayHandle h = VoidrvDisplayOpen();
    if (!h) {
        std::printf("error: cannot open VoidDisplay\n");
        return 1;
    }
    bool ok = VoidrvDisplaySetMode(h, index, &mode);
    std::printf(ok ? "set mode on display %u\n" : "error: setmode failed\n", index);
    VoidrvDisplayClose(h);
    return ok ? 0 : 1;
}

static int CmdSetModeDynamic(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("error: setdyn needs <index> <WxH@Hz>\n");
        return 1;
    }
    uint32_t index = (uint32_t)std::strtoul(argv[0], nullptr, 10);
    VoidrvDisplayMode mode;
    std::memset(&mode, 0, sizeof(mode));
    if (!ParseMode(argv[1], &mode)) {
        std::printf("error: bad mode '%s'\n", argv[1]);
        return 1;
    }
    VoidrvDisplayHandle h = VoidrvDisplayOpen();
    if (!h) {
        std::printf("error: cannot open VoidDisplay\n");
        return 1;
    }
    bool ok = VoidrvDisplaySetModeDynamic(h, index, &mode);
    std::printf(ok ? "set dynamic mode on display %u\n" : "error: setdyn failed\n", index);
    VoidrvDisplayClose(h);
    return ok ? 0 : 1;
}

static int CmdModes()
{
    VoidrvDisplayHandle h = VoidrvDisplayOpen();
    if (!h) {
        std::printf("error: cannot open VoidDisplay\n");
        return 1;
    }
    VoidrvModeList list;
    std::memset(&list, 0, sizeof(list));
    if (!VoidrvDisplayListModes(h, &list)) {
        PrintLastError("list modes failed");
        VoidrvDisplayClose(h);
        return 1;
    }
    std::printf("advertised modes: %u\n", list.Count);
    for (uint32_t i = 0; i < list.Count && i < VOIDRV_MAX_MODES; ++i) {
        std::printf("  %ux%u@%u\n", list.Modes[i].Width, list.Modes[i].Height, list.Modes[i].RefreshHz);
    }
    VoidrvDisplayClose(h);
    return 0;
}

static int CmdAddRemoveMode(int argc, char** argv, bool add)
{
    if (argc < 1) {
        std::printf("error: %smode needs <WxH@Hz>\n", add ? "add" : "remove");
        return 1;
    }
    VoidrvDisplayMode mode;
    std::memset(&mode, 0, sizeof(mode));
    if (!ParseMode(argv[0], &mode)) {
        std::printf("error: bad mode '%s'\n", argv[0]);
        return 1;
    }
    VoidrvDisplayHandle h = VoidrvDisplayOpen();
    if (!h) {
        std::printf("error: cannot open VoidDisplay\n");
        return 1;
    }
    bool ok = add ? VoidrvDisplayAddMode(h, &mode) : VoidrvDisplayRemoveMode(h, &mode);
    std::printf("%s mode %ux%u@%u: %s\n", add ? "add" : "remove",
                mode.Width, mode.Height, mode.RefreshHz, ok ? "ok" : "failed");
    VoidrvDisplayClose(h);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// mouse
// ---------------------------------------------------------------------------
static int CmdMouseStatus()
{
    VoidrvStatus s = VoidrvInputQueryStatus();
    const char* text =
        s == VOIDRV_STATUS_OK            ? "ok (present)" :
        s == VOIDRV_STATUS_NOT_INSTALLED ? "not installed" :
                                           "inaccessible";
    std::printf("VoidInput: %s\n", text);
    return s == VOIDRV_STATUS_OK ? 0 : 1;
}

static int CmdMouseVersion()
{
    uint32_t ver = VoidrvInputVersion();
    if (ver == 0) {
        std::printf("error: VoidInput version query failed (is it installed?)\n");
        return 1;
    }
    std::printf("VoidInput interface version: %u\n", ver);
    return 0;
}

static int CmdMouseMove(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("error: mouse move needs <dx> <dy>\n");
        return 1;
    }
    int dx = (int)std::strtol(argv[0], nullptr, 10);
    int dy = (int)std::strtol(argv[1], nullptr, 10);

    VoidrvInputHandle h = VoidrvInputCreate(VOIDRV_INPUT_MOUSE);
    if (!h) {
        std::printf("error: cannot create mouse (is VoidInput installed?)\n");
        return 1;
    }
    bool ok = VoidrvInputMouseMove(h, true, dx, dy);
    Sleep(50);   // let the report deliver before the device unplugs on close
    VoidrvInputClose(h);
    std::printf(ok ? "moved by (%d, %d)\n" : "error: move failed\n", dx, dy);
    return ok ? 0 : 1;
}

static int CmdMouseDemo(int argc, char** argv)
{
    int seconds = argc > 0 ? (int)std::strtol(argv[0], nullptr, 10) : 5;
    if (seconds <= 0) {
        seconds = 5;
    }

    VoidrvInputHandle h = VoidrvInputCreate(VOIDRV_INPUT_MOUSE);
    if (!h) {
        std::printf("error: cannot create mouse (is VoidInput installed?)\n");
        return 1;
    }
    std::printf("mouse created; tracing a circle for %d s\n", seconds);

    const int hz    = 60;
    const int ticks = seconds * hz;
    for (int i = 0; i < ticks; ++i) {
        double th = (double)i / hz * 3.14159265358979;   // ~0.5 revolution/sec
        int32_t x = (int32_t)(16384.0 + 6000.0 * std::cos(th));
        int32_t y = (int32_t)(16384.0 + 6000.0 * std::sin(th));
        VoidrvInputMouseMove(h, false, x, y);
        Sleep(1000 / hz);
    }
    VoidrvInputClose(h);
    std::printf("done\n");
    return 0;
}

// ---------------------------------------------------------------------------
// keyboard
// ---------------------------------------------------------------------------

// Minimal ASCII -> HID Keyboard/Keypad usage (+ shift). Enough for a smoke test;
// not a full layout. Returns false for unmapped characters.
static bool AsciiToHid(char c, uint8_t* usage, bool* shift)
{
    *shift = false;
    if (c >= 'a' && c <= 'z') { *usage = (uint8_t)(0x04 + (c - 'a')); return true; }
    if (c >= 'A' && c <= 'Z') { *usage = (uint8_t)(0x04 + (c - 'A')); *shift = true; return true; }
    if (c >= '1' && c <= '9') { *usage = (uint8_t)(0x1E + (c - '1')); return true; }
    switch (c) {
        case '0':  *usage = 0x27; return true;
        case ' ':  *usage = 0x2C; return true;
        case '\n': *usage = 0x28; return true;
        case '\t': *usage = 0x2B; return true;
        case '-':  *usage = 0x2D; return true;
        case '=':  *usage = 0x2E; return true;
        case '.':  *usage = 0x37; return true;
        case ',':  *usage = 0x36; return true;
        case '/':  *usage = 0x38; return true;
        case ';':  *usage = 0x33; return true;
        case '\'': *usage = 0x34; return true;
        case '!':  *usage = 0x1E; *shift = true; return true;
        case '@':  *usage = 0x1F; *shift = true; return true;
        case '#':  *usage = 0x20; *shift = true; return true;
        case '$':  *usage = 0x21; *shift = true; return true;
        case '%':  *usage = 0x22; *shift = true; return true;
        case '?':  *usage = 0x38; *shift = true; return true;
        case ':':  *usage = 0x33; *shift = true; return true;
        case '_':  *usage = 0x2D; *shift = true; return true;
        case '+':  *usage = 0x2E; *shift = true; return true;
    }
    return false;
}

static int CmdKbdType(int argc, char** argv)
{
    if (argc < 1) {
        std::printf("error: kbd type needs <text>\n");
        return 1;
    }
    std::string text;
    for (int i = 0; i < argc; ++i) {
        if (i) text += ' ';
        text += argv[i];
    }

    VoidrvInputHandle h = VoidrvInputCreate(VOIDRV_INPUT_KEYBOARD);
    if (!h) {
        std::printf("error: cannot create keyboard (is VoidInput installed?)\n");
        return 1;
    }

    for (char c : text) {
        uint8_t usage = 0;
        bool    shift = false;
        if (!AsciiToHid(c, &usage, &shift)) {
            continue;
        }
        if (shift) VoidrvInputKey(h, 0xE1, true);    // Left Shift
        VoidrvInputKey(h, usage, true);
        Sleep(8);
        VoidrvInputKey(h, usage, false);
        if (shift) VoidrvInputKey(h, 0xE1, false);
        Sleep(8);
    }
    VoidrvInputReset(h);
    Sleep(30);   // let the final reports deliver before the device unplugs on close
    VoidrvInputClose(h);
    std::printf("typed %d chars\n", (int)text.size());
    return 0;
}

static int CmdKbdTap(int argc, char** argv)
{
    if (argc < 1) {
        std::printf("error: kbd tap needs <hidUsage> (hex, e.g. 0x04 = 'a')\n");
        return 1;
    }
    unsigned usage = (unsigned)std::strtoul(argv[0], nullptr, 16);

    VoidrvInputHandle h = VoidrvInputCreate(VOIDRV_INPUT_KEYBOARD);
    if (!h) {
        std::printf("error: cannot create keyboard\n");
        return 1;
    }
    VoidrvInputKey(h, (uint16_t)usage, true);
    Sleep(20);
    VoidrvInputKey(h, (uint16_t)usage, false);
    Sleep(20);
    VoidrvInputClose(h);
    std::printf("tapped usage 0x%02X\n", usage);
    return 0;
}

// ---------------------------------------------------------------------------
// gamepad
// ---------------------------------------------------------------------------
static void PadRumbleCb(void*, uint8_t low, uint8_t high, uint8_t lt, uint8_t rt)
{
    std::printf("  rumble: low=%u high=%u ltrig=%u rtrig=%u\n", low, high, lt, rt);
}

// Optional leading "xbox" / "ds4" / "ds5" token selects the pad type (default Xbox).
static VoidrvInputType ParsePadType(int* argc, char*** argv)
{
    VoidrvInputType type = VOIDRV_INPUT_XBOXONE;
    if (*argc > 0) {
        if (std::strcmp((*argv)[0], "ds4") == 0) {
            type = VOIDRV_INPUT_DS4; ++(*argv); --(*argc);
        } else if (std::strcmp((*argv)[0], "ds5") == 0) {
            type = VOIDRV_INPUT_DS5; ++(*argv); --(*argc);
        } else if (std::strcmp((*argv)[0], "xbox") == 0 || std::strcmp((*argv)[0], "xboxone") == 0) {
            ++(*argv); --(*argc);
        }
    }
    return type;
}

// Short pad-type label for status lines.
static const char* PadTypeName(VoidrvInputType type)
{
    if (type == VOIDRV_INPUT_DS5) return "ds5";
    if (type == VOIDRV_INPUT_DS4) return "ds4";
    return "xbox";
}

static int CmdPadDemo(int argc, char** argv)
{
    VoidrvInputType type = ParsePadType(&argc, &argv);
    int seconds = argc > 0 ? (int)std::strtol(argv[0], nullptr, 10) : 5;
    if (seconds <= 0) {
        seconds = 5;
    }
    VoidrvInputHandle h = VoidrvInputCreate(type);
    if (!h) {
        std::printf("error: cannot create gamepad (is VoidInput installed?)\n");
        return 1;
    }
    VoidrvInputPadSetRumbleCallback(h, PadRumbleCb, nullptr);
    std::printf("%s pad created; animating for %d s (open joy.cpl to watch; rumble prints)\n",
                PadTypeName(type), seconds);

    const int hz = 60;
    const int ticks = seconds * hz;
    const uint16_t face[] = { VOIDRV_PAD_A, VOIDRV_PAD_B, VOIDRV_PAD_X, VOIDRV_PAD_Y };
    for (int i = 0; i < ticks; ++i) {
        double th = (double)i / hz * 3.14159265358979;
        VoidrvPadState st;
        std::memset(&st, 0, sizeof(st));
        st.ThumbLX      = (int16_t)(28000.0 * std::cos(th));
        st.ThumbLY      = (int16_t)(28000.0 * std::sin(th));
        st.Buttons      = face[(i / hz) % 4];          // change face button each second
        st.LeftTrigger  = (uint8_t)((i * 4) & 0xFF);
        VoidrvInputPadReport(h, &st);
        Sleep(1000 / hz);
    }
    VoidrvPadState neutral;
    std::memset(&neutral, 0, sizeof(neutral));
    VoidrvInputPadReport(h, &neutral);
    Sleep(30);
    VoidrvInputClose(h);
    std::printf("done\n");
    return 0;
}

static int CmdPadHold(int argc, char** argv)
{
    VoidrvInputType type = ParsePadType(&argc, &argv);
    int seconds = argc > 0 ? (int)std::strtol(argv[0], nullptr, 10) : 4;
    if (seconds <= 0) {
        seconds = 4;
    }
    VoidrvInputHandle h = VoidrvInputCreate(type);
    if (!h) {
        std::printf("error: cannot create gamepad\n");
        return 1;
    }
    // A known, checkable state: left stick full right + up, A and B held.
    VoidrvPadState st;
    std::memset(&st, 0, sizeof(st));
    st.ThumbLX     = 32767;
    st.ThumbLY     = 32767;
    st.Buttons     = VOIDRV_PAD_A | VOIDRV_PAD_B;
    st.LeftTrigger = 255;

    std::printf("holding pad state (LX+ LY+ A+B) for %d s\n", seconds);
    for (int i = 0; i < seconds * 10; ++i) {
        VoidrvInputPadReport(h, &st);
        Sleep(100);
    }
    VoidrvInputClose(h);
    std::printf("done\n");
    return 0;
}

// ---- touch + pen ----

static int CmdTouchTap(int argc, char** argv)
{
    int x = argc > 0 ? (int)std::strtol(argv[0], nullptr, 10) : (GetSystemMetrics(SM_CXSCREEN) / 2);
    int y = argc > 1 ? (int)std::strtol(argv[1], nullptr, 10) : (GetSystemMetrics(SM_CYSCREEN) / 2);
    VoidrvInputHandle h = VoidrvInputCreate(VOIDRV_INPUT_TOUCH);
    if (!h) {
        std::printf("error: cannot create touch digitizer (is VoidInput installed?)\n");
        return 1;
    }
    std::printf("tap at (%d,%d)\n", x, y);
    VoidrvInputTouchContact(h, 0, VOIDRV_TOUCH_DOWN, x, y, 512, 8);
    Sleep(60);
    VoidrvInputTouchContact(h, 0, VOIDRV_TOUCH_UP, x, y, 0, 8);
    Sleep(30);
    VoidrvInputClose(h);
    std::printf("done\n");
    return 0;
}

static int CmdTouchHold(int argc, char** argv)
{
    int x = argc > 0 ? (int)std::strtol(argv[0], nullptr, 10) : (GetSystemMetrics(SM_CXSCREEN) / 2);
    int y = argc > 1 ? (int)std::strtol(argv[1], nullptr, 10) : (GetSystemMetrics(SM_CYSCREEN) / 2);
    int seconds = argc > 2 ? (int)std::strtol(argv[2], nullptr, 10) : 4;
    if (seconds <= 0) {
        seconds = 4;
    }
    VoidrvInputHandle h = VoidrvInputCreate(VOIDRV_INPUT_TOUCH);
    if (!h) {
        std::printf("error: cannot create touch digitizer\n");
        return 1;
    }
    std::printf("holding touch contact at (%d,%d) for %d s\n", x, y, seconds);
    VoidrvInputTouchContact(h, 0, VOIDRV_TOUCH_DOWN, x, y, 512, 8);
    for (int i = 0; i < seconds * 10; ++i) {
        VoidrvInputTouchContact(h, 0, VOIDRV_TOUCH_MOVE, x, y, 512, 8);
        Sleep(100);
    }
    VoidrvInputTouchContact(h, 0, VOIDRV_TOUCH_UP, x, y, 0, 8);
    Sleep(30);
    VoidrvInputClose(h);
    std::printf("done\n");
    return 0;
}

static int CmdTouchDemo(int argc, char** argv)
{
    int seconds = argc > 0 ? (int)std::strtol(argv[0], nullptr, 10) : 5;
    if (seconds <= 0) {
        seconds = 5;
    }
    int cx = GetSystemMetrics(SM_CXSCREEN) / 2;
    int cy = GetSystemMetrics(SM_CYSCREEN) / 2;
    VoidrvInputHandle h = VoidrvInputCreate(VOIDRV_INPUT_TOUCH);
    if (!h) {
        std::printf("error: cannot create touch digitizer\n");
        return 1;
    }
    std::printf("two-finger demo around (%d,%d) for %d s\n", cx, cy, seconds);
    const int hz = 60;
    const int ticks = seconds * hz;
    for (int i = 0; i < ticks; ++i) {
        double th = (double)i / hz * 3.14159265358979;
        int r = 200;
        int x0 = cx + (int)(r * std::cos(th)), y0 = cy + (int)(r * std::sin(th));
        int x1 = cx - (int)(r * std::cos(th)), y1 = cy - (int)(r * std::sin(th));
        uint8_t act = (i == 0) ? VOIDRV_TOUCH_DOWN : VOIDRV_TOUCH_MOVE;
        VoidrvInputTouchContact(h, 0, act, x0, y0, 512, 8);
        VoidrvInputTouchContact(h, 1, act, x1, y1, 512, 8);
        Sleep(1000 / hz);
    }
    VoidrvInputTouchCancelAll(h);
    Sleep(30);
    VoidrvInputClose(h);
    std::printf("done\n");
    return 0;
}

static int CmdPenDemo(int argc, char** argv)
{
    int seconds = argc > 0 ? (int)std::strtol(argv[0], nullptr, 10) : 4;
    if (seconds <= 0) {
        seconds = 4;
    }
    int x0 = GetSystemMetrics(SM_CXSCREEN) / 4;
    int y0 = GetSystemMetrics(SM_CYSCREEN) / 2;
    int span = GetSystemMetrics(SM_CXSCREEN) / 2;
    VoidrvInputHandle h = VoidrvInputCreate(VOIDRV_INPUT_TOUCH);
    if (!h) {
        std::printf("error: cannot create digitizer\n");
        return 1;
    }
    std::printf("pen stroke for %d s (pressure ramps, fixed tilt)\n", seconds);
    const int hz = 60;
    const int ticks = seconds * hz;
    VoidrvInputPen(h, VOIDRV_TOUCH_HOVER, x0, y0, 0, 0, 0, false, false);
    for (int i = 0; i < ticks; ++i) {
        int x = x0 + (span * i / ticks);
        int y = y0 + (int)(80.0 * std::sin((double)i / hz * 6.28318530717959));
        uint16_t pr = (uint16_t)(100 + (900 * i / ticks));
        uint8_t act = (i == 0) ? VOIDRV_TOUCH_DOWN : VOIDRV_TOUCH_MOVE;
        VoidrvInputPen(h, act, x, y, pr, (int8_t)15, (int8_t)-10, false, false);
        Sleep(1000 / hz);
    }
    VoidrvInputPen(h, VOIDRV_TOUCH_UP,     x0 + span, y0, 0, 0, 0, false, false);
    VoidrvInputPen(h, VOIDRV_TOUCH_CANCEL, x0 + span, y0, 0, 0, 0, false, false);
    Sleep(30);
    VoidrvInputClose(h);
    std::printf("done\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        return Usage();
    }

    const char* group = argv[1];
    const char* cmd = argv[2];
    int    rest_argc = argc - 3;
    char** rest_argv = argv + 3;

    if (std::strcmp(group, "mouse") == 0) {
        if      (std::strcmp(cmd, "status")  == 0) return CmdMouseStatus();
        else if (std::strcmp(cmd, "version") == 0) return CmdMouseVersion();
        else if (std::strcmp(cmd, "move")    == 0) return CmdMouseMove(rest_argc, rest_argv);
        else if (std::strcmp(cmd, "demo")    == 0) return CmdMouseDemo(rest_argc, rest_argv);
        return Usage();
    }

    if (std::strcmp(group, "kbd") == 0) {
        if      (std::strcmp(cmd, "status")  == 0) return CmdMouseStatus();   // device-wide
        else if (std::strcmp(cmd, "version") == 0) return CmdMouseVersion();
        else if (std::strcmp(cmd, "type")    == 0) return CmdKbdType(rest_argc, rest_argv);
        else if (std::strcmp(cmd, "tap")     == 0) return CmdKbdTap(rest_argc, rest_argv);
        return Usage();
    }

    if (std::strcmp(group, "pad") == 0) {
        if      (std::strcmp(cmd, "status")  == 0) return CmdMouseStatus();   // device-wide
        else if (std::strcmp(cmd, "version") == 0) return CmdMouseVersion();
        else if (std::strcmp(cmd, "demo")    == 0) return CmdPadDemo(rest_argc, rest_argv);
        else if (std::strcmp(cmd, "hold")    == 0) return CmdPadHold(rest_argc, rest_argv);
        return Usage();
    }

    if (std::strcmp(group, "touch") == 0) {
        if      (std::strcmp(cmd, "status")  == 0) return CmdMouseStatus();   // device-wide
        else if (std::strcmp(cmd, "version") == 0) return CmdMouseVersion();
        else if (std::strcmp(cmd, "tap")     == 0) return CmdTouchTap(rest_argc, rest_argv);
        else if (std::strcmp(cmd, "hold")    == 0) return CmdTouchHold(rest_argc, rest_argv);
        else if (std::strcmp(cmd, "demo")    == 0) return CmdTouchDemo(rest_argc, rest_argv);
        return Usage();
    }

    if (std::strcmp(group, "pen") == 0) {
        if      (std::strcmp(cmd, "demo") == 0) return CmdPenDemo(rest_argc, rest_argv);
        return Usage();
    }

    if (std::strcmp(group, "display") != 0) {
        return Usage();
    }

    int  rc       = 0;
    bool persists = false;   // does this command change saved state?

    if (std::strcmp(cmd, "status") == 0)          rc = CmdStatus();
    else if (std::strcmp(cmd, "version") == 0)    rc = CmdVersion();
    else if (std::strcmp(cmd, "list") == 0)       rc = CmdList();
    else if (std::strcmp(cmd, "add") == 0)        { rc = CmdAdd(rest_argc, rest_argv); persists = true; }
    else if (std::strcmp(cmd, "remove") == 0)     { rc = CmdRemove(rest_argc, rest_argv); persists = true; }
    else if (std::strcmp(cmd, "setmode") == 0)    { rc = CmdSetMode(rest_argc, rest_argv); persists = true; }
    else if (std::strcmp(cmd, "setdyn") == 0)     rc = CmdSetModeDynamic(rest_argc, rest_argv);
    else if (std::strcmp(cmd, "modes") == 0)      rc = CmdModes();
    else if (std::strcmp(cmd, "addmode") == 0)    { rc = CmdAddRemoveMode(rest_argc, rest_argv, true); persists = true; }
    else if (std::strcmp(cmd, "removemode") == 0) { rc = CmdAddRemoveMode(rest_argc, rest_argv, false); persists = true; }
    else return Usage();

    // The live operation went through the driver, but saving it for restore-on-start
    // writes HKLM and needs elevation. Warn so a "successful" change that silently
    // won't survive a restart is not a surprise.
    if (rc == 0 && persists && !VoidrvDisplayPersistenceWritable()) {
        std::printf(
            "  warning: not elevated - this change is live but will NOT survive an adapter\n"
            "           restart or reboot. Run voidctl as administrator to persist it.\n");
    }
    return rc;
}
