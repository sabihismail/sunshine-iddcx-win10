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
   chain, monitor mode query/commit, etc.
3. Create the device interface `{40255101-a910-441c-84d6-9f027197fa70}` and a
   default WDFQUEUE for control IOCTLs.
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
- `LIST` returns the current table.
- No `UPDATE`/ping code exists, by design.

## 4. Default mode set

Advertised on every monitor unless restricted. Default mode is 1920x1080 at 60 Hz.

| Resolution | Aspect | Refresh (Hz) |
|------------|--------|--------------|
| 1280 x 720  | 16:9  | 60 |
| 1600 x 900  | 16:9  | 60 |
| 1920 x 1080 | 16:9  | 60, 120, 144, 240 |
| 1920 x 1200 | 16:10 | 60 |
| 2560 x 1440 | 16:9  | 60, 120, 144, 240 |
| 2560 x 1600 | 16:10 | 60 |
| 3440 x 1440 | 21:9  | 60, 100, 144 |
| 3840 x 2160 | 16:9  | 60, 120, 144 |

The set is a compiled-in table; per-display restriction and user-defined modes are a
later enhancement.

## 5. EDID

Each monitor reports a synthesized EDID 1.4 block:

- Manufacturer id `VVD`. EDID packs three 5-bit letters (A=1..Z=26) big-endian with
  the high bit clear: `V=22, V=22, D=4` -> bytes `0x5A 0xC4` at EDID offsets 8-9.
- Monitor product name `VoidDisplay` in a type 0xFC descriptor.
- Preferred detailed timing matches the display's current mode (default
  1920x1080 @ 60).
- Standard timings advertise the common modes from section 4; the full mode list is
  provided to IddCx via the monitor mode list, not only via EDID.
- Byte 127 is the checksum (sum of all 128 bytes mod 256 == 0).

The concrete 128-byte block is generated at build time from these fields; see the
planned `edid.md`. HDR/extension blocks are out of scope for v1.

## 6. Persistence

Display state is optional-persistent so a configured host survives reboot:

- Under the device hardware key: `...\PersistedDisplays\NN` (NN = slot 00..07) with
  values `Width`, `Height`, `RefreshHz`, and a top-level `RestoreOnStart` flag.
- On `ADD`/`REMOVE`/`SET_MODE`, the table is written back.
- On adapter init-finished, if `RestoreOnStart` is set, each persisted slot is
  recreated before control IOCTLs are served.

Persistence is a convenience layer over the in-memory table; the in-memory table is
always the source of truth while the driver is running.

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

## 8. Logging

- `OutputDebugString`-based tracing gated behind a debug flag, visible in DebugView.
- Messages are ASCII only (no em/en dashes or arrows).
- Log device add/remove, mode commits, IOCTL entry/exit with status, and IddCx
  callback transitions.

## 9. Out of scope for v1

- HDR and wide-gamut (requires a newer IddCx level and HDR metadata path).
- Per-display custom/user mode lists.
- Gamma/color management.

## 10. Open items

- Frame delivery contract for the capture path (how the latest swap-chain frame is
  exposed to the host application) - to be specified alongside the host integration.
- Exact preferred-timing detailed descriptor parameters (blanking/sync) for the
  default mode.
