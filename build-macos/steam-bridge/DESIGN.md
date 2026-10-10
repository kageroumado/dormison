# Steam bridge — Steamworks for native macOS games

A native macOS game installed through Sevoflurane talks to the Windows Steam client
running in the bottle. Nothing of Steam for Mac is used: no second sign-in, no second
client. This directory holds the pieces that make that work.

## The chain, end to end

1. **Steam Play in the Windows client** (done, `dlls/ntdll/loader.c`,
   `sevo_enable_steam_play`). With `SEVO_STEAM_PLAY=1` in `steam.exe`'s environment, the
   loader flips CCompatManager's enabled flag (`strcmp(platform, "linux") == 0` → true) as
   `steamclient64.dll` is mapped. Only in `steam.exe`. A game mapped to the app's tool
   `sevoflurane_macos` (`from_oslist macos`, `to_oslist windows`) then gets its macOS depots
   on install and update, and Steam swaps depots by itself when the mapping changes.
2. **Launch.** Steam shell-opens the game's `.app` (an `explorer.exe "<…>\Game.app"`
   process; the tool's `commandline` prefix is never run on the Windows client). The dock
   shim intercepts that process in its constructor and execs the game natively, same pid,
   so Steam keeps tracking it (TODO, see "Shim" below).
3. **Path lookup** (done, `mac/sevo_steam_ipc.c` → `libsevosteamipc.dylib`). Injected with
   `DYLD_INSERT_LIBRARIES`, it answers libsteam_api's `bootstrap_look_up` of
   `com.valvesoftware.steam.ipctool` from a thread inside the game: path
   `$SEVO_STEAM_BRIDGE_DIR/steam_osx`, pid = the game's own. libsteam_api then dlopens
   `$SEVO_STEAM_BRIDGE_DIR/steamclient.dylib`. Verified with Escape Dungeon 2 (1309000) on
   macbook-16: `Loaded '…/steamclient.dylib' OK`, then `CreateInterface("SteamClient017")`,
   `CreateInterface("SteamClient020")`.
4. **The bridge** (TODO, the subject of this document): `steamclient.dylib` (macOS,
   universal arm64+x86_64) forwards every interface call over a socket to
   `sevo-steambridge.exe` (x86_64 PE, mingw), which runs in the bottle, loads the bottle's
   `steamclient64.dll` and makes the same call on the same interface version.

## Facts the bridge rests on (measured, not assumed)

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
  `Steam_IsKnownInterface`, `Steam_NotifyMissingInterface`. Check each libsteam_api version
  you test with `strings`.
- **Layouts:** macOS arm64 and x86_64 produce identical Steamworks struct layouts (libclang,
  SDK 158), and they match Linux: the SDK packs callbacks with `pack(4)` on `__APPLE__` and
  `__linux__`, `pack(8)` on Windows. Examples: `LobbyChatMsg_t` 24/align 4 (mac) vs 24/8
  (win), `RemoteStorageFileShareResult_t` 272 vs 280, `SteamUGCDetails_t` 9764 vs 9776. So
  Proton's Windows↔Linux struct knowledge transfers to Windows↔macOS.
- **libclang targets that parse cleanly:** `x86_64-w64-windows-gnu` with mingw headers
  (`/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include`) and
  `arm64-apple-macos11` with `xcrun --show-sdk-path`; add `clang -print-resource-dir`
  `/include`. The MSVC target chokes on mingw's `stdio.h`. A venv with `pefile capstone
  libclang` exists at `/Volumes/Ugreen/Scratch/sevo-bridge/venv`.
- **MSVC vs Itanium vtables:** overloaded virtuals are reversed on MSVC (Proton's
  `SteamUserStats011` vtable: `GetStat_2, GetStat, SetStat_2, SetStat`); Itanium (macOS)
  keeps declaration order. Only `ISteamHTMLSurface` has a virtual destructor (MSVC one slot,
  Itanium two).
- **MSVC member functions return any record through a hidden pointer** passed after `this`
  (`CSteamID *f(this, CSteamID *ret, …)`), even 8-byte ones. On macOS a member function
  returning a record follows the C rules (`this` first; sret in x8 on arm64, rdi before
  `this` on x86_64), which a C++ subclass of the SDK class gets right automatically.

## Design

**Reverse Proton's lsteamclient.** Proton (`/Volumes/Ugreen/Projects/Proton/lsteamclient`,
sparse clone, BSD-3 code + Steamworks SDK headers for 91 SDK versions) lets a Windows game
call a native Linux steamclient in the same process. Ours crosses a process boundary in the
other direction, so every pointer argument becomes a sized buffer on the wire.

- **Generator** (`gen/`, Python + libclang, adapted from Proton's `gen_wrapper.py` and its
  tables: `SDK_VERSIONS`, `SDK_SOURCES`, `VERSION_ALIASES`, `MANUAL_METHODS`,
  `PATH_CONV_METHODS_*`). Parses every SDK for both ABIs, keeps the newest SDK defining each
  interface version, and emits per interface version:
  - `generated/mac/<Class>_<Version>.cpp`: a class deriving from the SDK's own interface
    class (TU includes that SDK's headers), each override marshalling its arguments and
    calling the transport. The compiler supplies the game-facing ABI.
  - `generated/win/<Class>_<Version>.cpp` (mingw g++): unmarshal, call the Windows object's
    vtable slot (MSVC order) through a function pointer with the MSVC signature (records via
    hidden return pointer), marshal results.
  - shared method ids, interface version → factory/dispatcher tables, callback tables,
    and a report of methods the rules cannot size.
- **Wire format = Windows layout.** All struct and callback conversion happens in the macOS
  dylib, from offset tables the generator computes for both ABIs. The PE side treats wire
  bytes as its native structs.
- **Argument rules** (generator): scalars and enums by value; records by value with
  conversion when layouts differ; `const char *` as a string; `char *`/`void *`/`uint8 *`
  out-buffers sized by the following count parameter (naming: `cch*`, `cub*`, `cb*`,
  `n*Max`, `*Size`, …) or a manual table; single `T *` as in/out values; typed arrays with
  a count; interface returns (`ISteam*` / `GetISteamGenericInterface`) resolved through the
  `pchVersion` argument into a handle the macOS side wraps in that version's proxy (cached
  per handle). Function-pointer setters (`SetWarningMessageHook`,
  `Set_SteamAPI_CCheckCallbackRegisteredInProcess`, …) are answered locally. Anything the
  rules cannot size answers zero and is listed for review.
- **Paths:** methods returning Windows paths (`GetAppInstallDir`, `GetUserDataFolder`,
  `GetItemInstallInfo`) convert to macOS paths on the way back; methods taking file paths
  (screenshots, workshop uploads) convert to Windows paths (`Z:\…` / drive mapping) on the
  way in. Proton's `PATH_CONV_METHODS_UTOW/WTOU` list the methods.
- **Callbacks:** `Steam_BGetCallback` returns the Windows `CallbackMsg_t` + payload; the
  macOS side converts by callback id (choose the layout whose Windows size matches
  `m_cubParam`), keeps the buffer until `Steam_FreeLastCallback`. `Steam_GetAPICallResult`
  and `ISteamUtils::GetAPICallResult` need the expected callback's Windows size from the
  same table.
- **Transport:** loopback TCP with a per-launch token in the environment (the in-bottle
  NW.js stub, `../steam-stub`, already serves loopback from Wine), one connection per game
  thread, `TCP_NODELAY`. A version/protocol hash handshake rejects mismatched halves.
- **The in-bottle helper** loads `steamclient64.dll` from the Steam install
  (`HKCU\Software\Valve\Steam\ActiveProcess\SteamClientDll64`), started by the shim with
  `SteamAppId` set, exits when the game's connections close.

## Shim (Dormison `../dock-shim/sevo_dock_shim.c`)

Beside the existing `SEVO_RUNNER=nwjs` hand-off (`sevo_run_natively`): when the process is
`explorer.exe` with one argument naming a directory that ends in `.app` under a Steam
library (`…\steamapps\common\<installdir>\…`), find the app id from that library's
`appmanifest_*.acf` (`installdir`), `chmod +x` the bundle's Mach-O files (the Windows client
writes 0644; `installscript_osx.vdf` lists more), spawn `sevo-steambridge.exe` like
`spawn_steam_stub` does, and exec `Contents/MacOS/<CFBundleExecutable>` with
`DYLD_INSERT_LIBRARIES=<engine>/steam-bridge/libsevosteamipc.dylib`,
`SEVO_STEAM_BRIDGE_DIR`, the transport port/token, `SteamAppId`, `SteamGameId`,
`SteamOverlayGameId`; cwd = the bundle's parent. Hardened-runtime games strip `DYLD_*`
(Escape Dungeon 2 is ad-hoc signed, fine); detect and log.

## Packaging

`../package-engine.sh` ships `steam-bridge/steamclient.dylib`,
`steam-bridge/libsevosteamipc.dylib`, `sevo-steambridge.exe` and `sevo-native.exe` (any tiny
PE; the app copies it into the tool directory), and adds `"steam-play-macos"` to
`engine-info.json` `features` — the app's UI switches on from that flag (Sevoflurane
`cd9ff09`, `Core/SteamPlayMacOS.swift`). SDK headers are fetched at build time from a
pinned Proton commit, not committed here.

## Test bed

- **macbook-16** (`ssh dudurcah@100.98.56.107`, Kiri's M3 Pro, free for tests): Sevoflurane
  1.1 beta 1, engine `dormison-b2-steamplay` (b2 + the loader patch), `steam.exe.env` with
  `SEVO_STEAM_PLAY=1`, test tool `sevo_macos`. Games mapped to it with macOS depots
  installed: 1309000 Escape Dungeon 2 (Unity, Steamworks.NET, universal, saves through
  Steam Remote Storage, logs `[S_API]` lines with `-logFile`), 540610 (i386, unrunnable).
  `sevo eval '<js>'` reaches Steam's UI; `logs/compat_log.txt` and `content_log.txt` in the
  bottle's Steam directory show tool mapping and depot swaps.
- First milestone: Escape Dungeon 2 passes `SteamAPI_Init` and its `FileWrite`/
  `FilePersisted` calls succeed through the bridge; then achievements (`SetAchievement` +
  `StoreStats`), persona name, `RunCallbacks` traffic.
- Broader coverage: pick a few native macOS games from Kiri's library with split depots
  (`app_info_print <id>` through `SteamClient.Console.ExecCommand` lists depot `oslist`;
  Fluffy Store 1038740, Cosplay Collection 2149070, Fatal Twelve 620210).
