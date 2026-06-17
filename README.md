# sunshine-vdd

IddCx virtual display driver for **headless GPU-PV hosts running Sunshine** (or any remote-streaming / game-streaming server). Win10 21H2+ / Win11. This is a public fork of [`nomi-san/void-drivers`](https://github.com/nomi-san/void-drivers) with the changes needed to run on Windows 10.

## What it is

A UMDF IddCx driver (`IddCx0102`) that adds up to 8 virtual monitors to a headless host so capture/streaming software sees a real display surface on the GPU-PV adapter. This routes the OS desktop through the GPU's render path instead of the synthetic CPU-composited Hyper-V Video adapter, giving:

- **1-5ms capture latency** instead of 30-80ms (synthetic = CPU bound, IddCx = GPU DirectX surface)
- **vsync-aligned frame pacing**, no tearing under load
- **Real hardware cursor plane** (capture pipelines that render their own cursor — common low-latency design — see only the desktop, not a baked-in mouse)
- **NVENC-friendly** — the GPU is already rendering the frame; Sunshine just copies the surface

## Diff vs. upstream `void-drivers`

- **Upstream-synced** at the fork origin (`sabihismail/sunshine-iddcx-win10` tracks `nomi-san/void-drivers`). Uses upstream's tiered build (`/p:IddTier=10|12|14`) so a single source produces three binaries:
  - **IddTier=14** (default, IddCx 1.4 / UMDF 2.25) — Win10 1903+ / Server 2022+ / Win11
  - **IddTier=12** (IddCx 1.2 / UMDF 2.19) — Win10 1709+ / Server 2019+ (manual dispatch)
  - **IddTier=10** (IddCx 1.0 / UMDF 2.15) — Win10 1607+ / Server 2016+ (manual dispatch)
- **Win10 install path** — cert import to `LocalMachine\Root` + `LocalMachine\TrustedPublisher`; no `testsigning` mode needed. See [`docs/`](./docs/) for the full Win10 install procedure.
- **Upstream CI** (`build-drivers.yml`) — `windows-2022` runner, WDK 10.0.26100, MSBuild, `SignMode=Off` (sign with the real cert at release time; CI artifacts are unsigned for inspection).

## When to use which tier

| Host | Tier | Notes |
|---|---|---|
| Win10 22H2, 21H2, 20H2, 2004 | **14** | In-box IddCx is 1.4. Default build. |
| Win10 1909, 1903 | **14** | In-box IddCx is 1.4. |
| Win10 1809, 1803 | **12** | In-box IddCx is 1.3 — needs the 1.2-compat tier. |
| Win10 1709 | **12** | In-box IddCx is 1.2. |
| Win10 1607 | **10** | In-box IddCx is 1.0. |
| Win11 / Server 2022+ | **14** | Default. |
| Server 2019 | **12** | |
| Server 2016 | **10** | |

## Build

```bash
# via GitHub Actions: Actions tab -> "Build drivers" -> Run workflow
# Manual dispatch has checkboxes for build_idd12 and build_idd10

# local build (default tier 14):
msbuild void-display\VoidDisplay.vcxproj /p:Configuration=Release /p:Platform=x64

# local build for older OS:
msbuild void-display\VoidDisplay.vcxproj /p:Configuration=Release /p:Platform=x64 /p:IddTier=10

# full suite (display + input + SDK + CLI):
msbuild void-display\VoidDisplay.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild void-input\VoidInput.vcxproj   /p:Configuration=Release /p:Platform=x64
msbuild libvoidrv\libvoidrv.vcxproj    /p:Configuration=Release /p:Platform=x64
msbuild voidctl\voidctl.vcxproj        /p:Configuration=Release /p:Platform=x64
```

`UmdfLibraryVersion` is stamped by `stampinf` from the `UMDF_VERSION_MINOR` per tier — no post-build patching needed.

## Install

```powershell
# 1. Trust the bundled test cert (in LocalMachine\Root + LocalMachine\TrustedPublisher)
Import-PfxCertificate -FilePath SunshineVDD.pfx -CertStoreLocation Cert:\LocalMachine\My -Password (Read-Host -AsSecureString)
# (or use the .cer via the GUI: double-click -> Install Certificate -> Local Machine -> Trusted Publishers)

# 2. Install the driver
pnputil /add-driver VoidDisplay.inf /install

# 3. Add a virtual display via IOCTL (or use the SDK from upstream void-drivers)
# Upstream: libvoidrv / voidctl - or roll your own IOCTL caller.
```

## Use with Sunshine

1. Install the driver (above) and reboot
2. Add a virtual display (e.g. `1920x1080@60`)
3. Set the new virtual display as the **primary** monitor in Windows Display Settings (right-click desktop -> Display settings)
4. The OS now renders the desktop to the GPU-PV adapter through the IddCx surface
5. Sunshine captures the surface; Moonlight on the client receives the stream

## Performance notes (vs. synthetic Hyper-V Video path)

| | Synthetic | IddCx (this) |
|---|---|---|
| Compositing | CPU (DWM host loop) | GPU (DirectX surface) |
| Latency | 30-80ms | 1-5ms |
| 1080p60 CPU use | 15-30% core | 1-3% (just copy) |
| Frame pacing | Teary under load | Vsync-aligned |
| Hardware cursor | No (baked in) | Yes (real plane) |
| Anti-cheat friendliness | Often flagged headless synthetic | Real display surface, AC happy |

## Credits

- Architecture, IOCTL design, EDID synthesis, hardware-cursor handling, persistence model, default mode table: [`nomi-san/void-drivers`](https://github.com/nomi-san/void-drivers) — MIT
- IddCx class extension + DDI: Microsoft
- Real-world deployment for headless game-streaming hosts: [`nomi-san/parsec-vdd`](https://github.com/nomi-san/parsec-vdd) (5.3k stars, also MIT)
- This fork: `sabihismail/sunshine-vdd` (public fork, MIT)
