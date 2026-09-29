# sevo-fpsunlock

Genshin Impact holds itself to 60 fps on PC. This raises the cap from beside the game, the
way genshin-fps-unlock does, in two megabytes instead of a .NET desktop application.

- `sevo-fpsunlock-stub.dll` is genshin-fps-unlock's `UnlockerStub` (MIT, `stub/LICENSE`),
  built here with mingw-w64 and linked static. Loaded into the game, it finds the frame-rate
  variable in the il2cpp code by pattern and writes the target into it every 62 ms.
- `sevo-fpsunlock.exe` (`sevo_fpsunlock.c`) gets it there: `sevo-fpsunlock.exe <fps> [wait]`
  waits for the game's Unity window, creates the shared memory block the stub opens by
  name (its `IPCData`: status, frame rate, two flags), and sets a `WH_GETMESSAGE` hook on the
  window's thread with the stub's exported `WndProc`, which makes Windows load the stub into
  the game. It keeps the target written and leaves when the game does.

It runs in the game's own prefix, on the game's engine and with its environment — a Wine
process sees only its own wineserver's processes — and only once the game has drawn: an
unlocker alive while the game initialises makes it quit. Sevoflurane's daemon starts it that
way (`SevofluraneDaemon/ProgramLaunch.swift`).

When a game update moves the code the stub looks for, the stub answers `Error` and the
program says "the stub found no frame-rate cap in this game build": take the patterns from
upstream's `UnlockerStub/dllmain.cpp` again.

```sh
make            # both, stripped; needs `brew install mingw-w64`
```
