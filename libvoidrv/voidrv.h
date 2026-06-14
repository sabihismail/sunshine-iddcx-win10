/*
 * libvoidrv - user-mode SDK for the Void drivers.
 *
 * Public, self-contained C ABI. A host app links libvoidrv and talks to the
 * drivers through these functions; it never builds raw IOCTL buffers. C++ is
 * fine too - everything is declared extern "C".
 *
 * This header intentionally does not expose the driver wire format (that lives
 * in the driver's Public.h); the SDK ABI is decoupled from it.
 */

#ifndef VOIDRV_H
#define VOIDRV_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOIDRV_MAX_DISPLAYS 8
#define VOIDRV_MAX_MODES    128

typedef struct VoidrvDisplayMode {
    uint32_t Width;      /* pixels; 0 in an Add request selects the driver default */
    uint32_t Height;     /* pixels */
    uint32_t RefreshHz;  /* nominal refresh, e.g. 60, 120, 144, 240 */
} VoidrvDisplayMode;

typedef struct VoidrvModeList {
    uint32_t          Count;
    VoidrvDisplayMode Modes[VOIDRV_MAX_MODES];
} VoidrvModeList;

typedef struct VoidrvDisplayEntry {
    uint32_t          InUse;
    VoidrvDisplayMode Mode;
} VoidrvDisplayEntry;

typedef struct VoidrvDisplayState {
    uint32_t           Count;
    VoidrvDisplayEntry Entries[VOIDRV_MAX_DISPLAYS];
} VoidrvDisplayState;

typedef enum VoidrvStatus {
    VOIDRV_STATUS_OK = 0,        /* present and openable */
    VOIDRV_STATUS_NOT_INSTALLED, /* device interface not found */
    VOIDRV_STATUS_INACCESSIBLE   /* found but could not be opened */
} VoidrvStatus;

/* Opaque control-channel handle. */
typedef struct VoidrvDisplay* VoidrvDisplayHandle;

/* Is the VoidDisplay control interface present? */
VoidrvStatus        VoidrvDisplayQueryStatus(void);

/* Open / close the control channel. Open returns NULL on failure (GetLastError). */
VoidrvDisplayHandle VoidrvDisplayOpen(void);
void                VoidrvDisplayClose(VoidrvDisplayHandle handle);

/* Driver interface version, or 0 on failure. */
uint32_t            VoidrvDisplayVersion(VoidrvDisplayHandle handle);

/* Add a display. Pass NULL or a zeroed mode for the driver default. */
/* Returns the new slot index (>= 0), or -1 on failure. */
int                 VoidrvDisplayAdd(VoidrvDisplayHandle handle, const VoidrvDisplayMode* mode);

/* Remove a display by slot index. */
bool                VoidrvDisplayRemove(VoidrvDisplayHandle handle, uint32_t index);

/* Change a display's mode. */
bool                VoidrvDisplaySetMode(VoidrvDisplayHandle handle, uint32_t index,
                                         const VoidrvDisplayMode* mode);

/* Set a display's mode, advertising it on the fly if not already in the list (for
   on-resize / arbitrary resolutions, like a VM resizing its screen). Width/height
   are rounded to even and clamped to <= 4096x2160 / 24..165 Hz. Unlike SetMode this
   is ephemeral - it is NOT persisted across an adapter restart. */
bool                VoidrvDisplaySetModeDynamic(VoidrvDisplayHandle handle, uint32_t index,
                                                const VoidrvDisplayMode* mode);

/* Read the current display table. */
bool                VoidrvDisplayList(VoidrvDisplayHandle handle, VoidrvDisplayState* state);

/* Add a custom mode to the advertised list (visible in Windows display settings).
   Built-in default modes are always advertised. Live displays re-plug to pick it up. */
bool                VoidrvDisplayAddMode(VoidrvDisplayHandle handle, const VoidrvDisplayMode* mode);

/* Remove a previously added custom mode (defaults cannot be removed). */
bool                VoidrvDisplayRemoveMode(VoidrvDisplayHandle handle, const VoidrvDisplayMode* mode);

/* Read the full advertised mode list (defaults + custom). */
bool                VoidrvDisplayListModes(VoidrvDisplayHandle handle, VoidrvModeList* list);

/* Whether the SDK can persist changes so they survive an adapter restart / reboot.
   Persistence writes HKLM and needs elevation. When this is false, Add/Remove/
   SetMode/AddMode/RemoveMode still take effect for the current session but are NOT
   saved (the driver reads the saved set at init; an unelevated caller cannot write
   it). A host app can call this to decide whether to elevate. */
bool                VoidrvDisplayPersistenceWritable(void);

/* ===================== VoidInput ===================================== */

/* Virtual input device types (mirrors the driver's type enum). */
typedef enum VoidrvInputType {
    VOIDRV_INPUT_MOUSE    = 1,  /* relative + absolute pointer */
    VOIDRV_INPUT_KEYBOARD = 2,
    VOIDRV_INPUT_XBOXONE  = 3,
    VOIDRV_INPUT_DS4      = 4,
    VOIDRV_INPUT_DS5      = 5,
    VOIDRV_INPUT_TOUCH    = 6,
} VoidrvInputType;

/* Mouse button bitmask. */
#define VOIDRV_MB_LEFT    0x01
#define VOIDRV_MB_RIGHT   0x02
#define VOIDRV_MB_MIDDLE  0x04
#define VOIDRV_MB_X1      0x08
#define VOIDRV_MB_X2      0x10

/* Keyboard modifier bitmask (matches HID usages 0xE0..0xE7). */
#define VOIDRV_KMOD_LCTRL   0x01
#define VOIDRV_KMOD_LSHIFT  0x02
#define VOIDRV_KMOD_LALT    0x04
#define VOIDRV_KMOD_LGUI    0x08
#define VOIDRV_KMOD_RCTRL   0x10
#define VOIDRV_KMOD_RSHIFT  0x20
#define VOIDRV_KMOD_RALT    0x40
#define VOIDRV_KMOD_RGUI    0x80

/* Opaque per-device handle. Each handle owns exactly one virtual device; the
   device exists until the handle is closed. */
typedef struct VoidrvInput* VoidrvInputHandle;

/* Is the VoidInput control interface present? */
VoidrvStatus      VoidrvInputQueryStatus(void);

/* Driver interface version, or 0 on failure. Opens and closes its own handle. */
uint32_t          VoidrvInputVersion(void);

/* Create a virtual input device of the given type. Returns NULL on failure
   (GetLastError). Close the handle to remove the device. */
VoidrvInputHandle VoidrvInputCreate(VoidrvInputType type);

/* Remove the device and close its handle. */
void              VoidrvInputClose(VoidrvInputHandle handle);

/* ---- Raw report tier: submit one full-state HID report (no held state). ---- */

/* Mouse relative report: buttons bitmask + signed deltas + signed wheel detents. */
bool              VoidrvInputMouseReportRelative(VoidrvInputHandle handle, uint8_t buttons,
                                                 int16_t dx, int16_t dy,
                                                 int8_t wheel, int8_t hwheel);

/* Mouse absolute report: buttons + normalized 0..32767 X/Y + signed wheel detents. */
bool              VoidrvInputMouseReportAbsolute(VoidrvInputHandle handle, uint8_t buttons,
                                                 uint16_t x, uint16_t y,
                                                 int8_t wheel, int8_t hwheel);

/* Keyboard report: modifier bitmask (VOIDRV_KMOD_*) + up to 6 HID key usages
   (0 = empty slot). Bypasses the stateful key tracking below. */
bool              VoidrvInputKeyboardReport(VoidrvInputHandle handle, uint8_t modifiers,
                                            const uint8_t keys[6]);

/* Consumer-control report: one 16-bit HID consumer usage (0 = release). */
bool              VoidrvInputConsumerReport(VoidrvInputHandle handle, uint16_t usage);

/* ---- Stateful event tier: the SDK remembers held buttons / keys and re-emits
        the full report, so callers can feed a raw input event stream. ---- */

/* Mouse motion. relative: x/y are signed deltas; absolute: x/y are 0..32767.
   Re-emits with the currently-held buttons. */
bool              VoidrvInputMouseMove(VoidrvInputHandle handle, bool relative,
                                       int32_t x, int32_t y);

/* Mouse absolute motion in desktop pixels, mapped to 0..32767. Defaults to the
   primary display (see VoidrvInputMouseSetBounds). */
bool              VoidrvInputMouseMoveAbsolutePixels(VoidrvInputHandle handle,
                                                     int32_t px, int32_t py);

/* Pixel rectangle that VoidrvInputMouseMoveAbsolutePixels maps within.
   width<=0 restores the default (primary display). */
bool              VoidrvInputMouseSetBounds(VoidrvInputHandle handle, int32_t left,
                                            int32_t top, int32_t width, int32_t height);

/* Mouse button press/release (VOIDRV_MB_*); re-emits with held buttons. */
bool              VoidrvInputMouseButton(VoidrvInputHandle handle, uint8_t button, bool down);

/* Mouse wheel: signed vertical + horizontal detents; re-emits with held buttons. */
bool              VoidrvInputMouseWheel(VoidrvInputHandle handle, int8_t dv, int8_t dh);

/* Keyboard key press/release by HID usage (incl. modifiers 0xE0..0xE7); the SDK
   maintains the modifier bitmask + 6-key rollover and re-emits the report. */
bool              VoidrvInputKey(VoidrvInputHandle handle, uint16_t hidUsage, bool down);

/* Consumer-control key by HID usage (0 to release). */
bool              VoidrvInputConsumer(VoidrvInputHandle handle, uint16_t usage);

/* Release all held buttons / keys and emit a cleared report. */
bool              VoidrvInputReset(VoidrvInputHandle handle);

/* ---- Gamepad (Xbox One) ---- */

/* Pad button bitmask (layout matches XInput's wButtons for familiarity). */
#define VOIDRV_PAD_DPAD_UP    0x0001
#define VOIDRV_PAD_DPAD_DOWN  0x0002
#define VOIDRV_PAD_DPAD_LEFT  0x0004
#define VOIDRV_PAD_DPAD_RIGHT 0x0008
#define VOIDRV_PAD_START      0x0010   /* menu  */
#define VOIDRV_PAD_BACK       0x0020   /* view  */
#define VOIDRV_PAD_LTHUMB     0x0040
#define VOIDRV_PAD_RTHUMB     0x0080
#define VOIDRV_PAD_LSHOULDER  0x0100
#define VOIDRV_PAD_RSHOULDER  0x0200
#define VOIDRV_PAD_GUIDE      0x0400
#define VOIDRV_PAD_A          0x1000
#define VOIDRV_PAD_B          0x2000
#define VOIDRV_PAD_X          0x4000
#define VOIDRV_PAD_Y          0x8000

/* Full gamepad state (one report). Sticks are signed (-32768..32767, up/right
   positive); triggers are 0..255. */
typedef struct VoidrvPadState {
    uint16_t Buttons;       /* VOIDRV_PAD_* bitmask */
    uint8_t  LeftTrigger;
    uint8_t  RightTrigger;
    int16_t  ThumbLX;
    int16_t  ThumbLY;
    int16_t  ThumbRX;
    int16_t  ThumbRY;
} VoidrvPadState;

/* Submit a full gamepad report. Requires a gamepad handle. */
bool              VoidrvInputPadReport(VoidrvInputHandle handle, const VoidrvPadState* state);

/* Force-feedback callback. Motor magnitudes are 0..100; invoked on an SDK thread
   whenever the OS/game sends rumble. Low/high = left (low-freq) / right (high-freq)
   motors; lTrig/rTrig = impulse-trigger motors. */
typedef void (*VoidrvRumbleCallback)(void* context, uint8_t low, uint8_t high,
                                     uint8_t lTrigger, uint8_t rTrigger);

/* Register (or clear, with NULL) the rumble callback for a gamepad handle. The
   SDK starts a background reader that drains the output-event channel. */
bool              VoidrvInputPadSetRumbleCallback(VoidrvInputHandle handle,
                                                  VoidrvRumbleCallback callback, void* context);

/* ---- Touch + pen digitizer ---- */

/* Pointer action for the touch/pen helpers. */
#define VOIDRV_TOUCH_DOWN   0   /* contact down / pen tip down */
#define VOIDRV_TOUCH_MOVE   1   /* update while in contact */
#define VOIDRV_TOUCH_UP     2   /* contact lifted / pen tip up (pen stays in range) */
#define VOIDRV_TOUCH_HOVER  3   /* in range, not in contact */
#define VOIDRV_TOUCH_CANCEL 4   /* touch: cancel this contact; pen: leave range */

/* Touch contact event on a touch-digitizer handle. contactId identifies the finger
   (0..9 live at once). px/py are desktop pixels mapped to the associated display
   (primary by default; see VoidrvInputTouchSetBounds). pressure (0..1023) and size
   (contact width in pixels) are best-effort. The SDK keeps the live contact set and
   submits the full multitouch report each call. */
bool              VoidrvInputTouchContact(VoidrvInputHandle handle, uint8_t contactId,
                                          uint8_t action, int32_t px, int32_t py,
                                          uint16_t pressure, uint8_t size);

/* Drop all active touch contacts (and lift the pen), emitting a clean release. */
bool              VoidrvInputTouchCancelAll(VoidrvInputHandle handle);

/* Pixel rectangle the touch/pen pixel helpers map within. width<=0 restores the
   default (primary display). */
bool              VoidrvInputTouchSetBounds(VoidrvInputHandle handle, int32_t left,
                                            int32_t top, int32_t width, int32_t height);

/* Pen event on a touch-digitizer handle. px/py are desktop pixels; pressure 0..1023;
   tiltX/tiltY in degrees (-90..90); barrel/eraser are button flags. action is a
   VOIDRV_TOUCH_* code (UP keeps the pen hovering; CANCEL lifts it out of range). */
bool              VoidrvInputPen(VoidrvInputHandle handle, uint8_t action,
                                 int32_t px, int32_t py, uint16_t pressure,
                                 int8_t tiltX, int8_t tiltY, bool barrel, bool eraser);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VOIDRV_H */
