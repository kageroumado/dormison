# Steam bridge — Steamworks for native macOS games

A native macOS game installed through Sevoflurane talks to the Windows Steam client
running in the bottle. Nothing of Steam for Mac is used: no second sign-in, no second
client. This directory holds the pieces that make that work.

## The chain, end to end

1. **Steam Play in the Windows client** (`dlls/ntdll/loader.c`, `sevo_enable_steam_play`).
   With `SEVO_STEAM_PLAY=1` in `steam.exe`'s environment, the loader flips CCompatManager's
   enabled flag (`strcmp(platform, "linux") == 0` → true) as `steamclient64.dll` is mapped.
   Only in `steam.exe`. A game mapped to the app's tool `sevoflurane_macos` (`from_oslist
   macos`, `to_oslist windows`) then gets its macOS depots on install and update, and Steam
   swaps depots by itself when the mapping changes.
2. **Launch** (`../dock-shim/sevo_dock_shim.c`, `sevo_run_native_app`). Steam shell-opens
   the game's `.app`, an `explorer.exe "<…>\Game.app"` process; the tool's `commandline`
   prefix (`sevo-native.exe`) is never run by the Windows client. A launch entry that names
   the executable inside the bundle (`Game.app\Contents\MacOS\Game`) reaches the same place:
   kernelbase's `create_macos_build_process` turns a Mach-O or `#!` file under
   `steamapps\common\…\*.app\Contents\MacOS\` into `explorer.exe "<that exact path>"`, and
   ntdll's `is_steam_macos_build` keeps Wine's own `fork_and_exec` off such files (it would
   run them bare once they are executable). Both match case-insensitively with either
   separator, as the volume does. The dock shim's constructor recognizes that process,
   finds the app id in the library's `appmanifest_*.acf`, confirms Steam maps the app to the
   macOS tool, gives the bundle's Mach-O and script files their executable bit (the Windows
   client writes 0644), starts the helper (4) with stdio and nothing else, and forks a child
   that execs **`sevo-native-supervisor`** (`../dock-shim/sevo_native_supervisor.c`,
   native-arch through `posix_spawn` with an arm64 binary preference, since a translated
   parent would otherwise hand a universal binary its x86_64 slice). The supervisor holds
   the write end of the status pipe alone, spawns the game (the exact executable, or the
   bundle's `CFBundleExecutable`) as the leader of a new process group with a clean
   environment — `HOME`, `USER`, `TMPDIR`, a plain `PATH`, the `Steam*` variables of the
   launch, and the bridge's own (`DYLD_INSERT_LIBRARIES` = `libsevosteamipc.dylib`,
   `SEVO_STEAM_BRIDGE_DIR/PORT_FILE/TOKEN/PREFIX`) — cwd the bundle's parent and no file
   descriptor but 0–2, and publishes the session in
   `<prefix>/.sevo/native-sessions/<supervisor pid>.json` (`appid`, `supervisor`, `game`,
   `pgid`, `executable`, `bundle`, `started`). The process Steam created goes on as wine
   running `sevo-native.exe --wait` on the pipe's read end, so Steam's launch action
   completes (`CreatingProcess → WaitingGameWindow → Completed`) and Steam's tracked game
   process lives exactly as long as the session. The session lasts while the game's process
   group has members, so a game that execs itself or a launcher that hands off to a child
   stays one session. When the waiter dies (Steam's Stop, the wineserver ending with an app
   quit or engine switch) or the supervisor gets SIGTERM (the app's stop), it TERMs the
   group, waits 10 s and KILLs the rest. None of this needs the game to load anything: a
   bundle with the hardened runtime and no `allow-dyld-environment-variables` entitlement
   is logged, runs without Steam (`DYLD_*` is stripped), and is supervised the same way.
   Without the bridge files in the engine, the game is still run, bare.
3. **Path lookup** (`mac/sevo_steam_ipc.c` → `libsevosteamipc.dylib`). Answers libsteam_api's
   `bootstrap_look_up` of `com.valvesoftware.steam.ipctool` from a thread inside the game:
   path `$SEVO_STEAM_BRIDGE_DIR/steam_osx`, pid = the game's own. libsteam_api then dlopens
   `$SEVO_STEAM_BRIDGE_DIR/steamclient.dylib`.
4. **The bridge.** `steamclient.dylib` (macOS, universal arm64+x86_64, `mac/` + generated
   proxies) forwards every interface call over loopback TCP to `sevo-steambridge.exe`
   (x86_64 PE, mingw, `win/` + generated handlers), which runs in the bottle, loads the
   running client's `steamclient64.dll` and makes the same call on the same interface
   version.

## Measured 2026-10-10 on macbook-16

From Steam's own Play (Sevoflurane 1.1 beta 1 with the test tool, engine
`dormison-b2-bridge`): the game action completes, Steam tracks `sevo-native.exe --wait`
as the game, Escape Dungeon 2 runs ARM64 as its child with the bridge connected and stays
up; `sevo app terminate 1309000` ends waiter, game and helper, and a second launch follows
cleanly. `SteamClient.Apps.TerminateApp(1309000, false)` from the app's page did nothing
(an app-side question; its own stop path works).

Escape Dungeon 2 (1309000, Unity, Steamworks.NET 20.x, SDK 1.53), launched natively by
hand with the helper started in the bottle: `SteamAPI_Init` passes; Steamworks.NET's
context fetches all 24 interfaces; the game runs at frame rate (`RunFrame` ×3 per frame
over the wire); its saves go through Remote Storage (`FileWrite`, `FilePersisted`). The
probe (`tests/probe.c`, driving the game's own libsteam_api through the flat API) passes
every check: SteamID and persona name, `GetAppInstallDir` as a macOS path, a Remote
Storage write/read/delete round trip, `UserStatsReceived_t` converted (game id and result
right), the 26-achievement list, `SetAchievement` + `StoreStats` with its
`UserAchievementStored_t`, `ClearAchievement` restoring the account. Both sides' logs are
clean of unknown methods and short frames.

## Facts the bridge rests on

- **Steam's launch needs the process it created to live.** With the game exec'd in place
  of `explorer.exe`, that process vanished from wineserver at the exec, Steam's game action
  stayed at `CreatingProcess`, and no later launch of any game started until the client
  restarted (or `SteamClient.Apps.CancelGameAction(<id>)`). Hence the fork and the waiter.
- **The client's environment is not a game's.** The app hands wine processes a built
  environment (no `HOME`/`USER`/`TMPDIR`, wine's `PATH`, the renderer's variables); Escape
  Dungeon 2 inherited it and died in Unity's text renderer three seconds after init, on
  both architectures. With the Finder-style environment above it runs.
- **Overwriting an engine dylib in place while processes map it** invalidates the vnode's
  cached code signature: every later process loading it dies `SIGKILL (Code Signature
  Invalid)` in dyld, and the client restarts in a silent loop. Install through a new inode
  (`cp` to a temp name, `mv`).

- **Load path in libsteam_api (SDK 1.5x–1.6x):** `SteamAPI_Init` asks ipcserver for the
  path (fails → "ipcserver GetSteamPath failed"), checks the pid with `kill(pid, 0)`, cuts at
  the last `/`, appends `steamclient.dylib`, dlopens, dlsyms `CreateInterface`, asks for
  `SteamClient0xx`, then `CreateSteamPipe`, `ConnectToGlobalUser`, `GetISteamUser`, … A
  "local steamclient" branch exists but is gated by a flag only server builds set.
- **Exports libsteam_api resolves from steamclient:** `CreateInterface`,
  `Steam_BGetCallback`, `Steam_FreeLastCallback`, `Steam_GetAPICallResult`,
  `Steam_ReleaseThreadLocalMemory`, `Breakpad_SteamMiniDumpInit`, `Breakpad_SteamSetAppID`,
  `Breakpad_SteamSetSteamID`, `Breakpad_SteamWriteMiniDumpSetComment`,
  `Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId`; newer builds also
  `Steam_IsKnownInterface`, `Steam_NotifyMissingInterface`. All exported.
- **The interface versions a game asks for are not all in its libsteam_api.** Steamworks.NET
  names its own set from managed code (`SteamMatchMaking009`, …) and its context init fails
  when any is missing, after which the game dereferences null. So the dylib serves every
  version every SDK defines (213 interface versions, 6584 methods), never a subset.
- **Layouts:** macOS arm64 and x86_64 produce identical Steamworks struct layouts (the
  generator parses both and refuses to run if they differ), and they match Linux: the SDK
  packs callbacks with `pack(4)` on `__APPLE__` and `__linux__`, `pack(8)` on Windows.
  Examples: `LobbyChatMsg_t` 24/align 4 (mac) vs 24/8 (win), `RemoteStorageFileShareResult_t`
  272 vs 280, `SteamUGCDetails_t` 9764 vs 9776. `CallbackMsg_t` is 20 bytes on macOS, 24
  on Windows.
- **MSVC vs Itanium vtables:** overloaded virtuals are reversed on MSVC (`GetStat_2,
  GetStat`); Itanium keeps declaration order. Only `ISteamHTMLSurface` has a virtual
  destructor (MSVC one slot, Itanium two).
- **MSVC member functions return any record through a hidden pointer** passed after `this`
  (`CSteamID *f(this, CSteamID *ret, …)`), even 8-byte ones. Records of 1/2/4/8 bytes are
  passed by value in a register; others by pointer to a copy. On macOS the compiler handles
  all of it, since the proxy is a C++ subclass of the SDK class.
- **libclang targets that parse cleanly:** `x86_64-w64-windows-gnu` with mingw headers
  (`$(x86_64-w64-mingw32-gcc -print-sysroot)/x86_64-w64-mingw32/include`, else Homebrew's
  `/opt/homebrew/opt/mingw-w64/toolchain-x86_64/…`; `MINGW_INCLUDE` overrides; `-fms-extensions`) and `arm64-apple-macos11` / `x86_64-apple-macos11` with `xcrun
  --show-sdk-path`; plus `clang -print-resource-dir`/include. Parsing 91 SDKs × 3 targets
  takes about 10 s in a process pool and is cached in `parsed.json`.
- **SDK 1.53 is missing from Proton** (it has 1.52 with `SteamNetworkingSockets009` and
  1.53a with `012`), and 1.53's `011` is what Steamworks.NET 20.x games ask for.
  `gen/sdk-overlay/steamworks_sdk_153/` holds that release's four networking headers
  (GameNetworkingSockets' copies); `fetch-sdk.sh` lays them over 1.53a, minus
  `steamnetworkingfakeip.h`.

## Design

**Reverse Proton's lsteamclient.** Proton (`/Volumes/Ugreen/Projects/Proton/lsteamclient`,
BSD-3 code + Steamworks SDK headers for 90 SDK versions) lets a Windows game call a native
Linux steamclient in the same process. Ours crosses a process boundary in the other
direction, so every pointer argument becomes a sized buffer on the wire.

- **Generator** (`gen/gen_bridge.py`, Python + libclang; Proton's tables `SDK_VERSIONS`,
  `SDK_SOURCES`, `VERSION_ALIASES`, `PATH_CONV_*` carried over). Parses every SDK for both
  ABIs into plain data, keeps the newest SDK defining each interface version, classifies
  every parameter, and emits:
  - `generated/mac/<Class>_<Version>.cpp`: `struct Proxy final : public <Class>` with every
    virtual overridden (the TU includes that SDK's headers; `-fno-rtti`, since abstract SDK
    classes have no key function), each override marshalling into a `bridge::Call`.
  - `generated/win/<Class>_<Version>.cpp`: one handler per method, no SDK headers at all:
    it reads the wire, calls `slot(obj, n)` through a function pointer typed with the MSVC
    x64 signature, writes the results.
  - `generated/{mac,win}/tables.cpp`: interface factories (aliases included), method names,
    the callback table, the protocol hash; `sources.txt` for the Makefile.
  - `generated/REPORT.md`: unsupported and locally answered methods, and how every pointer
    argument was sized. Read it after any generator change.
- **Wire format** (`shared/wire.h`): little-endian frames, `u32 length, u32 method, u64
  object, payload` / `u32 length, u32 status, payload`. Structs travel in the Windows
  layout; the macOS side converts with run tables (`{win offset, mac offset, length}`) the
  generator computes by pairing both layouts' scalar leaves. Unions and bitfield structs
  are one opaque leaf.
- **Argument rules** (the generator; overrides in `PARAM_OVERRIDES`): scalars and enums by
  value; records by value with conversion; `const char *` as a string; a pointer sized by
  the count parameter right after it (`cch*`/`cub*`/`cb*`/`c<Upper>`/`*Size*`/`*Count*`…;
  bytes when the name says `cub`/`cb`/`cch`/`Size`/`Bytes`, elements otherwise), by an
  integer count *pointer* right after it (`punCount`), or by a count after a run of
  array-named pointers (`prg*`, `pArray*`, `pvec*`…); any other typed pointer or reference
  is a single element; `void *`/`char *`/`uint8 *` with no size is unsupported. The SDK's
  `STEAM_OUT_ARRAY_COUNT(<constant>)` arrays (Input and Controller handles, origins, layers)
  are `fixed:<n>` overrides; the generator refuses to run while any writable pointer it
  guessed to be one element is named like several (`*Out`, `*List`, `*Array`, `prg*`, a
  plural), so each new one gets a decision. Every non-const pointer travels both ways
  (present flag, Windows capacity, contents) so in, out and in/out buffers are one case.
  The helper allocates a fixed array at its full Windows size, and clamps every count and
  size it passes to the callee (`*punCount` included) to the buffer it allocated, so a
  sizing rule that is wrong costs a short answer, never a write past a buffer. `char **` is an out string. Interface returns resolve through the `pchVersion`
  argument (`FIXED_RETURN_VERSIONS` for the rest) into a handle the macOS side wraps in that
  version's proxy, cached per (version, handle). Function-pointer setters are answered
  locally. A record holding pointers (`SteamParamStringArray_t`,
  `SteamNetworkingMessage_t`), a game-side listener object (`ISteamMatchmaking*Response`,
  `ISteamNetworkingConnectionSignaling`) or a pointer-to-pointer array makes the method
  unsupported: it logs once and answers zero. 266 such methods across the 213 versions,
  listed in the report; none that Escape Dungeon 2 reaches.
- **Paths:** `GetAppInstallDir`, `GetUserDataFolder`, `GetItemInstallInfo`, `GetAppInstallDir`
  (AppList) rewrite their out-buffer from Windows to macOS (`Z:\…` → `/…`, other drives →
  `$SEVO_STEAM_BRIDGE_PREFIX/dosdevices/<l>:/…`); a macOS path too long for the game's
  buffer leaves it empty, and the method answers false or the size the path needs; the glyph methods convert their returned
  string; the `PATH_CONV_METHODS_WTOU` methods (screenshots, workshop files, manifests)
  convert their string arguments the other way (`<prefix>/drive_c/…` → `C:\…`, else `Z:`).
- **Callbacks:** `Steam_BGetCallback` returns the Windows `CallbackMsg_t` payload; the macOS
  side picks the table entry with that id and Windows size, converts into a per-thread
  buffer kept until `Steam_FreeLastCallback`, and rewrites the inner size of
  `SteamAPICallCompleted_t` (703). `Steam_GetAPICallResult` and
  `ISteamUtils::GetAPICallResult` send the expected callback's Windows size from the same
  table (319 layouts, one per distinct id and size pair across SDKs).
- **Strings** a method returns are copied into a per-thread ring of 256; Steam's own
  contract is "valid until the next call".
- **Transport:** loopback TCP, `TCP_NODELAY`, close-on-exec, one connection per game
  thread opened at the thread's first call (retrying for 30 s while the helper starts; the
  first connection waits for wine), dropped by `Steam_ReleaseThreadLocalMemory(true)`. The
  first wait that runs out, a refused hello, or a port file reading `failed` (the helper's
  word that it cannot serve) turns the bridge off for the process: every later call fails
  at once instead of waiting again. `libsevosteamipc.dylib` holds one idle keepalive
  connection from the game's start (hello kind 2: token only, serves nothing), since a
  game's threads come and go between calls, a game may initialize Steam late, and the
  helper quits when its last greeted connection closes. The helper binds a
  port of its own (port 0) and publishes it atomically in `steambridge-<appid>.port`; the
  dylib reads `SEVO_STEAM_BRIDGE_PORT_FILE` (the shim's way) or `SEVO_STEAM_BRIDGE_PORT`
  (by hand), so no process can take the port between a choice and the bind. The first frame is a
  hello carrying `SEVO_STEAM_BRIDGE_TOKEN` and the protocol hash (a SHA-256 of every method
  name, its wire signature and the callback table); the helper refuses anything else and
  refuses to listen at all without a token. Before the hello a connection gets 10 s and one
  frame of at most 4 KB; the token compares in constant time; only greeted connections
  count toward the helper's exit. A refused hello disables the dylib for the
  process, so `SteamAPI_Init` fails fast instead of hanging.
- **The helper** loads `steamclient64.dll` from
  `HKCU\Software\Valve\Steam\ActiveProcess\SteamClientDll64`, serves one thread per
  connection, logs to `%LOCALAPPDATA%\Sevoflurane\steambridge-<appid>.log` (every call with
  `SEVO_STEAM_BRIDGE_LOG=1`) and writes its port to `steambridge-<appid>.port` (or
  `failed`). It exits when the last greeted connection closes (2 s grace), which with the
  keepalive is when the game ends, or after `SEVO_STEAM_BRIDGE_IDLE` seconds (120) when no
  game ever said hello (a failed launch, or a hardened runtime that stripped the bridge).

## Build

```
make -C build-macos/steam-bridge sdk venv generate   # SDK headers, libclang venv, generator
make -C build-macos/steam-bridge -j12 mac win ipc native
```

`make venv` installs libclang 18.1.1 exactly. Everything lands under
`$DORMISON_BUILD/steam-bridge/` (`sdk/`, `parsed.json`,
`generated/`, `obj/`, `out/`). `fetch-sdk.sh` links a `PROTON_DIR` checkout or makes a
blob-less sparse clone of Proton's `lsteamclient` at the pinned commit. `package-engine.sh`
runs all of it and ships `steam-bridge/steamclient.dylib`, `steam-bridge/libsevosteamipc.dylib`,
`steam-bridge/REPORT.md`, `sevo-steambridge.exe` and `sevo-native.exe` (the tool's command
line, which the Windows client never runs; the app copies it into the tool directory),
with `"steam-play-macos"` in `engine-info.json` `features`, which switches the app's UI
on (Sevoflurane `cd9ff09`, `Core/SteamPlayMacOS.swift`). The dock shim links Security.framework
for the hardened-runtime check.

## Testing

- **`tests/probe.c`** drives a game's libsteam_api through the flat API the way
  Steamworks.NET does, with the bridge environment the shim gives a game. By hand, with the
  helper started like the steam stub (`../steam-stub/README.md`, plus `SteamAppId`,
  `SEVO_STEAM_BRIDGE_PORT`, `SEVO_STEAM_BRIDGE_TOKEN`):
  `probe <bundle>/Contents/PlugIns/steam_api.bundle/Contents/MacOS/libsteam_api.dylib
  [achievement]`; naming an achievement unlocks it and clears it again.
- **macbook-16** (`ssh dudurcah@100.98.56.107`, Kiri's M3 Pro, free for tests): Sevoflurane
  1.1 beta 1 (predates the feature flag, so its test tool is `sevo_macos` under
  `compatibilitytools.d`), engine `dormison-b2-steamplay` (b2 + the loader patch) and
  `dormison-b2-bridge` (the same plus the bridge files and the new shim, cloned by hand),
  `steam.exe.env` with `SEVO_STEAM_PLAY=1`. Games mapped to the tool with macOS depots
  installed: 1309000 Escape Dungeon 2 (universal, ad-hoc signed), 540610 (i386, unrunnable).
  `sevo eval '<js>'` reaches Steam's UI; `logs/compat_log.txt` and `content_log.txt` in the
  bottle's Steam directory show tool mapping and depot swaps.
- Broader coverage: native macOS games from Kiri's library with split depots
  (`app_info_print <id>` through `SteamClient.Console.ExecCommand` lists depot `oslist`;
  Fluffy Store 1038740, Cosplay Collection 2149070, Fatal Twelve 620210).

## Not built yet

- `SteamParamStringArray_t` arguments (workshop tags), `SteamNetworkingMessage_t`
  (networking sockets/messages receive and send), game-side listener objects
  (matchmaking server queries, custom signaling), `ISteamHTMLSurface` paint callbacks.
- `SetWarningMessageHook` relays nothing; the hook is accepted and ignored.
- Strings a bridged call cannot answer come back as `""`, never null, since games `strlen`
  them; a game that reads Steam before the helper is up sees empty names, not a crash.
- `installscript_osx.vdf` chmod lists; the Mach-O/script magic pass covers every bundle
  seen so far.
- The Steam overlay in a native game (Steam's own `gameoverlayrenderer.dylib` needs Steam
  for Mac; the app-hosted overlay is the route).
