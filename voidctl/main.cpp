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
 */

#include "voidrv.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

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
        "  voidctl display setmode <index> <WxH@Hz>\n");
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
    std::printf("VoidDisplay interface version: %u\n", VoidrvDisplayVersion(h));
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
        std::printf("error: list failed\n");
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

int main(int argc, char** argv)
{
    if (argc < 3 || std::strcmp(argv[1], "display") != 0) {
        return Usage();
    }

    const char* cmd = argv[2];
    int    rest_argc = argc - 3;
    char** rest_argv = argv + 3;

    if (std::strcmp(cmd, "status") == 0)  return CmdStatus();
    if (std::strcmp(cmd, "version") == 0) return CmdVersion();
    if (std::strcmp(cmd, "list") == 0)    return CmdList();
    if (std::strcmp(cmd, "add") == 0)     return CmdAdd(rest_argc, rest_argv);
    if (std::strcmp(cmd, "remove") == 0)  return CmdRemove(rest_argc, rest_argv);
    if (std::strcmp(cmd, "setmode") == 0) return CmdSetMode(rest_argc, rest_argv);

    return Usage();
}
