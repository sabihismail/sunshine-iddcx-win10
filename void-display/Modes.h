/*
 * VoidDisplay - default mode table and DISPLAYCONFIG signal-info helpers.
 *
 * Needs only <windows.h> (DISPLAYCONFIG_* live in wingdi.h); the IddCx-specific
 * mode structures are filled in by the caller using these helpers.
 */

#pragma once

#include <windows.h>
#include "Public.h"   // VOIDDISPLAY_MAX_MODES and the wire structs

typedef struct _VOID_MODE_DESC {
    UINT32 Width;
    UINT32 Height;
    UINT32 RefreshHz;
} VOID_MODE_DESC;

/* Compiled-in set of modes advertised on every monitor. */
extern const VOID_MODE_DESC g_VoidDefaultModes[];
extern const unsigned       g_VoidDefaultModeCount;

/* Default mode used when an ADD request leaves the mode zeroed. */
#define VOID_DEFAULT_WIDTH      1920
#define VOID_DEFAULT_HEIGHT     1080
#define VOID_DEFAULT_REFRESH    60

/*
 * Build a DISPLAYCONFIG_VIDEO_SIGNAL_INFO for a (w, h, vsync) mode.
 *
 * vSyncFreqDivider rule (from IddCx): pass 0 for a monitor mode
 * (IDDCX_MONITOR_MODE), pass a non-zero value (1) for a target mode
 * (IDDCX_TARGET_MODE). The two paths must not share the same value.
 */
DISPLAYCONFIG_VIDEO_SIGNAL_INFO VoidCreateSignalInfo(UINT32 width, UINT32 height,
                                                     UINT32 vsync, UINT32 vSyncFreqDivider);

/* Index of (w, h, vsync) within g_VoidDefaultModes, or 0 if not present. */
unsigned VoidFindModeIndex(UINT32 width, UINT32 height, UINT32 vsync);

/*
 * Runtime advertised-mode store: the built-in defaults plus user-added custom
 * modes (managed via the control IOCTLs). Thread-safe; the IddCx mode callbacks
 * read it on arbitrary threads.
 */

/* Add a custom mode. No-op-success if it is already advertised. Returns false
 * only on bad input or when the custom-mode store is full. */
bool VoidModesAdd(UINT32 width, UINT32 height, UINT32 hz);

/* Remove a custom mode. Built-in defaults cannot be removed. Returns true if a
 * custom mode was removed. */
bool VoidModesRemove(UINT32 width, UINT32 height, UINT32 hz);

/* Copy the full advertised list (defaults first, then custom) into out (up to
 * cap entries). Returns the number written. */
unsigned VoidModesGet(VOID_MODE_DESC* out, unsigned cap);

/* Total advertised mode count (defaults + custom). */
unsigned VoidModesCount(void);

/* True if (w, h, hz) is already advertised (a built-in default or an added custom
 * mode). Used to decide whether a live monitor must re-plug to expose a new mode. */
bool VoidModesContains(UINT32 width, UINT32 height, UINT32 hz);

/* Seed the custom-mode store from the SDK-persisted list in the driver's WUDF
 * service Parameters key (REG_BINARY of packed {w, h, hz} triples). Call once at
 * adapter init, before the first monitor arrives. No-op if nothing is persisted. */
void VoidModesLoadPersisted(void);
