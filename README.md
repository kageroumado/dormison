# Dormison

[English](README.md) · [简体中文](README.zh-CN.md)

Dormison is the Wine engine used by [Sevoflurane](https://github.com/kageroumado/sevoflurane)
to run Windows Steam games on macOS.

Sevoflurane downloads the engine and manages its settings. Start with
[Sevoflurane's setup instructions](https://github.com/kageroumado/sevoflurane#get-started)
to use it. Dormison is built for x86_64 macOS and runs under Rosetta on
Apple silicon.

Like the app, it is named after an anesthetic.

## What it does

| Feature | What you can use |
|---|---|
| Graphics | DXMT for Direct3D 10/11, DXVK over MoltenVK, D3DMetal from Apple's Game Porting Toolkit, or Wine's wined3d. Choose a renderer per bottle or game in Sevoflurane. |
| Window scaling | Resize supported game windows while the game keeps drawing at its original resolution. Scale the picture with Lanczos, MetalFX Spatial, Anime4K or CuNNy. |
| Frame rate | Show a frame-rate counter or a frame-time graph from the game's View menu. |
| Input | Use raw mouse movement when a game holds the cursor for mouse-look. |
| Game settings | Change per-game settings for the next launch while Steam stays open. |
| Mac integration | Show a game's title and icon in the Dock, and run supported NW.js games with native macOS NW.js. |

Compatibility depends on the game, Mac and graphics translator.

## Build or contribute

The [build guide](build-macos/README.md) covers the toolchain, configuration,
packaging and releases. [CONTRIBUTING.md](CONTRIBUTING.md) explains how to
report a problem and verify a change.

Each release has an `r<N>` tag and includes the engine tarball, checksum,
Ed25519 signature, `engine-info.json` and the diff against
`wine-staging-base`. Sevoflurane finds engines through its signed release
manifest.

## Technical details

This repository is [Wine](https://www.winehq.org) 11.16 with
[wine-staging](https://github.com/wine-staging/wine-staging) 11.16 applied
(tag `wine-staging-base`), plus Dormison's changes.
`git diff wine-staging-base` shows the complete diff, and
[CHANGES.md](build-macos/CHANGES.md) describes each release.

### Changes from Wine

Everything below is absent from Wine 11.16 and wine-staging 11.16.

**Graphics**
- D3DMetal host support for Game Porting Toolkit 3.0 and 4.0, including 4.0's
  host-callback table.
- A present-time upscaler for Metal, OpenGL and GDI windows (Lanczos, MetalFX
  Spatial, Anime4K, CuNNy), with a final filter and an in-game View menu.
- wined3d hands the upscaler the game's own frame, at its own resolution.
- Resizable game windows that keep the game's resolution and aspect ratio.
- An FPS counter and frame-time graph, fed by a per-process stats page that
  the app also reads.
- `SEVO_GPU_*` adapter identity, plus RTX 50 and RX 7000/9000 device IDs in
  wined3d.
- Vulkan portability enumeration on, so Windows programs see MoltenVK.
- On a Metal 4 GPU (M3 and later), a present with display sync off returns at
  once, and the upscaler encodes one frame at a time.

**Windows, input and the Mac**
- Each game gets its own Dock tile with its Steam title and icon. Steam's
  tools, and any process started with `SEVO_QUIET=1`, stay out of the Dock and
  off the screen.
- A game's Dock tile or Finder copy, opened on its own, starts the game
  through Sevoflurane.
- NW.js games run on native NW.js, with a Steamworks stub for achievements
  and stats that loads 32- and 64-bit Steamworks alike.
- Cursor confinement through the window server; raw mouse movement for
  mouse-look.
- A "not responding" sheet for a game that ignores quit, and crash reports
  for faults on threads Wine did not create.
- Tray icons can stay out of the menu bar. A background program no longer
  keeps the display awake.

**Processes, sync and memory**
- msync: Mach-based in-process synchronization (`WINEMSYNC=1`).
- Critical sections leave the lock available on release, as on Windows: a
  running thread may take the lock back before a woken waiter arrives, and
  `LockCount` reads as Windows encodes it.
- Per-program settings files, read at every process start.
- A native arm64 wineserver.
- Fixes for Steam's 20-second network wait and for Unity/Mono TLS reads.
- `SEVO_LARGE_ADDRESS_AWARE` for 32-bit games.

**Audio, media and text**
- Bundled GStreamer (LGPL build) for Media Foundation and DirectShow movies,
  MPEG program streams included.
- CoreAudio applies each stream's volume, channel by channel, to its own
  samples, so other programs' playback and the Mac's volume stay intact and a
  muted capture records silence.
- A Discord Rich Presence bridge.
- Japanese font families mapped onto the bundled IPAGothic/IPAPGothic and the
  Mac's Hiragino Mincho.

**Release**
- `wine --version` names the release.
- Releases are signed, and each ships its diff against `wine-staging-base`
  and a manifest of every file with its sha256 and origin. Packaging refuses
  a driver or server that does not match the sources.

### Compared to CrossOver

This comparison is against the CrossOver 26.3 source release, which is
Wine 11.0 with CodeWeavers' patches and no wine-staging.

**Taken from CrossOver**
- msync, with fixes CrossOver 26.3 lacks:
  - WaitAll is granted by wineserver under a freeze of the whole set, so a
    grant is a set that was whole at one instant and is consumed once, before
    the waiting thread returns; a query never reads the set half consumed.
  - WaitAll reports an abandoned mutex.
  - `NtReleaseMutant` reports NT's previous count.
  - Freed object indexes are reused from a stack.
  - A process exits when its wineserver dies.
- Rosetta workarounds: the `lretq` 32→64 transition, MXCSR restore,
  XGETBV/CET/debug-register handling, and retranslation of written code.
  Dormison's cross-process write invalidation walks the range region by
  region, where CrossOver restores the first page's protection over the whole
  range.
- D3DMetal glue: `__wine_unix_call`, the Microsoft-ABI trampoline, and the
  GS base kept on Darwin thread storage with the TEB and PEB mirrored.
- The Mono TLS mirror at `%gs:0x1780`. Dormison also reserves the pthread key
  behind that offset.

**In Dormison, not in CrossOver 26.3**
- Wine 11.16 and wine-staging.
- The upscaler, the D3DMetal 4.0 callback table, and the Metal 4 present
  ordering, with presents that do not wait when display sync is off.
- Critical sections that leave the lock available on release, where CrossOver,
  like Wine, hands the lock to the next waiter.
- Per-channel stream volume applied to the samples; CrossOver sets it on the
  device, which moves the Mac's volume.
- A native arm64 wineserver.
- The Steam network-wait fix, Vulkan portability enumeration and
  `SEVO_GPU_*`.
- Window-server cursor confinement.
- `localtime_r` in ntdll, which the PEB mirror needs to be safe.
- The Dock shim, NW.js runner, Discord bridge, stats page and per-program
  settings.

**In CrossOver 26.3, not in Dormison**
- A 32-bit-only bottle mode under WoW64, and Rosetta's 16-bit LDT fix.
- wined3d on Vulkan by default for Direct3D 10/11.
- Per-title fixes: GTA IV/V, Counter-Strike 2, Cities: Skylines II, and BC7
  decoding for Tomb Raider I–III Remastered.
- Launcher fixes for Epic, Battle.net, Rockstar, GOG Galaxy and Ubisoft
  Connect.
- PlayStation controller rumble over Bluetooth, an Xbox 360 USB bus, and the
  microphone permission prompt.
- CrossOver's desktop and Office integration.

### Graphics and presentation

The engine packages [DXMT](https://github.com/3Shain/dxmt) for 64-bit and
32-bit Direct3D 10/11 programs, and [DXVK](https://github.com/Gcenx/DXVK-macOS)
over MoltenVK. Sevoflurane can import D3DMetal from Apple's Game Porting
Toolkit.

D3DMetal support includes the `__wine_unix_call` export, Microsoft-ABI
wrappers for toolkit callbacks, and the host-callback table used by
D3DMetal 4.0. Wine keeps the GS base on Darwin thread storage, mirrors the
TEB and PEB, and maps `win32u.so` before the toolkit resolves its symbols.

The optional presenter scales Metal drawables, OpenGL window drawables and
GDI window surfaces. An OpenGL drawable is a framebuffer object whose swap
lands in an IOSurface the presenter reads, which is what puts the upscaler
over wined3d's Direct3D 9; multisampled, stereo, floating-point and 10-bit
drawables stay on an OpenGL view, and `OpenGLPresenter=N` keeps every one
there. The presenter supports Lanczos, MetalFX Spatial and compiled mpv
shader packages, including fragment and compute passes used by Anime4K and
CuNNy. A final filter resamples the output. Configure it with `Upscaler`,
`FinalFilter` and `PresenterLog`, or `SEVO_UPSCALER`, `SEVO_FINAL_FILTER`,
`SEVO_PRESENTER_LOG` and `SEVO_SHADER_DIR`.

A running program has a View menu: Upscaler, Final Filter, Show Frame Rate
(`FrameRate=Y` or `SEVO_FPS=1` shows it from the first frame), Show Frame
Time Graph (`FrameRateGraph=Y` or `SEVO_FPS_GRAPH=1`) and Show Picture
Details. The counter and graph read the same per-process stats page as
Sevoflurane.

`ResizableWindows` preserves the game's rendering size and aspect ratio
while allowing the presentation window to resize. `LinearMouse` supplies
raw displacement when the program holds the cursor for mouse-look.

### Processes and compatibility

Dormison includes CrossOver's msync implementation, which synchronizes in
process through shared memory, `__ulock` waits and a Mach port to wineserver
(`WINEMSYNC=1`). It is maintained as commits on `main`. Rosetta workarounds cover
32-to-64-bit transitions, written executable code and signal contexts.
The transition uses `lretq`, and signal handling preserves the thread's
MXCSR state.

At process start, Wine reads `<prefix>/.sevo/bottle.env`, then
`<prefix>/.sevo/apps/<exe>.env`, where `<exe>` is the executable's
lowercase basename. Game settings therefore apply at the next launch.
`WINEDLLPATH_PREPEND` adds a directory ahead of the built-in DLLs.

`SEVO_GPU_*` variables set the adapter identity, memory and driver
metadata reported to Windows programs. Without a driver override, an
unknown vendor receives NVIDIA driver metadata.

Steam startup fixes cover Vulkan portability enumeration, initial network
notifications, display-mode switching, root-device enumeration and
reentrant local-time conversion.

The dock shim (`libsevodockshim.dylib`) hides Steam's infrastructure
processes from the Dock and gives games their own titles and icons.
For supported NW.js games, it starts the native runtime. The
[Steamworks stub](build-macos/steam-stub/README.md) stays in Wine and
serves achievements and stats to the native game over loopback.

## License

Wine's: LGPL 2.1 or later, see `LICENSE`. The fonts under `fonts/` carry
their own licenses (`COPYING.*`). msync is CrossOver's, LGPL. The presenter,
the dock shim and the Steamworks stub are © 2026 kageroumado, LGPL 2.1 or
later.
