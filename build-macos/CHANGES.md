# Engine releases

One section per release, written when the work lands. `publish-engine.sh`
uses the section for the version (`r<N>` for a release, `b<N>` for a beta) as the GitHub
release body.

## b1

The first public beta of Dormison: Wine 11.16 with wine-staging 11.16, plus the changes
Sevoflurane needs to run Windows Steam games on an Apple silicon Mac.

- `sevo-fpsunlock.exe`, Genshin Impact's frame-rate cap raised from beside the game: a two-megabyte
  program and stub DLL (genshin-fps-unlock's UnlockerStub, MIT) the app starts 30 seconds after the
  game appears, aiming for the frame rate set in Settings › Games, 120 by default.

- Graphics through DXMT (Direct3D 10/11), DXVK over MoltenVK, D3DMetal from Apple's Game
  Porting Toolkit 3.0 and 4.0 (the user's own copy), or wined3d, chosen per bottle or game.
- Resizable game windows that keep the game's resolution and aspect ratio, with a
  present-time upscaler for Metal, OpenGL and GDI windows: Lanczos, MetalFX Spatial, Anime4K
  and CuNNy, switched from the game's View menu.
- Dragging a game window's corner keeps the picture on screen through the drag: the stage
  scales the last frame, the presenter holds its output resolution, and the program hears of
  the new size once, when the drag ends.
- A frame-rate counter and frame-time graph from the View menu, and per-run frame traces the
  app reads.
- Raw mouse movement for games that hold the cursor for mouse-look.
- msync+ synchronization, Dormison's fork of CrossOver's msync, with a wineserver that commits
  WaitAll sets whole. When a process dies, wineserver wakes every object it held, so a game
  that exits in the middle of setting an event cannot leave steam.exe asleep on it; `SIGUSR2`
  to wineserver sweeps for any sleeper left on an available object, wakes it, and says which
  object held it and who shares it.
- Per-bottle and per-game settings read from env files at each launch, so a change needs no
  Steam restart.
- Each game's title and icon in the Dock, and native macOS NW.js for supported RPG Maker MV
  and MZ games.
- CoreAudio streams that apply WASAPI per-channel volume, and a GStreamer media back end.
- View › Resizable Windows switches scaling on and off in a running game, and a short notice
  over the picture says when the upscaler runs and when a window is too small for it to.
- Native full screen keeps its top-edge title bar and menu bar for every game, so it can
  always be left.
- A program that crashes prints a `sevo:crash` line before its own crash handler runs, so
  Sevoflurane can tell a crash from a quit.
- `SEVO_CPU_COUNT` caps the processors a game sees, which lets Unity 5-era job systems rest
  on Macs with many cores.
- An idle `winedevice.exe` wakes 4 times a second instead of 250: its SDL bus polls every
  250 ms while no controller is open.

`git diff wine-staging-base` is the complete change; the README's "Changes from Wine" lists
it by area.
