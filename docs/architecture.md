# Void Drivers - Architecture

Void provides virtual display and input hardware for headless Windows hosts (for
example cloud-gaming or remote-desktop servers that have no physical monitor,
mouse, keyboard, or gamepad attached). The virtual devices appear to applications
and the OS as genuine hardware.

## Components

```
  +-----------------------------------------------------------+
  |                     Host application                       |
  |             (capture / streaming / input feed)            |
  +----------------------------+------------------------------+
                               |  links
                               v
                     +-------------------+        +-----------+
                     |     libvoidrv     |<------>|  voidctl  |
                     |    (voidrv.h)     |        |   (CLI)   |
                     +---------+---------+        +-----------+
                               |  DeviceIoControl
             +-----------------+------------------+
             v                                    v
  +----------------------+            +-------------------------+
  |     VoidDisplay      |            |        VoidInput        |
  |  IddCx UMDF (.dll)   |            |   UMDF + VHF (.dll)     |
  |  user mode           |            |   user mode             |
  +----------+-----------+            +------------+------------+
             |                                     |
             v                                     v
   virtual monitors                    virtual HID devices:
   (1..8 displays)                     mouse, keyboard, touch,
                                       Xbox One / DS4 / DS5
```

| Component | Kind | Runs in | Responsibility |
|-----------|------|---------|----------------|
| VoidDisplay | IddCx Indirect Display Driver (UMDF) | user mode | Create/destroy virtual monitors, advertise modes, hand frames to the OS compositor |
| VoidInput | UMDF driver on the Virtual HID Framework (VHF) | user mode | Create/destroy virtual HID devices (mouse, keyboard, gamepads, touch) and relay reports |
| libvoidrv | Static library, `voidrv.h` | user mode | C ABI wrapper over both drivers' IOCTL interfaces |
| voidctl | CLI executable | user mode | Manual control and per-milestone smoke testing |

## Control plane

Both drivers expose a private device interface and are driven entirely through
`DeviceIoControl`. There is no user-mode service or background pinger; the host
application calls into `libvoidrv` synchronously.

- VoidDisplay device interface GUID: `{40255101-a910-441c-84d6-9f027197fa70}`
- VoidInput control interface GUID: `{7b0c8d49-54fc-45ca-ab47-6a572f2ff510}`

`libvoidrv` opens each interface by GUID (enumerate device interfaces, then
`CreateFile` the device path) and presents a typed API. Host code never builds raw
IOCTL buffers.

### Hot path

Per-frame input (mouse moves, pad reports) is submitted to VoidInput as HID input
reports - one `WriteFile` per report on the device handle, no polling. VoidDisplay
frames are delivered through the IddCx swap-chain path, not through IOCTL.

## Device model

### VoidDisplay

- One root-enumerated PnP device (hardware id `Root\Void\Display`) of the Display
  class, with the `IndirectKmd` upper filter and the IddCx UMDF extension.
- Up to 8 virtual monitors, each created on demand via the control IOCTLs and
  surfaced to the OS through IddCx monitor arrival.
- See [voiddisplay-design.md](voiddisplay-design.md).

### VoidInput

- One root-enumerated PnP device (hardware id `Root\Void\Input`), a UMDF driver
  layered on the in-box Virtual HID Framework (VHF).
- Each virtual input device is created on demand (one `VhfCreate` per device) and
  enumerated by the OS under HIDClass with a cloned HID identity (VID/PID + report
  descriptor), so it binds the normal in-box HID class driver and is
  indistinguishable from a real HID device to applications.
- Gamepads are HID (Xbox One, DualShock 4, DualSense), reaching games via
  DirectInput / Windows.Gaming.Input / Steam Input. Real XInput/`xusb` is out of
  scope (it would require a kernel USB bus).
- See [voidinput-design.md](voidinput-design.md).

## Persistence model

Void favors explicit lifetime over session coupling:

- A virtual display or input device exists from the moment it is added until it is
  explicitly removed or the driver unloads. There is no keepalive timer; nothing
  has to ping the driver to keep a device alive.
- Optionally, display state (which monitors were present, and their modes) is recorded
  in a config file under `%ProgramData%` (not the registry - the user-mode host cannot
  write there) and restored on the next start, so a configured host comes back the same
  after a reboot. Input devices are plugged on demand by the host and are not persisted.

## Build, signing, and platform

- Target: x64, Windows 10 21H2 / Windows 11.
- Both VoidDisplay and VoidInput are user-mode UMDF drivers (VoidInput layers on
  the in-box VHF); neither enters the kernel.
- Development builds are signed with a local test certificate; no `testsigning`
  mode and no Microsoft attestation are required. Production signing (a directly
  applied OV/EV Authenticode signature) is a separate, later release task.

## Threading and I/O

- VoidDisplay runs in the UMDF host process; IddCx callbacks drive monitor and
  swap-chain lifetime. Control IOCTLs mutate a small in-memory display table guarded
  by a lock.
- VoidInput runs in the UMDF host process. Control IOCTLs and input-report writes
  go through a WDF queue; VHF callbacks deliver output/feature requests, which pend
  on an inverted read until the host services them. No busy loops.
