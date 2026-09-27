# sevo-steamstub — Steamworks for native NW.js games

The app runs NW.js games in native macOS NW.js: the dock shim execs `nwjs` in
place of the wine process, so Steam keeps counting the same pid as the game.
The native process is then outside the bottle and cannot call the game's
`steam_api.dll`.

This stub stays inside. It loads the game's Steamworks dll, calls
`SteamAPI_Init`, and answers newline-delimited JSON on loopback. The native
half is the app's `greenworks.js`, which presents the greenworks API to the
game and forwards each call here.

## Build

```
make            # both bitnesses
make 64         # sevo-steamstub.exe
make 32         # sevo-steamstub32.exe
```

Needs `mingw-w64`. Two bitnesses because the stub loads the game's own dll into
its own address space: RPG Maker MV ships 32-bit `steam_api.dll`, MZ and
Unreal titles ship 64-bit `steam_api64.dll`. The shim only ever spawns
`sevo-steamstub.exe`. The stub reads the machine field of the dll's PE header
before loading it: a 32-bit dll sends the whole invocation to
`sevo-steamstub32.exe` beside it, once, with `SEVO_STEAM_STUB_HANDOFF=1` in the
sibling's environment so it never hands back; a dll of neither machine ends
with one log line naming the file and its machine. Both exes must be installed
in one directory, or the handoff cannot find its sibling.

## Running it

```
SteamAppId=<appid>                 required; Steamworks initializes against it
SEVO_STEAM_API_DIR=<windows path>  where to look for the dll
SEVO_STEAM_STUB_PORT=<port>        default 27060
SEVO_STEAM_STUB_IDLE=<seconds>     wait for a first client, default 300
argv[1]                            game directory, used when SEVO_STEAM_API_DIR is unset
```

The app gives each game its own port and passes it as `SEVO_STEAM_STUB_PORT`
in the game's `.sevo/apps/<exe>.env`; the native half reads the same file. It
compares the `appId` in the `init` reply against its own and hangs up on a
mismatch, so a port collision costs a game its achievements rather than giving
it another game's.

The dll search looks for `steam_api64.dll` then `steam_api.dll` in that
directory and four levels below it, so pointing at the game root is enough for
the usual layouts (beside `Game.exe`, under `www/`, beside the greenworks
`.node`).

The stub writes the port it actually bound to
`%LOCALAPPDATA%\Sevoflurane\steamstub-<appid>.port` and a transcript to
`steamstub-<appid>.log` beside it. From macOS both are under
`<prefix>/drive_c/users/<user>/AppData/Local/Sevoflurane/`. The port file is
deleted on exit.

It exits on `{"op":"quit"}`, when the last client that had connected drops, or
after the idle timeout with no client at all.

## Protocol

One JSON object per line in, one per line out. Every reply carries `ok`;
failures carry `error`.

| request | reply |
|---|---|
| `{"op":"ping"}` | `{"ok":true}` |
| `{"op":"init"}` | `{"ok":true,"steamId":"765…","appId":480,"language":"english","personaName":"…"}` |
| `{"op":"activateAchievement","name":"ACH_1"}` | `{"ok":true}` after `SetAchievement`+`StoreStats` |
| `{"op":"getAchievement","name":"ACH_1"}` | `{"ok":true,"achieved":false}` |
| `{"op":"clearAchievement","name":"ACH_1"}` | `{"ok":true}` after `ClearAchievement`+`StoreStats` |
| `{"op":"getAchievementNames"}` | `{"ok":true,"names":["ACH_1",…]}` |
| `{"op":"getNumberOfAchievements"}` | `{"ok":true,"count":5}` |
| `{"op":"setStat","name":"N","value":3}` | `{"ok":true}` — the stat's INT or FLOAT type is read from Steamworks (the typed getter that answers), and the matching setter is called; when neither getter answers yet, an integral value goes to `SetStatInt32` and a fractional one to `SetStatFloat`, and the log says so |
| `{"op":"getStat","name":"N"}` | `{"ok":true,"value":3}` |
| `{"op":"getStatFloat","name":"N"}` | `{"ok":true,"value":3.500000}` |
| `{"op":"storeStats"}` | `{"ok":true}` |
| `{"op":"quit"}` | `{"ok":true}`, then the stub exits |

The reader scans for `"op"`, `"name"` and `"value"` in a flat object; nesting
and arrays in a request are ignored.

## Testing it by hand

Run it in the live bottle the way any wine probe runs there. The running
wineserver is msync, so a process without `WINEMSYNC=1 WINEESYNC=1` exits
silently:

```bash
E="$HOME/Library/Application Support/Sevoflurane/Engines/<engine>"
P="$HOME/Library/Application Support/Sevoflurane/Bottles/Steam"
env WINEPREFIX="$P" PATH="$E/wine/bin:/usr/bin:/bin" WINEMSYNC=1 WINEESYNC=1 \
    WINEDEBUG=-all SEVO_ENV_FILES=0 \
    DYLD_INSERT_LIBRARIES="$E/libsevodockshim.dylib" SEVO_SUPPRESS_WINDOWS=1 \
    SteamAppId=480 \
    SEVO_STEAM_API_DIR='C:\Program Files (x86)\Steam\steamapps\common\<game>\<the folder holding steam_api64.dll>' \
    "$E/wine/bin/wine" "$E/sevo-steamstub.exe" &

printf '{"op":"init"}\n{"op":"getAchievementNames"}\n{"op":"quit"}\n' | nc -w 3 127.0.0.1 27060
```

Steam must be running. App id 480 is Spacewar, the Steamworks sample: it has
achievements and stats defined, so it exercises the paths that a game without
achievements cannot.
