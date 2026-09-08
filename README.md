# dormison

The Wine engine inside [Sevoflurane](https://github.com/kageroumado/sevoflurane),
the app that runs Steam and its Windows games as a Mac app.

This repository is [Wine](https://www.winehq.org) 11.16 with
[wine-staging](https://github.com/wine-staging/wine-staging) 11.16 applied
(tag `wine-staging-base`), plus the changes that turn it into the engine.
`git diff wine-staging-base` shows all of them. The engine is built for
x86_64 macOS and runs under Rosetta on Apple silicon. Sevoflurane downloads
it as a tarball and runs the Windows Steam client and every game through it.

Like the app, it is named after an anesthetic.

## Wine, CrossOver, Dormison

| | Wine | CrossOver | Dormison |
|---|---|---|---|
| Price | Free | Paid, per seat | Free |
| Who makes it | The Wine project | CodeWeavers, a company with a support team and per-game fixes | One developer, for Sevoflurane |
| Support | Bugzilla and the forums | Support tickets and a compatibility database | GitHub issues and pull requests. It is a free project and makes no guarantees. |
| License | LGPL | Proprietary, with Wine's LGPL parts published | LGPL, like Wine |
| Direct3D 12 on macOS | vkd3d over MoltenVK | Apple's D3DMetal, integrated | Apple's D3DMetal, hosted by the engine and installed by the app |
| Direct3D 9 to 11 | `wined3d` over OpenGL | D3DMetal, DXMT, DXVK | DXMT, DXVK, `wined3d`, per bottle or per game |
| Synchronization | wineserver round trips | msync | msync |
| 32-bit games under Rosetta | Thunk and signal bugs | Fixed | Fixed the same way |
| The Steam client | Runs; its boot trips over Vulkan enumeration and the network wait | Runs | Runs, with the boot fixed |
| Frames on screen | As the program draws them | As the program draws them, with a high-resolution mode per bottle | Through a presenter: Lanczos, MetalFX or a shader package (Anime4K, CuNNy), and a resizable window for programs that lock theirs |
| Per-game settings without a restart | — | — | Env files read at process start |
| Mouse | Accelerated pointer | Accelerated pointer | Raw deltas for a program that holds the cursor |
| The Dock | Every process is "wine" | The bottle's icon | The game's name and icon per process |
| Without Rosetta | No macOS build | An ARM64 preview, without D3DMetal yet | Runs under Rosetta |

## What it adds to Wine

| Area | Upstream Wine on macOS | This engine |
|---|---|---|
| Direct3D 12 | vkd3d over MoltenVK, incomplete | Hosts Apple's D3DMetal (the Game Porting Toolkit's Direct3D 12 layer): GS base kept on the thread's TSD with the TEB and PEB mirrored, the `__wine_unix_call` export, ms_abi wrappers for the syscalls the toolkit's native threads make, `win32u.so` mapped before the toolkit resolves its symbols, and the host-callback table D3DMetal 4.0 works from. Apple's libraries are installed by the app; only Apple distributes them. |
| Direct3D 9 to 11 | `wined3d` over macOS OpenGL | The engine tarball carries [DXMT](https://github.com/3Shain/dxmt) (Direct3D 10/11 to Metal, 64- and 32-bit) and [DXVK](https://github.com/Gcenx/DXVK-macOS) over MoltenVK beside `wined3d`; the renderer is chosen per bottle or per program. |
| Synchronization | Every wait is a wineserver round trip | msync, CrossOver's in-process synchronization on Mach semaphores (`WINEMSYNC=1`). |
| 32-bit programs under Rosetta | The 32-to-64 thunk breaks on a signal; code written by another process or on executable pages runs stale; signal contexts lack an mxcsr | The thunk returns through `lretq`; written code is re-translated; a default mxcsr is supplied and four mistranslated instructions are handled. 32-bit Direct3D 11 games reach Metal through DXMT. |
| Presentation | The frame is drawn as the program sizes it | A presenter puts the frame on screen through its own Metal layer and scales it on the way: Lanczos, MetalFX, or an mpv user-shader package (fragment and compute passes, so Anime4K and CuNNy run). A final filter resamples the last pass. GDI window surfaces go through the same path. `Upscaler`, `FinalFilter`, `PresenterLog` options; `SEVO_UPSCALER`, `SEVO_FINAL_FILTER`, `SEVO_SHADER_DIR` in the environment. |
| Windows the program locked | A non-resizable window stays that size | `ResizableWindows` lets the user resize it, and lets a full-screen-style window run in a window, while the program keeps drawing at its own size. The aspect ratio is held; the cursor clip waits for the cursor to hide. |
| Mouse | Accelerated pointer deltas | `LinearMouse` hands a program that holds the cursor the mouse's own displacement. |
| Per-program settings | The environment is inherited from the parent | `<prefix>/.sevo/bottle.env` and `<prefix>/.sevo/apps/<exe>.env` are read at process start, so a setting reaches a game at its next launch while the Steam client keeps running. `WINEDLLPATH_PREPEND` puts a directory ahead of the built-in DLLs. |
| GPU identity | A generic adapter and an invented driver version | The adapter, memory, driver version and provider a game sees come from the environment (`SEVO_GPU_*`); an unknown vendor reads as a current NVIDIA card. |
| The Steam client's boot | Every CEF GPU process dies on Vulkan enumeration; the client waits for a network change; a display-mode switch crashes on Apple silicon | Vulkan portability enumeration, the network interface's first registration completed and its address change primed, the safe display-mode flag, a trimmed root-device list, a reentrant `localtime`. |
| Processes on macOS | Every process shows as "wine" and may take a Dock icon | The dock shim (`libsevodockshim.dylib`) keeps Steam's infrastructure processes out of the Dock, names the game process after the game with Steam's title and a shaped Dock tile, runs NW.js games as native processes, and can start a game through a loader of its own. |
| Steamworks outside the bottle | — | A stub (`sevo-steamstub.exe`) loads the game's `steam_api.dll` inside the bottle and answers achievements and stats over loopback for a game that runs natively. |

Left out on purpose: X11, OpenGL, GStreamer, FFmpeg, SDL, CUPS and the
other desktop integrations. The engine runs games under Steam.

## DirectX 12, measured

Microsoft's DirectX-Graphics-Samples were built on Windows with a frame-count
exit and run on Dormison with D3DMetal 4.0 beta 2, then compared frame by frame
with the same binaries on an RTX 4080 SUPER. Of 35 comparable samples, 23 draw
the reference image.

| Works | Does not |
|---|---|
| The HelloWorld set: window, triangle, texture, constant buffers, bundles, frame buffering | DirectX Raytracing, all four samples. The toolkit reports no raytracing tier. |
| Execute indirect, multithreading, predication queries, reserved resources, residency, small resources, depth bounds, dynamic indexing, n-body gravity | Variable Rate Shading. No tier is reported and the sample gives up. |
| Mesh shaders: meshlet render, cull, instancing | Mesh shaders: dynamic LOD draws the mesh as shards. |
| Full screen, linked-GPU samples on one adapter | Cross-GPU copy. One adapter is exposed. |
| | Pipeline state cache, generic programs, 11-on-12. They abort at the first Direct3D 12 call. |
| | HDR and SM6 wave intrinsics. They stop on their own error dialog. |

The toolkit reports feature level 12_2, resource binding tier 3, heap tier 2,
enhanced barriers, wave operations and mesh shaders, the same as the RTX
card. Shader Model is 6.6 against 6.8, root signature 1.1 against 1.2, tiled
resources tier 2 against 4. Conservative rasterization, sampler feedback,
double-precision shaders and raytracing are absent. The Agility SDK redirect
games ship is never honored; the engine's own `d3d12.dll` answers every time.

## Building

`build-macos/README.md` is the build guide: toolchain, configure line, the
driver's Swift half, and `package-engine.sh`, which assembles an engine
directory with `engine-info.json`. `build-macos/` also holds the dock shim,
the Steamworks stub, CrossOver's msync sources and the port that placed
them, and patches that exist but are not applied.

## Releases

Each engine release is a tag `r<N>` here and a GitHub release carrying the
tarball, its checksum, its `engine-info.json` and the full diff against
`wine-staging-base`. The app finds releases through the manifest published
with [Sevoflurane's releases](https://github.com/kageroumado/sevoflurane/releases).

## License

Wine's: LGPL 2.1 or later, see `LICENSE`. The fonts under `fonts/` carry
their own licenses (`COPYING.*`). msync is CrossOver's, LGPL. The presenter,
the dock shim and the Steamworks stub are © 2026 kageroumado, LGPL 2.1 or
later.
