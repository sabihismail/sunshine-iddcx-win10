# VoidDisplay - Design Specification

VoidDisplay is an Indirect Display Driver (IDD) built on the IddCx UMDF extension.
It creates up to eight virtual monitors on a headless host so that capture and
streaming software sees real display outputs. Displays persist until explicitly
removed; there is no keepalive.

## 1. Device stack

```
  Root\Void\Display          (root-enumerated PnP device, Display class)
        |
   IndirectKmd                (upper filter, provides the indirect display KMD)
        |
   WUDFRd  ->  WUDFHost.exe
        |
   VoidDisplay.dll  +  IddCx  (UMDF extension IddCx0102)
```

- Class: Display, `{4d36e968-e325-11ce-bfc1-08002be10318}`.
- Hardware id: `Root\Void\Display`.
- UMDF extension: the INF binds `IddCx0102` so the driver loads on the widest range
  of Windows. The binary is compiled against the IddCx 1.4 headers (UMDF 2.25) with
  the minimum version set to 1.3, and any post-1.2 DDI is gated at runtime with
  `IDD_IS_FUNCTION_AVAILABLE`. One binary therefore runs on older hosts (1.2/1.3
  feature set) and lights up newer features where the running OS supports them
  (see section 7). The HDR path will compile against a newer header still.
- `PnpLockdown=1`. The INF installs WUDFRd, sets the `IndirectKmd` upper filter, and
  sets a `DeviceGroupId` of `VoidDisplayGroup`.

## 2. Lifecycle and callbacks

Standard IddCx flow:

1. `DriverEntry` -> `WdfDriverCreate`, register `EVT_WDF_DRIVER_DEVICE_ADD`.
2. Device add -> `IddCxDeviceInitConfig`, create the WDFDEVICE, then
   `IddCxDeviceInitialize`. Register the IddCx client callbacks
   (`IDD_CX_CLIENT_CONFIG`): adapter init-finished, monitor assign/unassign swap
   chain, monitor mode query/commit, and `EvtIddCxDeviceIoControl` for control IOCTLs.
3. Create the device interface `{40255101-a910-441c-84d6-9f027197fa70}` for user
   mode to open. Control IOCTLs are delivered through `EvtIddCxDeviceIoControl`, NOT
   a custom WDF queue: IddCx owns the device's `IRP_MJ_DEVICE_CONTROL` dispatching,
   so a default/own queue never receives them (and `WdfDeviceConfigureRequestDispatching`
   returns `STATUS_WDF_BUSY`).
4. On adapter init-finished, restore any persisted displays (section 6), then begin
   serving control IOCTLs.

Each virtual monitor:

- `IddCxMonitorCreate` with the monitor description (EDID + supported modes), then
  `IddCxMonitorArrival` to surface it to the OS.
- On removal, `IddCxMonitorDeparture`.
- Swap-chain assign/unassign callbacks set up and tear down frame delivery; for a
  headless capture target the processing thread simply acquires and releases frames
  (and exposes the latest frame to the capture path).

## 3. Control interface (IOCTL)

Private interface GUID `{40255101-a910-441c-84d6-9f027197fa70}`. All codes use
`FILE_DEVICE_UNKNOWN`, `METHOD_BUFFERED`.

```c
#define VOIDDISPLAY_IOCTL(i, access) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (i), METHOD_BUFFERED, (access))

// Query interface/driver version. Out: VOIDDISPLAY_VERSION (ULONG).
#define IOCTL_VOIDDISPLAY_VERSION   VOIDDISPLAY_IOCTL(1, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Add a display. In: VOIDDISPLAY_MODE. Out: ULONG index (0..7).
#define IOCTL_VOIDDISPLAY_ADD       VOIDDISPLAY_IOCTL(2, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Remove a display. In: ULONG index.
#define IOCTL_VOIDDISPLAY_REMOVE    VOIDDISPLAY_IOCTL(3, FILE_WRITE_ACCESS)

// Change a display's mode. In: VOIDDISPLAY_SET_MODE.
#define IOCTL_VOIDDISPLAY_SET_MODE  VOIDDISPLAY_IOCTL(4, FILE_WRITE_ACCESS)

// List current displays. Out: VOIDDISPLAY_STATE.
#define IOCTL_VOIDDISPLAY_LIST      VOIDDISPLAY_IOCTL(5, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Add a custom mode to the advertised list. In: VOIDDISPLAY_MODE.
#define IOCTL_VOIDDISPLAY_ADD_MODE    VOIDDISPLAY_IOCTL(6, FILE_WRITE_ACCESS)

// Remove a custom mode (defaults cannot be removed). In: VOIDDISPLAY_MODE.
#define IOCTL_VOIDDISPLAY_REMOVE_MODE VOIDDISPLAY_IOCTL(7, FILE_WRITE_ACCESS)

// List advertised modes (defaults + custom). Out: VOIDDISPLAY_MODE_LIST.
#define IOCTL_VOIDDISPLAY_LIST_MODES  VOIDDISPLAY_IOCTL(8, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Set a mode on a live display, advertising it on the fly if new. In: VOIDDISPLAY_SET_MODE.
#define IOCTL_VOIDDISPLAY_SET_MODE_DYNAMIC  VOIDDISPLAY_IOCTL(9, FILE_WRITE_ACCESS)
```

```c
#define VOIDDISPLAY_VERSION     1
#define VOIDDISPLAY_MAX_DISPLAYS 8

typedef struct _VOIDDISPLAY_MODE {
    UINT32 Width;            // pixels
    UINT32 Height;           // pixels
    UINT32 RefreshHz;        // nominal refresh, e.g. 60, 120, 144, 240
} VOIDDISPLAY_MODE;

typedef struct _VOIDDISPLAY_SET_MODE {
    UINT32 Index;            // 0..VOIDDISPLAY_MAX_DISPLAYS-1
    VOIDDISPLAY_MODE Mode;
} VOIDDISPLAY_SET_MODE;

typedef struct _VOIDDISPLAY_ENTRY {
    UINT32 InUse;            // 0 or 1
    VOIDDISPLAY_MODE Mode;
} VOIDDISPLAY_ENTRY;

typedef struct _VOIDDISPLAY_STATE {
    UINT32 Count;            // number of displays in use
    VOIDDISPLAY_ENTRY Entries[VOIDDISPLAY_MAX_DISPLAYS];
} VOIDDISPLAY_STATE;
```

Semantics:

- `ADD` allocates the lowest free slot, creates and surfaces the monitor with the
  requested mode (or the default mode if the request is zeroed), and returns the
  slot index. Fails if all 8 slots are in use.
- `REMOVE` departs the monitor in that slot and frees it. Idempotent on an empty
  slot.
- `SET_MODE` updates the active mode of an existing display and re-advertises it.
- `SET_MODE_DYNAMIC` sets a mode on a live display for on-resize / arbitrary
  resolutions. The settable resolution list is fixed at monitor arrival
  (`ParseMonitorDescription`), so a brand-new mode is exposed by re-plugging that one
  monitor (a single-monitor hotplug - it blinks only that display, not the desktop;
  `IddCxMonitorUpdateModes` is deliberately NOT used because it forces a global
  topology re-evaluation that modesets the other monitors). An already-advertised mode
  is set directly with no re-plug. The SDK rounds to even and clamps to <= 4096x2160 /
  24..165 Hz, and does not persist it (dynamic modes are ephemeral).
- `LIST` returns the current table.
- `ADD_MODE` / `REMOVE_MODE` add or remove a custom resolution+refresh from the
  advertised mode list (built-in defaults always remain). The change applies to
  displays surfaced afterward; a live display picks up a new mode only via
  `SET_MODE_DYNAMIC` (or a remove/re-add). `LIST_MODES` returns the full advertised
  list (defaults + custom).
- The advertised list is a process-global store (the `ParseMonitorDescription`
  callback has no per-monitor context), so custom modes apply to every VoidDisplay
  monitor. Custom modes are durable: the SDK mirrors each `ADD_MODE`/`REMOVE_MODE`
  into a `CustomModes` value (REG_BINARY array of packed `{Width, Height, RefreshHz}`
  triples) under the driver's WUDF service `Parameters` key, and the driver re-reads
  it at adapter init, so they survive a device restart or reboot. The IOCTL changes
  the live list; the registry write (which needs elevation) is best-effort and does
  not affect the live change. Built-in defaults are always advertised and are never
  written to the registry.
- No `UPDATE`/ping code exists, by design.

## 4. Default mode set

Advertised on every monitor unless restricted. Default mode is 1920x1080 at 60 Hz
(reported as the preferred mode). Wide-gamut resolutions carry a 60/120/144/165 Hz
refresh ladder; 3:2 and laptop-panel resolutions are 60 Hz only.

| Resolution  | Aspect           | Refresh ladder (Hz) |
|-------------|------------------|---------------------|
| 4096 x 2160 | DCI 4K (256:135) | 60/120/144/165 |
| 3840 x 2160 | 16:9             | 60/120/144/165 |
| 3840 x 1600 | 24:10            | 60/120/144/165 |
| 3840 x 1080 | 32:9             | 60/120/144/165 |
| 3440 x 1440 | 21:9             | 60/120/144/165 |
| 2560 x 1080 | 21:9             | 60/120/144/165 |
| 3200 x 1800 | 16:9             | 60/120/144/165 |
| 2880 x 1620 | 16:9             | 60/120/144/165 |
| 2560 x 1600 | 16:10            | 60/120/144/165 |
| 2560 x 1440 | 16:9             | 60/120/144/165 |
| 2048 x 1152 | 16:9             | 60/120/144/165 |
| 1920 x 1200 | 16:10            | 60/120/144/165 |
| 1920 x 1080 | 16:9             | 60/120/144/165 |
| 1680 x 1050 | 16:10            | 60/120/144/165 |
| 1600 x 1200 | 4:3              | 60/120/144/165 |
| 1600 x 900  | 16:9             | 60/120/144/165 |
| 1440 x 900  | 16:10            | 60/120/144/165 |
| 1366 x 768  | 16:9             | 60/120/144/165 |
| 1280 x 800  | 16:10            | 60/120/144/165 |
| 1280 x 720  | 16:9             | 60/120/144/165 |
| 3240 x 2160 | 3:2              | 60 |
| 3000 x 2000 | 3:2              | 60 |
| 2880 x 1800 | 16:10            | 60 |
| 2736 x 1824 | 3:2              | 60 |
| 2496 x 1664 | 3:2              | 60 |
| 2256 x 1504 | 3:2              | 60 |
| 1800 x 1200 | 3:2              | 60 |

That is 87 advertised modes. The set is compiled in (`g_VoidDefaultModes`); callers
add their own resolutions at runtime with `ADD_MODE` (section 3), which persist
across restarts. Per-display restriction is a later enhancement.

## 5. EDID

Each monitor reports a synthesized EDID 1.4 block:

- Manufacturer id `VVD`. EDID packs three 5-bit letters (A=1..Z=26) big-endian with
  the high bit clear: `V=22, V=22, D=4` -> bytes `0x5A 0xC4` at EDID offsets 8-9.
- Monitor product name `VoidVDA` in a type 0xFC descriptor (the name shown in
  Windows Display Settings; the adapter stays `Void Virtual Display Adapter`).
- Preferred detailed timing matches the display's current mode (default
  1920x1080 @ 60).
- Standard timings advertise the common modes from section 4; the full mode list is
  provided to IddCx via the monitor mode list, not only via EDID.
- Byte 127 is the checksum (sum of all 128 bytes mod 256 == 0).

The concrete 128-byte block is generated at build time from these fields; see the
planned `edid.md`. HDR/extension blocks are out of scope for v1.

## 6. Persistence

Display state is persistent so a configured host survives a device restart or
reboot:

- Stored under the driver's WUDF service `Parameters` key as a `PersistedDisplays`
  value (REG_BINARY array of packed `{Index, Width, Height, RefreshHz}` UINT32
  entries, one per active slot), alongside a `RestoreOnStart` REG_DWORD (absent or
  non-zero means restore; `0` disables it).
- **The SDK writes it.** A UMDF host process has no registry write rights (both the
  device hardware key and the service `Parameters` key return access-denied), so the
  driver cannot self-persist. `libvoidrv` instead mirrors each `Add`/`Remove`/
  `SetMode` into `PersistedDisplays` (best-effort, needs elevation) - the same
  split used for custom modes (section 3). Because the write is best-effort, an
  unelevated caller's change is live but not saved; the SDK exposes
  `VoidrvDisplayPersistenceWritable()` to detect this and `voidctl` warns when a
  persisting command runs unelevated.
- On adapter init-finished, after the custom modes are loaded and before any control
  IOCTL is served, the driver reads `PersistedDisplays` (if `RestoreOnStart`) and
  recreates each display at its stored slot and mode. Monitor identities are stable
  per slot (ContainerId + EDID serial), so Windows also restores each display's last
  resolution and desktop position.

The in-memory slot table is the source of truth while the driver runs; the registry
copy is what a fresh adapter instance restores from.

## 7. Preferred render GPU (parent adapter)

On a multi-GPU headless host (for example an encode GPU plus an iGPU) the OS would
otherwise auto-pick the adapter that composes the virtual desktop. The operator can
pin it so the desktop is rendered on the GPU that also does capture/encode, avoiding
a cross-adapter copy.

- Configured via the driver's WUDF service Parameters key:
  `...\WUDF\Services\VoidDisplay\Parameters\PreferredRenderAdapterVendorId`
  (REG_DWORD). `0` or absent = auto; `0x10DE` = NVIDIA, `0x1002` = AMD,
  `0x8086` = Intel. The INF seeds the value at `0` (auto).
- At adapter init-finished the driver reads the value, enumerates DXGI adapters,
  finds the first non-software adapter from that vendor, and pins it with
  `IddCxAdapterSetRenderAdapter(adapter, { LUID })` - done before any monitor is
  added, as recommended.
- This DDI is IddCx 1.4+. The call is gated by `IDD_IS_FUNCTION_AVAILABLE`, so on
  older runtimes it is silently skipped (the OS keeps auto-selecting). This is why
  the binary compiles against the 1.4 headers while the INF still binds `IddCx0102`.
- Pinning by vendor id picks the first matching adapter; selecting between two GPUs
  of the same vendor (by LUID/index) is a later enhancement.

## 8. Hardware cursor

Each monitor exposes a hardware-cursor plane so the OS draws the mouse pointer as
an overlay rather than compositing it into the swap-chain image.

- On swap-chain assign, the driver calls `IddCxMonitorSetupHardwareCursor` with an
  `IDDCX_CURSOR_CAPS` (alpha + full color-XOR support, 256x256 max) and a per-monitor
  data-available event. Available since IddCx 1.0, so no runtime version guard.
- This keeps the captured desktop image pointer-free, which matters for streaming:
  remote-desktop clients that render their own cursor (the common low-latency design)
  otherwise show a double pointer - one baked into the stream and one drawn by the
  client. With the hardware-cursor plane the stream carries none, leaving only the
  client's.
- On by default; `HardwareCursorEnabled=0` (REG_DWORD under the service Parameters
  key) bakes the pointer into the frames instead (server-side cursor).
- The data-available event lets the capture path later query the cursor shape and
  position (`IddCxMonitorQueryHardwareCursor`) for a server-side-cursor mode; v1 only
  sets the plane up so the pointer stays out of the image. Each monitor uses its own
  unnamed event (a single shared event would collide across monitors).

## 9. Logging

- `OutputDebugString`-based tracing gated behind a debug flag, visible in DebugView.
- Messages are ASCII only (no em/en dashes or arrows).
- Log device add/remove, mode commits, IOCTL entry/exit with status, and IddCx
  callback transitions.

## 10. Out of scope for v1

- HDR and wide-gamut (requires a newer IddCx level and HDR metadata path).
- Per-display custom/user mode lists.
- Gamma/color management.

## 11. Open items

- Frame delivery contract for the capture path (how the latest swap-chain frame is
  exposed to the host application) - to be specified alongside the host integration.
- Exact preferred-timing detailed descriptor parameters (blanking/sync) for the
  default mode.
