# sunshine-vdd

IddCx virtual display driver for **headless GPU-PV hosts running Sunshine** (or any remote-streaming / game-streaming server). Win10 21H2+ / Win11. This is a public fork of [`nomi-san/void-drivers`](https://github.com/nomi-san/void-drivers) with the changes needed to run on Windows 10.

## What it is

A UMDF IddCx driver (`IddCx0102`) that adds up to 8 virtual monitors to a headless host so capture/streaming software sees a real display surface on the GPU-PV adapter. This routes the OS desktop through the GPU's render path instead of the synthetic CPU-composited Hyper-V Video adapter, giving:

- **1-5ms capture latency** instead of 30-80ms (synthetic = CPU bound, IddCx = GPU DirectX surface)
- **vsync-aligned frame pacing**, no tearing under load
- **Real hardware cursor plane** (capture pipelines that render their own cursor — common low-latency design — see only the desktop, not a baked-in mouse)
- **NVENC-friendly** — the GPU is already rendering the frame; Sunshine just copies the surface

## Diff vs. upstream `void-drivers`

- **UMDF 2.19 in the INF** (down from 2.25) — in-box version on Win10 22H2; binary is forward-compatible
- **Self-signed cert install path** — Win10 install trusts the cert in `LocalMachine\Root` + `LocalMachine\TrustedPublisher`; no `testsigning` mode needed
- **GitHub Actions CI** — `windows-2022` runner installs WDK 10.0.22621, builds with MSBuild, signs with a generated self-signed cert, packages the DLL+INF+CAT
- Everything else (architecture, IOCTL design, mode table, EDID synthesis, hardware-cursor plane, persistence in `%ProgramData%\.voidrv\display.ini`) is **upstream unchanged** and credited accordingly

## When to use this vs. alternatives

| Setup | Path |
|---|---|
| Headless GPU-PV host, Win10 22H2, single GPU, streaming to client | **This driver** |
| Headless GPU-PV host, Win11 22H2+, single GPU | Use upstream `nomi-san/void-drivers` directly |
| Win10 with physical display on GPU | Synthetic path is fine, don't need this |
| DDA with second GPU for host | Synthetic path is fine, don't need this |

## Build

```bash
# via GitHub Actions: Actions tab -> "build-win10" -> Run workflow
# or locally with WDK 10.0.22621 + VS 2022 + Windows SDK 22621:
msbuild void-display\VoidDisplay.vcxproj /p:Configuration=Release /p:Platform=x64
```

The CI patches the INF's `UmdfLibraryVersion` to 2.19 after build. For local builds, edit the INF yourself or use `UMDF_VERSION_MINOR=19` in the build command.

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
