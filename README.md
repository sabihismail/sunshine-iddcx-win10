<h1 align="center">Void Drivers</h1>
<p align="center">
  Windows virtual drivers for <b>headless cloud-gaming hosts</b> - genuine-looking
  display, mouse, keyboard, and gamepad with no physical hardware attached.
</p>
<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue?style=for-the-badge" />
  <img src="https://img.shields.io/badge/status-pre--alpha-orange?style=for-the-badge" />
</p>

---

## About

**Void** is a pair of Windows drivers built to make a headless machine look like a
fully-equipped gaming PC to games and anti-cheat (Vanguard, EAC, BattlEye):

| Component | Tech | Runs in | What it does |
|---|---|---|---|
| **VoidDisplay** | IddCx UMDF | user mode | Adds virtual monitors for video capture / remote streaming |
| **VoidInput** | UDECX virtual USB | kernel | Hosts virtual USB mouse, keyboard, Xbox 360 / DS4 / DS5 gamepads, and touch |
| **libvoidrv** | C++ SDK (C ABI) | - | One library that drives both, via `voidrv.h` |
| **voidctl** | CLI | - | Command-line control + test harness |

Void is built from the ground up as clean, directly-controllable virtual hardware:
devices that persist on your terms, driven by a single SDK with no session coupling.

### Why it exists

- **Headless hosts have no GPU display output, no input devices.** Void supplies both
  as real device nodes, so capture APIs see a monitor and games see real controllers.
- **Real input, not `SendInput`.** VoidInput presents genuine USB devices (real
  VID/PID, true **XInput** for Xbox) built on the in-box UDECX framework, so input
  survives anti-cheat that blocks injected events.
- **No keepalive.** Void displays persist until you explicitly remove them - no
  per-display heartbeat or periodic ping to keep them alive.

## Design highlights

- **VoidDisplay** - up to 8 virtual monitors, default 1920x1080@60, custom `VVD`
  EDID, SDR first (HDR planned). Persists across sessions, no heartbeat.
- **VoidInput** - a UDECX virtual USB bus (`Root\Void\VUSB`) into which child devices
  plug. Child devices clone genuine descriptors so they're indistinguishable from real
  hardware; only the bus is Void-branded.
- **One control surface** - both drivers are driven through `DeviceIoControl`, wrapped
  by `libvoidrv` so host apps never touch raw IOCTLs.

## Status & roadmap

Pre-alpha - project scaffolding stage. Implementation order:

1. **VoidDisplay** (in progress) - virtual monitor + control IOCTLs.
2. **VoidInput** - UDECX bus -> HID mouse -> keyboard -> Xbox 360 -> DS4/DS5 -> touch.
3. **libvoidrv / voidctl** - SDK + CLI alongside each milestone.

## Building

Requires **Visual Studio 2022** + **WDK 10.0.26100** (with VS integration), x64.

```pwsh
# from a Developer PowerShell / Command Prompt
msbuild Void.sln /p:Configuration=Debug /p:Platform=x64
```

Drivers are **test-signed** for local development. Enable test-signing on the target:

```pwsh
bcdedit /set testsigning on   # then reboot
```

Production signing (EV cert + Microsoft attestation for the kernel driver) is a
deferred release task.

## Repository layout

```
void-display/   VoidDisplay (IddCx UMDF, C++)
void-input/     VoidInput  (UDECX KMDF, C)
libvoidrv/      SDK (voidrv.h)
voidctl/        CLI
docs/           Design docs
```

## License

MIT.
