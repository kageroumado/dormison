# Known issues

Games and behaviors that do not work under Dormison as of the release named, with what is
known. Each entry moves out when a release fixes it; each will also feed the shared
compatibility database once that exists.

## Every D3D12 game ends 6–45 s in with the upscaler on, on an M3 or later — r15, fixed in r16

`-[WineMacSwift.SevoDrawable waitOnCommandQueue:]: unrecognized selector` in the Wine log, the
game gone. On Apple GPU family 9 hardware D3DMetal drives a Metal 4 command queue, whose
`waitForDrawable:` and `signalDrawable:` re-send `waitOnCommandQueue:` and
`signalOnCommandQueue:` to the presenter's drawable; r15's drawable answered neither. D3D11
games and every Mac up to the M2 are unaffected. Until r16: the upscaler off for the game
(`sevo app config <appid> upscaler off`, or Settings › Games).

## A game freezes black and the Wine log grows by tens of MB a second — r15, fixed in r16

`msync: error: mach_vm_map failed with 3` then `wineserver crashed` in the log, then every
process repeating `err:sync:server_register_wait Failed to send server register wait:
0x10000003`. The server mapped a shared-memory page from an uninitialized address hint and
died; its clients retried against the dead server without bound. `sevo client stop`, then
end the leftover game process (`kill -9`), then `sevo client start`; the log file
(`~/Library/Logs/Sevoflurane-wine.log`) can be truncated.

## HuniePop (Steam 339800) — black window in most launches — r15

Unity 4.2.2, 32-bit, Direct3D 9. The game loads for about 150 s (its own x87 audio decoding
under Rosetta), draws its first frame, and then in most launches stays black at full frame rate:
its managed startup never asks for its Steam plugin (`CSteamworks.dll`), and a script hits a
null object every frame. About one launch in fifteen reaches the title screen and plays; nothing
we can set makes the difference. Measured and cleared: the Steam client and its IPC (a probe
launched by Steam in the game's place initializes in 0.4 s on a server where the game fails),
msync, the loader, the environment, the save. Record: the `astra-huniepop-*` documents in the
app repository's `Docs/`. If it comes up black, quit and try again later.

## WaitForMultipleObjects with WaitAll can succeed on a set that was never all signaled — msync, r17 and earlier, fixed in r18

With `WINEMSYNC=1`, WaitAll checked each object and then took each one. Nothing ordered those
steps against other threads' SetEvent, ResetEvent and ReleaseSemaphore calls, so two manual
events that are signaled one at a time, never together, could satisfy it. A synthetic fixture
counts 4–13 such grants per 20,000 contended waits on r17 and none on r18, where wineserver
freezes the whole set, grants it only when it is whole at one instant and consumes it itself.
No game is known to have failed from this. On an earlier engine, `WINEMSYNC=0` in the
bottle's `bottle.env` rules it out for a suspect game.

## Where else

The app's `Docs/OPEN-ISSUES.md` holds engine leads that are not user-facing failures.
