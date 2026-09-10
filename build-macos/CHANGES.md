# Engine releases

One section per release, written when the work lands. `publish-engine.sh`
uses the section for `r<N>` as the GitHub release body.

## r6

Discord: `sevo-discord-bridge.exe` sits in the engine directory and serves
`\\.\pipe\discord-ipc-0` inside the bottle, relaying every byte to the Discord
client's unix socket on macOS and back. A game that ships discord-rpc, the Game
SDK or the Social SDK reaches Discord through it unchanged, with its own
artwork, state and buttons, because the bridge parses nothing. The engine
declares `discord-bridge` in `engine-info.json`'s `features`, which is what the
app gates its switch on.

- One bridge serves every program in the prefix: four free pipe instances at a
  time, a thread per connected client, and no limit on how many are connected.
  A game that finds them all taken gets `ERROR_PIPE_BUSY`, which is what
  discord-rpc's `WaitNamedPipe` retry is for. A named mutex keeps a second copy
  from starting.
- The socket is reached with WinSock `AF_UNIX`, addressed as
  `\\?\unix\var\folders\…\T\discord-ipc-N` with **backslash** separators.
  `connect()` resolves `sun_path` through `wine_get_unix_file_name()`, a socket
  node will not open, and the fallback that opens the parent directory instead
  splits the name on a backslash — with forward slashes the call returns
  `ERROR_CANT_ACCESS_FILE`. Indices 0 through 9 are tried by connecting, since
  Discord leaves dead socket files behind and takes the next free index when it
  restarts. `--dir` carries the directory, because a PE process sees Wine's
  environment rather than the host's `TMPDIR`.
- When no socket answers, the pipe client is disconnected at once, so a game
  reads Discord as absent instead of hanging.
- A relay that ends leaves the pipe connected, unblocks its own reader with
  `CancelSynchronousIo` and flushes before closing. `FSCTL_PIPE_DISCONNECT`
  drops unread data, and Discord's last frame before it hangs up is the CLOSE
  that says why.
- Half a minute in, the bridge calls
  `NtSetInformationProcess(ProcessWineMakeProcessSystem)`, so it never holds a
  prefix open on its own. The window matters: made a system process at startup,
  it is the only thing in a fresh prefix and wineserver takes the bottle down
  around it.
- One line on stderr per connect and disconnect, tagged `sevo:discord`, so the
  wine log shows which socket index answered.

## r5

Provenance: after this release the wine log names, for every process it
starts, which loader ran it, which engine and renderer answered, and whether
a frame ever reached the screen. The lines go straight to stderr, so
`WINEDEBUG=-all` does not silence them, and there are four per process.

- `sevo:loader pid=<pid> exe=<name> loader=alternate|bundle=<path>|engine`
  — one line per exec of a wine loader, from `loader_exec`. `engine` is the
  engine's own `wine`; `bundle=` is the copy inside the game's launcher
  bundle, which is what gives the process its name, icon and Game Mode
  eligibility, so a Dock tile reading "wine" and a `loader=engine` line for
  the same program are the same fact seen from two sides. `alternate` is the
  other bitness. The alternate loader is now `access()`ed before the exec, so
  a tree without it falls through by decision rather than by a failed
  `execv`.
- `sevo:run pid=<pid> exe=<name> appid=<id> engine=<name>` — at winemac.drv
  init. `appid` is Steam's `SteamAppId`/`SteamGameId`, or `none` outside a
  game; `engine` is the directory the running `winemac.so` was loaded from.
- `sevo:gfx pid=<pid> renderer=… toolkit=… presenter=on|off upscaler=…
  msync=…` and `sevo:gfx pid=<pid> d3d11=<sha8> d3d12=<sha8> dxgi=<sha8>` —
  which renderer answered, and the identity of the DLLs it answered with.
  Both come from `<engine>/renderer-hashes`, written by the app when it
  stages a renderer (format in `build-macos/README.md` § Renderer
  provenance); each field is `unknown` when the file or the key is missing.
  The DLL hashes are the only honest answer, since every renderer spoofs the
  same DXGI adapter string.
- `sevo:gfx pid=<pid> first present +<N>ms surface=<ptr> layer=on-screen|
  off-screen` — the first frame that reached the compositor, from the client
  surface funcs table, so Vulkan, OpenGL and D3DMetal all count. Its absence
  separates "the game never drew" from "the game drew and nothing
  composited". `off-screen` means the presenter owns the layer.
- `sevo:gfx pid=<pid> exit presents=<N>` — at process exit, through
  `atexit`, so a process that is killed prints none.
- `SEVO_GFX_LOG=1` adds `sevo:gfx pid=<pid> d3dmetal posted=<P>
  executed=<E>` on the first presented frame and every 10 s after: the
  `CLIENT_SURFACE_PRESENTED` notices D3DMetal's `nextDrawable` hook posted
  against the ones the driver ran. They differ by the notices coalescing
  drops; `posted` climbing while `executed` stands still is the present hook
  firing into a window thread that is not running it.

Other engine changes:

- `<prefix>/.sevo/debug.env` is read between `bottle.env` and the
  per-program file, with the same syntax. It is where the app's debug mode
  puts the Wine channels and the renderer logging it turns on, so the
  bottle's own settings survive underneath it and are back the moment the
  file is deleted.
- `SEVO_ENV_FILES=0` now also disables the `SEVO_LOADER` exec, so a harness
  opting out of the env files is not silently rerouted through a game
  bundle.
- The dock shim watches `SEVO_OWNER_PID` — when the process that owns the
  bottle exits, a `kqueue` `EVFILT_PROC` wakes the shim, it logs
  `sevo:shim owner <pid> exited — bringing the prefix down` and runs
  `wineserver -k`. Unset, nothing is watched; an owner already gone when the
  watch opens counts as the event.
- `build-macos/patches/dxmt-log-airconv-failure.patch` makes a failed
  DXBC-to-AIR translation name its stage and its shader hash at ERR level.
  DXMT is a binary payload, so the patch is carried rather than applied; the
  payload in this release is the stock v0.80. Rebuild instructions are in
  `build-macos/README.md`.

## r4

- Presenter: a GDI flush ahead of the view's first layout is copied into a
  frame the size of the DIB, and the layout cuts it to the view, so a
  program that draws once and never flushes again still has its picture on
  screen; a flush with no frame texture to copy into reports failure and
  win32u keeps the dirty bounds. A resize whose source textures cannot be
  allocated leaves the ring as it was, so the drawables out on it still come
  back and the old size serves again. Detaching a surface stops its display
  link behind any start already on its way, so no start lands after the
  final stop. A presenter released with a drawable dropped unpresented no
  longer trips libdispatch's semaphore check and takes the process with it.
- D3DMetal: one presentation notice waits per client surface; a renderer
  outrunning its window's thread no longer grows the event queue.
- Input: a cursor warp or clip in a presentation-scaled game maps through
  the visible window under the point, the key window first, the way
  `GetCursorPos` already did; a hidden or minimized window that once covered
  the point no longer answers.
- winemac: a failed client surface allocation returns NULL instead of
  dereferencing it.

## r3

- Every release carries `dormison-r<N>.tar.xz.sig`, an Ed25519 signature the
  app verifies against its pinned key before it opens the tarball; the
  manifest is signed the same way.
- msync: a wait-all on an abandoned mutex returns `WAIT_ABANDONED` instead of
  spinning until its timeout; a failed wait-all puts back only what it took,
  keeps a mutex the caller already owned, restores abandoned state, and wakes
  the waiters the put-back objects have. Multiwait registration drops its
  interest on every exit, the alert object included, and the server drops
  the node it kept when a later object was already available.
- Presenter: a GDI flush copies the dirty rectangle out of the DIB while the
  program's surface lock is held, so the picture on screen is the one the
  program finished; a flush the presenter cannot take reports failure and
  win32u keeps the dirty bounds. Source textures for Metal views are leased
  until both the renderer and the GPU are done with them. The final Lanczos
  pass is separable with precomputed weights and is skipped at 1:1.
- Input: raw mouse-look counts are delivered unscaled whatever the window's
  presentation scale; a cursor warp rewrites queued clicks in each window's
  own coordinates. `GetCursorPos` finds the presentation-scaled window under
  the cursor in the process's own window list instead of asking the window
  server: 29 µs a call instead of 336 µs, for the games and the Steam overlay
  thread that poll it every frame. The synchronous main-thread handoff uses
  an acquire/release flag.
