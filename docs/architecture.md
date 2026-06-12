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
  |  IddCx UMDF (.dll)   |            |   UDECX KMDF (.sys)     |
  |  user mode           |            |   kernel mode           |
  +----------+-----------+            +------------+------------+
             |                                     |
             v                                     v
   virtual monitors                    virtual USB bus + child
   (1..8 displays)                     devices: mouse, keyboard,
                                       Xbox 360 / DS4 / DS5, touch
```

| Component | Kind | Runs in | Responsibility |
|-----------|------|---------|----------------|
| VoidDisplay | IddCx Indirect Display Driver (UMDF) | user mode | Create/destroy virtual monitors, advertise modes, hand frames to the OS compositor |
| VoidInput | UDECX virtual USB bus (KMDF) | kernel mode | Host a virtual USB controller and plug/unplug emulated USB devices |
| libvoidrv | Static library, `voidrv.h` | user mode | C ABI wrapper over both drivers' IOCTL interfaces |
| voidctl | CLI executable | user mode | Manual control and per-milestone smoke testing |

## Control plane

Both drivers expose a private device interface and are driven entirely through
`DeviceIoControl`. There is no user-mode service or background pinger; the host
application calls into `libvoidrv` synchronously.

- VoidDisplay device interface GUID: `{40255101-a910-441c-84d6-9f027197fa70}`
- VoidInput bus interface GUID: `{7b0c8d49-54fc-45ca-ab47-6a572f2ff510}`

`libvoidrv` opens each interface by GUID (enumerate device interfaces, then
`CreateFile` the device path) and presents a typed API. Host code never builds raw
IOCTL buffers.

### Hot path

Per-frame input (mouse moves, pad reports) is sent through VoidInput as overlapped
endpoint-write IOCTLs - one report per call, no polling. VoidDisplay frames are
delivered through the IddCx swap-chain path, not through IOCTL.

## Device model

### VoidDisplay

- One root-enumerated PnP device (hardware id `Root\Void\Display`) of the Display
  class, with the `IndirectKmd` upper filter and the IddCx UMDF extension.
- Up to 8 virtual monitors, each created on demand via the control IOCTLs and
  surfaced to the OS through IddCx monitor arrival.
- See [voiddisplay-design.md](voiddisplay-design.md).

### VoidInput

- One root-enumerated PnP device (hardware id `Root\Void\VUSB`) acting as a virtual
  USB host controller via UDECX.
- Child devices are emulated USB devices that plug into that virtual bus. Each child
  presents a complete, standard USB device descriptor so the OS binds the normal
  in-box class driver (HID, XInput/XUSB) - the children are indistinguishable from
  physical USB hardware.
- Design doc to follow (`voidinput-design.md`).

## Persistence model

Void favors explicit lifetime over session coupling:

- A virtual display or input device exists from the moment it is added until it is
  explicitly removed or the driver unloads. There is no keepalive timer; nothing
  has to ping the driver to keep a device alive.
- Optionally, device state (which displays/devices were present, and their config)
  is recorded under the driver's hardware registry key and restored on the next
  start, so a configured host comes back the same after reboot.

## Build, signing, and platform

- Target: x64, Windows 10 21H2 / Windows 11.
- VoidDisplay is a user-mode UMDF driver; VoidInput is a kernel-mode driver.
- Development builds are test-signed; enable test-signing on the target host before
  installing. Production signing is a separate, later release task.

## Threading and I/O

- VoidDisplay runs in the UMDF host process; IddCx callbacks drive monitor and
  swap-chain lifetime. Control IOCTLs mutate a small in-memory display table guarded
  by a lock.
- VoidInput uses KMDF queues for control IOCTLs and per-endpoint I/O. Endpoint reads
  pend until data is available; writes complete as reports are consumed. No busy
  loops.
