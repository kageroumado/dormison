# Known issues

Games and behaviors that do not work under Dormison as of the release named, with what is
known. Each entry moves out when a release fixes it; each will also feed the shared
compatibility database once that exists.

## HuniePop (Steam 339800) — black window in most launches — r15

Unity 4.2.2, 32-bit, Direct3D 9. The game loads for about 150 s (its own x87 audio decoding
under Rosetta), draws its first frame, and then in most launches stays black at full frame rate:
its managed startup never asks for its Steam plugin (`CSteamworks.dll`), and a script hits a
null object every frame. About one launch in fifteen reaches the title screen and plays; nothing
we can set makes the difference. Measured and cleared: the Steam client and its IPC (a probe
launched by Steam in the game's place initializes in 0.4 s on a server where the game fails),
msync, the loader, the environment, the save. Record: the `astra-huniepop-*` documents in the
app repository's `Docs/`. If it comes up black, quit and try again later.

## Where else

The app's `Docs/OPEN-ISSUES.md` holds engine leads that are not user-facing failures.
