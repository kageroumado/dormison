# Dormison

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
| Input | Use raw mouse movement when a game holds the cursor for mouse-look. |
| Game settings | Change per-game settings for the next launch while Steam stays open. |
| Mac integration | Show a game's title and icon in the Dock, and run supported NW.js games with native macOS NW.js. |

Compatibility depends on the game, Mac and graphics translator. See the
[DirectX 12 testing notes](#directx-12-testing) for what the sample runs establish.

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
`git diff wine-staging-base` shows the complete diff.

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
(`FrameRate=Y` or `SEVO_FPS=1` shows it from the first frame) and Show
Picture Details.

`ResizableWindows` preserves the game's rendering size and aspect ratio
while allowing the presentation window to resize. `LinearMouse` supplies
raw displacement when the program holds the cursor for mouse-look.

### Processes and compatibility

Dormison includes CrossOver's msync implementation, using Mach semaphores
for in-process synchronization (`WINEMSYNC=1`). Rosetta workarounds cover
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

### DirectX 12 testing

[Bispectral's sample results](https://github.com/kageroumado/bispectral/blob/main/dx12-samples/RESULTS.md)
record Microsoft DirectX-Graphics-Samples runs and comparisons with an
RTX 4080 SUPER. The archived run combines several invocations and lacks
verified engine provenance. Its captures are unsynchronized, and its
`match` classification records matching exit status and window behavior.

Feature support also depends on the Mac and toolkit version. A sample
completing its run establishes less than a full game playing correctly;
use the individual results and their recorded limitations when assessing
compatibility.

## License

Wine's: LGPL 2.1 or later, see `LICENSE`. The fonts under `fonts/` carry
their own licenses (`COPYING.*`). msync is CrossOver's, LGPL. The presenter,
the dock shim and the Steamworks stub are © 2026 kageroumado, LGPL 2.1 or
later.
