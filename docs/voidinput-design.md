# VoidInput - Design Specification

VoidInput is a virtual input driver that presents genuine-looking HID devices -
mouse, keyboard, gamepads (Xbox One, DualShock 4, DualSense), and a touch
digitizer - on a headless host that has no physical input hardware. A host
application drives the devices through one SDK; games and the OS see real HID
controllers.

VoidInput is a **UMDF driver built on the in-box Virtual HID Framework (VHF,
`Vhf.sys`)**. It runs entirely in user mode: no kernel code of our own, and the
same signing class as VoidDisplay (a directly-signed `.dll`, no Microsoft
attestation). Devices are plugged on demand and live until removed; there is no
keepalive.

## 1. Device stack

```
  Root\Void\Input            (root-enumerated PnP device, System class)
        |
   VHF (Vhf.sys)             (lower service - the in-box virtual HID framework)
        |
   VoidInput.dll             (UMDF upper filter; the VHF client/source)
        |
   WUDFRd  ->  WUDFHost.exe
        |
   (per device) VhfCreate -> a virtual HID device enumerated under HIDClass
```

- Class: System, `{4d36e97d-e325-11ce-bfc1-08002be10318}`.
- Hardware id: `Root\Void\Input`.
- The driver installs as an **upper filter on VHF**: the INF pulls in the in-box
  `hidvhf.inf` (VHF service) and `wudfrd.inf` (UMDF reflector), so the one
  root-enumerated devnode loads VHF underneath and VoidInput on top. The driver
  acts as the VHF client (calls `VhfCreate` / `VhfStart` / `VhfDelete`).
- Each virtual input device the host asks for becomes one `VhfCreate` call, and
  the resulting HID device is enumerated by the OS under HIDClass with the
  device's cloned VID/PID and report descriptor - indistinguishable to
  applications from a real HID device.
- `PnpLockdown=1`. One INF, attestation-ready but not requiring attestation.

VoidInput presents HID devices only. Real `XInputGetState`/`xusb` controllers
require a kernel USB bus and are out of scope by design (see section 9); gamepads
are HID and reach games through DirectInput, Windows.Gaming.Input, and Steam
Input.

## 2. Lifecycle and callbacks

The driver is a single root-enumerated UMDF device that exposes a private control
interface. The model is **one virtual HID device per open handle**:

1. `DriverEntry` -> `WdfDriverCreate`, register `EVT_WDF_DRIVER_DEVICE_ADD`.
2. Device add -> create the WDFDEVICE, set it up as a VHF upper filter, create the
   default I/O queue (control IOCTLs + input-report writes) and a manual queue for
   pended output-event reads, and register the control device interface
   `{7b0c8d49-54fc-45ca-ab47-6a572f2ff510}`.
3. A host opens the interface. Each open gets a fresh file context and its own VHF
   I/O target; no virtual device exists yet.
4. `IOCTL_VOIDINPUT_CREATE` selects a device type. The driver looks up the
   compiled-in descriptor set for that type, fills the `VHF_CONFIG` (VID/PID,
   version, report descriptor, container id, and the subset of VHF event callbacks
   the type needs), enforces the capacity rules (section 6), then calls `VhfCreate`
   + `VhfStart`. The new HID device arrives in the OS. A neutral initial report is
   submitted so the device reads as "connected, no input."
5. The host streams input reports (section 5) and services output events
   (section 5) for the life of the handle.
6. Closing the handle (or `IOCTL_VOIDINPUT_DESTROY`) calls `VhfDelete`, the HID
   device departs, and the slot is freed. There is no persisted state to restore.

The host application keeps one device handle per virtual device. It may also keep
one extra "control" handle (one that never issues `CREATE`) for version/enumeration
queries.

## 3. Control interface (IOCTL)

Private interface GUID `{7b0c8d49-54fc-45ca-ab47-6a572f2ff510}`. All codes use
`FILE_DEVICE_UNKNOWN`, `METHOD_BUFFERED`. The hot path (input report submission)
is a plain `WriteFile` on the device handle, not an IOCTL, to keep per-report
overhead minimal.

```c
#define VOIDINPUT_IOCTL(i, access) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (i), METHOD_BUFFERED, (access))

// Query interface/driver version. Out: VOIDINPUT_VERSION (ULONG).
#define IOCTL_VOIDINPUT_VERSION    VOIDINPUT_IOCTL(1, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Create the virtual HID device on this handle. In: VOIDINPUT_CREATE.
#define IOCTL_VOIDINPUT_CREATE     VOIDINPUT_IOCTL(2, FILE_WRITE_ACCESS)

// Destroy the device on this handle (also implicit on handle close).
#define IOCTL_VOIDINPUT_DESTROY    VOIDINPUT_IOCTL(3, FILE_WRITE_ACCESS)

// List live devices across the driver (diagnostics). Out: VOIDINPUT_STATE.
#define IOCTL_VOIDINPUT_LIST       VOIDINPUT_IOCTL(4, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Pend for the next output/feature event for this device. Out: VOIDINPUT_EVENT.
#define IOCTL_VOIDINPUT_GET_EVENT  VOIDINPUT_IOCTL(5, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Complete a previously returned output/feature event. In: VOIDINPUT_EVENT_COMPLETE.
#define IOCTL_VOIDINPUT_COMPLETE_EVENT VOIDINPUT_IOCTL(6, FILE_WRITE_ACCESS)
```

```c
#define VOIDINPUT_VERSION       1
#define VOIDINPUT_MAX_DEVICES   8

typedef enum _VOIDINPUT_DEVICE_TYPE {
    VoidInputMouse    = 1,   // relative + absolute, singleton
    VoidInputKeyboard = 2,   // boot keyboard + consumer + system, singleton
    VoidInputXboxOne  = 3,   // HID gamepad
    VoidInputDS4      = 4,   // DualShock 4
    VoidInputDS5      = 5,   // DualSense (input-only)
    VoidInputTouch    = 6,   // precision multi-touch digitizer, singleton
} VOIDINPUT_DEVICE_TYPE;

typedef struct _VOIDINPUT_CREATE {
    UINT32 Type;             // VOIDINPUT_DEVICE_TYPE
    UINT32 Flags;            // reserved, 0
    UINT16 VendorId;         // 0 = use the type's default cloned VID
    UINT16 ProductId;        // 0 = use the type's default cloned PID
    UINT16 VersionNumber;    // 0 = type default
    UINT16 Reserved;
} VOIDINPUT_CREATE;

typedef struct _VOIDINPUT_ENTRY {
    UINT32 Type;             // VOIDINPUT_DEVICE_TYPE, 0 if slot empty
    UINT16 VendorId;
    UINT16 ProductId;
} VOIDINPUT_ENTRY;

typedef struct _VOIDINPUT_STATE {
    UINT32 Count;
    VOIDINPUT_ENTRY Entries[VOIDINPUT_MAX_DEVICES];
} VOIDINPUT_STATE;

// One output/feature request handed back to the host.
typedef enum _VOIDINPUT_EVENT_TYPE {
    VoidInputEventWriteReport = 1,   // OS -> device output report (rumble, LED, lightbar)
    VoidInputEventSetFeature  = 2,   // OS -> device feature write
    VoidInputEventGetFeature  = 3,   // OS <- device feature read (host must supply data)
} VOIDINPUT_EVENT_TYPE;

typedef struct _VOIDINPUT_EVENT {
    UINT32 Type;             // VOIDINPUT_EVENT_TYPE
    UINT32 RequestId;        // opaque, echo back in COMPLETE
    UINT8  ReportId;
    UINT8  Reserved[3];
    UINT32 DataLength;       // bytes following (write/set-feature carry data)
    UINT8  Data[1];          // DataLength bytes
} VOIDINPUT_EVENT;

typedef struct _VOIDINPUT_EVENT_COMPLETE {
    UINT32 RequestId;
    INT32  Status;           // 0 = success
    UINT32 DataLength;       // for GetFeature: bytes the host is returning
    UINT8  Data[1];
} VOIDINPUT_EVENT_COMPLETE;
```

Semantics:

- `CREATE` builds the device from the compiled-in descriptor for `Type`. The
  optional VID/PID/version fields override the cloned identity (for testing);
  zero means "use the genuine default." Fails with a distinct status if the
  per-type capacity is exceeded (section 6) or if `CREATE` was already issued on
  this handle.
- Input reports are submitted with `WriteFile` (section 5), not an IOCTL.
- `GET_EVENT` is the inverted read for the device->host direction: the host keeps
  one outstanding, and the driver completes it when the OS issues an output or
  feature request. The host processes it and calls `COMPLETE_EVENT` echoing
  `RequestId` (supplying data for a `GetFeature`). This is how rumble, lightbar,
  and keyboard LED state reach the host.
- `DESTROY` (or handle close) tears the device down. `LIST` reports the live set
  for `voidctl` and diagnostics.
- There is no `UPDATE`/keepalive code, by design.

## 4. Device types and descriptors

Each type ships a fixed, compiled-in HID report descriptor and a cloned device
identity, so every instance is byte-identical and not caller-tamperable. We clone
the **HID identity** (VID/PID + report descriptor); these are HID devices, not USB
devices, so cloning is at the HID-report level (see section 9).

| Type | VID:PID | Identity | Collections / report layout |
|------|---------|----------|------------------------------|
| Mouse | generic | a common HID mouse | relative collection + absolute collection (one device, two report IDs) |
| Keyboard | generic | a common HID keyboard | boot keyboard + consumer control + system control; output report for lock LEDs |
| Xbox One | `045E:0B13` | Xbox wireless controller (HID/BLE form) | game pad: 2x 16-bit sticks, 2x 10-bit triggers, 10 buttons, hat, rumble output |
| DualShock 4 | `054C:05C4` | Sony DualShock 4 | full DS4 input report (sticks, triggers, buttons, gyro/accel, touchpad); rumble + lightbar output; feature-report handshake |
| DualSense | `054C:0CE6` | Sony DualSense | DS5 input report (adds adaptive-trigger fields); rumble + lightbar + trigger output; feature-report handshake; **no audio** |
| Touch | generic | precision touch digitizer | multi-touch, up to 10 contacts; the Windows Precision certification feature reports |

Report-shape notes:

- **Mouse.** One device exposing both a relative and an absolute collection so a
  host can drive FPS-style relative motion and desktop-accurate absolute
  positioning without a second device. Buttons are a bitmask
  (left=0x01, right=0x02, middle=0x04, X1=0x08, X2=0x10), plus a vertical and a
  horizontal wheel. Relative axes are signed 16-bit; absolute axes are unsigned
  16-bit over a 0..32767 logical range.
- **Keyboard.** A boot-protocol keyboard (one modifier-bits byte, one reserved
  byte, six key-code slots) plus a consumer-control collection (media/volume keys)
  and a system-control collection (power/sleep/wake). The OS sends a lock-LED
  output report (caps/num/scroll), surfaced to the host as an output event. N-key
  rollover is a later enhancement.
- **Xbox One.** A standard Xbox-compatible HID game pad. The single output report
  carries the rumble model (left/right motors + trigger motors, with
  duration/delay/repeat), surfaced as an output event so the host can forward
  haptics to a remote controller.
- **DualShock 4 / DualSense.** The genuine DS4/DS5 input report layouts (sticks,
  triggers, face/shoulder buttons, D-pad, gyro and accelerometer, touchpad
  contacts). Output reports carry rumble and lightbar (and DualSense trigger
  effects). The driver answers the feature-report handshake those controllers use
  for identification/calibration so that Steam and games recognize them as genuine
  DualShock devices and enable gyro/touchpad/lightbar.
- **Touch.** A Windows Precision multi-touch digitizer (contact id, tip switch,
  X/Y, scan time, contact count) answering the required device-capability and
  certification feature reports. Last in the build order.

The compiled-in descriptors live in the driver; the SDK never sends a descriptor.
Because the driver knows each descriptor at compile time, it also knows every
report's exact size and does not parse descriptors at runtime.

## 5. Report flow

### Input (host -> device), the hot path

The OS HID stack keeps a read request pending on each device's input pipe. The
host submits a report with a single `WriteFile` on the device handle; the driver
forwards it to VHF (`VhfReadReportSubmit`), which completes the OS read. One
report per call, fire-and-forget, no polling and no keepalive.

The SDK packs typed state into report bytes in user mode (the driver is a thin
sink): e.g. `VoidInput_MouseMoveRelative(dx, dy, buttons, wheel)`,
`VoidInput_KeyboardReport(mods, keys[6])`, `VoidInput_PadReport(handle, &state)`.
The host therefore never builds raw HID bytes.

Coalescing is the SDK's job: input reports are device *state*, so when the host
produces reports faster than the OS consumes them, the SDK submits only the latest
state rather than building a backlog. This keeps absolute-pointer and pad state
current instead of replaying stale positions.

### Output (device -> host)

Rumble/force-feedback, the DualSense/DS4 lightbar, DualShock trigger effects, and
keyboard lock LEDs are output and feature reports the OS sends *down* to the
device. VHF delivers them to the driver as asynchronous operations; the driver
queues them and completes the host's pending `GET_EVENT` IOCTL with the report
type and bytes. The host processes the event (for example, mapping motor
amplitudes to a remote controller) and calls `COMPLETE_EVENT`. For a feature read
(`GetFeature`), the host returns the requested bytes in the completion. The SDK
hides this behind a per-device callback registered at create time.

A device only registers for the event kinds its type uses (a mouse registers
none; a pad registers output reports; the DualShock types also register feature
reports for the handshake), so unused paths cost nothing.

## 6. Capacity and concurrency

- Up to **8** virtual devices total (`VOIDINPUT_MAX_DEVICES`).
- **Gamepads: up to 4** simultaneously (any mix of Xbox One / DS4 / DS5), matching
  the practical four-player ceiling.
- **Mouse, keyboard, touch: one each** (singletons). A headless host has no use
  for a second virtual pointer or keyboard, and Windows merges multiple HID
  keyboards/mice into one logical input anyway.
- The driver tracks the live set and fails `CREATE` with a distinct status when a
  per-type cap or the overall cap would be exceeded. Multiple gamepads are
  distinguished to the OS by a per-instance container id / instance id derived from
  the input child container-id base.

Limits are enforced in the driver, not the SDK, so they hold no matter how the
control interface is opened.

## 7. Signing and installation

- VoidInput is UMDF (user mode); it never enters the kernel. It is installed by an
  Authenticode signature (our OV or EV cert) on the `.dll` + `.cat`, signed
  directly. **No Microsoft attestation and no `testsigning` mode are required.**
- For development, a local self-signed cert in `Root` + `TrustedPublisher` is
  sufficient.
- Install is root-enumerated. As with VoidDisplay, creating the root devnode uses
  `devcon install <inf> Root\Void\Input` (a bare `pnputil` will not create the
  devnode); the install/test scripts live alongside the VoidDisplay ones.

## 8. Logging

- `OutputDebugString`-based tracing gated behind a debug flag, visible in
  DebugView. UMDF host output comes from a session-0 process, captured with the
  DBWIN listener used for VoidDisplay.
- Messages are ASCII only (no em/en dashes or arrows).
- Log device create/destroy with type and identity, capacity rejections, VHF
  create/start/delete transitions, and output-event dispatch.

## 9. Out of scope for v1

- **Real XInput / `xusb`.** An Xbox 360-class XInput device is a vendor-class USB
  device bound by `xusb22.sys`; presenting one requires a kernel USB bus, which
  this user-mode design deliberately avoids. Gamepads are HID, reaching games via
  DirectInput / Windows.Gaming.Input / Steam Input. `XInputGetState` will not see
  them.
- **USB-level fidelity.** These are virtual HID devices, not USB devices; software
  that inspects the USB device tree sees a virtual HID device rather than a USB
  device with the cloned descriptors. HID-report-level identity is cloned.
- **Audio.** Virtual speaker and microphone are a separate future component
  (`VoidAudio`), a dedicated virtual audio endpoint, not part of this driver.
- **Persistence.** Input devices are plugged on demand by the host application and
  are not restored across reboot (unlike VoidDisplay monitors). A config-file hook
  (the same `%ProgramData%` file VoidDisplay uses) is reserved for a later always-on
  mouse/keyboard option.
- N-key-rollover keyboard; per-device descriptor customization.

## 10. Open items

- Exact cloned report descriptors and feature-report handshakes for DualShock 4
  and DualSense (enough for Steam/game recognition of gyro, touchpad, lightbar).
- The Xbox One identity choice (`045E:0B13` BLE form vs `045E:02FD`) and whether to
  match a specific controller's report descriptor exactly.
- Touch digitizer certification feature-report set.
- Whether to offer optional persistence for the mouse/keyboard singletons.
