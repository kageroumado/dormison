# Engine releases

One section per release, written when the work lands. `publish-engine.sh`
uses the section for `r<N>` as the GitHub release body.

## r16

- A D3D12 game with the upscaler on no longer ends 6–45 seconds in with "unrecognized
  selector `waitOnCommandQueue:`" on an M3 or later. D3DMetal's D3D12 present path has the
  command queue wait for and signal the drawable; a Metal 4 queue, which D3DMetal gets on
  Apple GPU family 9 hardware, re-sends both as the private QuartzCore methods
  `waitOnCommandQueue:` and `signalOnCommandQueue:` to whatever `nextDrawable` returned. The
  presenter's drawable answers both; a leased slot's previous present has already completed,
  so each returns at once. Found in the 2026-09-22 retest, on both of its D3D12 titles.
- A game whose wineserver has died ends instead of filling the log. Every waiting thread
  retried its register-wait message against the dead server's port without bound: 27 million
  `Failed to send server register wait` lines, 7.9 GB, the game at 94 % CPU. A send that
  reports the server's port gone (`MACH_SEND_INVALID_DEST`) now prints one line and ends the
  thread, the way the socket path ends it on `EPIPE`; the last thread takes the process down.
- wineserver no longer maps an object page at a stack-garbage hint. `get_shm` passed an
  uninitialized address to `mach_vm_map` with `VM_FLAGS_ANYWHERE`, which the kernel treats as
  the place to search from: usually harmless, sometimes `KERN_NO_SPACE` or
  `KERN_INVALID_ARGUMENT`, after which the server zeroed a page at that garbage and died with
  `wineserver crashed` — a playtest's `msync: error: mach_vm_map failed with 3: (os/kern) no space
  available` under Subnautica 2, twice, and every client's flood after it. The hint is zero,
  and a map that still fails ends the server with a line naming the pid, the page and the
  kernel's text instead of touching the address. The 64 MB tid map is made after the master
  socket's lock, so a candidate server that finds one alive exits without mapping it.
- The run record's `renderer=` names wined3d only when it drew. Steam's overlay loads
  `d3d9.dll` into every game to hook it, which brings Wine's wined3d and opengl32 along, so a
  D3DMetal D3D12 game read `renderer=wined3d-gl`. wined3d is the answer when the bottle names
  it or names no renderer, or when neither `d3d11.dll` nor `d3d12.dll` is loaded. Found with
  the Black Myth: Wukong benchmark.
- A game covering the screen with a borderless window keeps the menu bar reachable. The
  window sat above the status level, where the process's own menu bar (View › Upscaler, Show
  Frame Rate) could never come in front of it. It stays at the normal level, and while it is
  key the bar and the Dock auto-hide: the bar slides in when the pointer reaches the top edge.
  Displays a game captured are covered as before. Found with KAMITSUBAKI CITY REGENERATE.

## r15

- A program whose window has stopped taking messages is said so, as Windows says it: a close
  button or a Quit the program has not taken within five seconds puts a sheet on the window,
  "“<name>” is not responding", with Wait and End Game. End Game ends the process and logs
  `sevo:exit … ended by the user while not responding`. Found with HuniePop's two-and-a-half
  minute load, during which the close button did nothing.
- A fault on a thread Wine did not make (the Cocoa main thread, an audio or dispatch thread)
  ends the process with a macOS crash report, where it used to end only that thread and leave
  a window that answered nothing. Wine's handler assumes a thread it made.
- The per-process stats page carries a beat written once a second from the Cocoa main thread's
  run loop, so the app can tell a blocked main thread from a game that is merely quiet or busy.
- View › Show Picture Details stays out of windows under 320 by 240 points, so a game's message
  boxes and dialogs are readable with it on.

## r14

- A game drawn with GDI no longer hangs with the upscaler on. The view that shows a window
  surface's frames called into its presenter from `dealloc`, and AppKit keeps a removed view
  in the autorelease pool — while the pointer is over it, for one — until after the presenter
  has been released: the call landed in freed memory on the Cocoa main thread, and the
  window stayed on "Initializing.." under a spinning cursor. The view holds the layer it
  hosts and forgets the presenter when it is detached. Found with Gore Screaming Show.

## r13

- OpenGL games go through the presenter. wined3d's Direct3D 9, which is what most visual
  novels that are not plain GDI draw with, ended in an OpenGL view the upscaler never saw.
  A window's OpenGL drawable is a framebuffer object whose swap lands in an IOSurface the
  presenter reads, so Lanczos, MetalFX, Anime4K and CuNNy apply to those games as they do
  to a Metal one. 9-nine-:Episode 1 runs 1600x900 to 3200x1800 through Anime4K with no
  dropped frame. Multisampled, stereo, floating-point and 10-bit drawables stay on a view;
  `Mac Driver\OpenGLPresenter=N` puts every drawable back on one.
- A running game has a View menu: Upscaler and Final Filter switch live and are written
  back to the game's settings, Show Frame Rate (Option-Command-F) puts one number in a
  capsule at the top right of the window whatever draws the picture, and Show Picture
  Details (Option-Command-I) names the engine, the source and target size, the upscaler
  and the filter. Games started through their own Dock bundle get the standard menus too.
- A window that puts its old size back while it is being resized goes through the scaler
  (9-nine and engines like it). No texture is shown before it holds a picture, which was a
  purple second at launch. Every titled window carries a hidden toolbar, which is what
  gives an x86-64 program a title bar in full screen on macOS 27.
- `SEVO_LARGE_ADDRESS_AWARE=1` gives a 32-bit program the whole 4 GB whatever its image
  says. The app has written the variable per game since r4; nothing read it.
- The waiting paths in ntdll and msync: a waker enters the kernel only for a waiter that
  parked, a waiter cannot leave while a waker holds its stack entry, `RtlWakeAddressAll`
  wakes the waiters enrolled before it began, and an exclusive SRW acquire takes a free
  lock before it counts itself a waiter. The spins built on top (`SEVO_OBJECT_SPIN`, the
  alert spin, the retreating yield) are off unless asked for: they make synthetic hand-offs
  up to ten times quicker and have not raised the frame rate of a game.
- `SEVO_FORCE_UMA=1` leaves `CacheCoherentUMA` as the device answered it, and the driver
  can report the memory the machine has.

## r11

- wineserver is native arm64. The server runs no guest code, so it is the one process
  in the engine that need not be translated: `build-native-server.sh` builds it in its
  own arm64 tree with `DORMISON_X86_64_GUEST`, under which the server reports the x86
  machines a prefix supports and decides per process, once, whether a client runs under
  Rosetta; libinotify is universal for it and `package-engine.sh` ships both. On this
  Mac it measures at parity with the translated server (syncprof p99 1.01×, server CPU
  0.96×; Subnautica 2 at the same 21 fps; Steam's login in the same second), so its case
  is one fewer Rosetta process, not speed. `engine-info.json` carries `"server"`.
- winebus is built with SDL again: the IOHID backend alone
  never delivers an Xbox Wireless Controller over Bluetooth (045E:02E0) to a game, and the
  SDL backend does. The SDL bus polls instead of waiting, which takes an idle
  winedevice.exe from ~2.8 % CPU and ~310 wakeups/s to ~0.8 % and ~75/s on her M4 Max.
- `build-macos/configure.sh` is the README's configure block as a script, so a tree
  cannot be configured with a different flag set by accident.

## r10

- A window whose game draws through OpenGL (Direct3D 9 on wined3d, so Unity 4
  and most older titles) is opaque and black from the moment its drawable
  attaches, and its title bar is drawn like any other window's. The Mac driver
  kept a window fully transparent until its first surface draw, and an OpenGL
  swap never counted as one: HuniePop preloads its audio for 150 s before its
  first present and had no visible window for all of it, then a see-through
  title bar afterwards. The first flush now ends the transparency the way a
  surface draw does, and a drawable attaching to a shown window ends it at once
  with a black fill, which is what Windows shows before a first frame.
- Steam's `WaitingForNetwork` wait is gone. `GetAdaptersAddresses` returned every
  host interface (32 here), and Steam's login controller asks its device manager
  for at most ten, so a connected adapter that hashed past the tenth was never
  seen and the login timed out after 20 s. Adapters are now selected by the
  requested address family from the protocols attached to the interface
  (`SIOCGIFPROTOLIST`), which keeps down and unconfigured interfaces the way
  Windows does and drops the Apple-internal ones that never carry IP. The call
  is 2.7× faster (1.8 ms against 4.9 ms for Steam's flags), each adapter's DNS
  query advertises its real buffer capacity, and the synthetic first
  `NotifyAddrChange` completion r2 introduced for this wait is removed.
- Bottles on the same engine share one GStreamer plugin registry, keyed by the
  engine's module path and the effective plugin search paths, so the second
  bottle opens Media Foundation in 6 ms rather than rescanning; a plugin change
  still triggers a rescan, and an explicit `GST_REGISTRY` still wins.
- msync's registration accounting is what the tree already ran: a failed
  registration drops its interest, an abandoned mutex found on a wait-all
  attempt is put back abandoned, and a put-back wakes the waiters the attempt
  hid it from. The `msync-src` copies carry the same fixes so a rebase does
  not lose them.

## r9

- Provenance names the renderer of a 32-bit game correctly. The `sevo:gfx`
  header reads the loaded module list to see whether a title fell to wined3d,
  but the winemac unix half walked only the 64-bit loader list; a wow64 game
  loads its d3d DLLs on the 32-bit list, so wined3d went unseen and the header
  repeated the bottle's staged renderer (`renderer=d3dmetal` for a D3D9 game on
  OpenGL). It now walks the wow64 process's own 32-bit loader list, which
  shares this address space, and a 32-bit D3D9 game on wined3d reports
  `renderer=wined3d-gl` as a 64-bit one already did. The header format is
  unchanged.

## r8

- Provenance reports the renderer that actually drew, read from the modules
  the process loaded: a D3D9 game in a bottle whose overrides only cover
  d3d10core/d3d11 falls to wined3d on OpenGL, and its `sevo:gfx` line now says
  `renderer=wined3d-gl` where it used to repeat the bottle's staged renderer.
  The `sevo:gfx` header waits for the first present, where the module list can
  name the back end; DXMT, DXVK and D3DMetal keep their names, since they
  supply their own d3d DLLs and never load wined3d.
- wined3d knows the GeForce RTX 5060–5090 and Radeon RX 7600/7800 XT/7900 XT
  and RX 9070 XT device ids the app assigns a bottle from the Mac's chip, so a
  `VideoPciDeviceID` override resolves to the card instead of logging
  `Invalid GPU override 10de:2c05` and falling back to a default.
- nsiproxy reports an interface whose media is inactive as down rather than
  connected, so an unplugged port or Apple-internal adapter no longer sorts
  ahead of the one carrying traffic. iphlpapi gives an adapter with routes the
  interface metric Windows assigns for its link speed when the platform
  reports none, and lists each gateway once.

## r7

Media: the engine ships `winegstreamer` and the GStreamer it needs, so Media
Foundation has a source and a decoder. Without them Wine's source resolver
falls through every registered byte-stream handler to the GStreamer one, fails
to create it, and returns `MF_E_UNSUPPORTED_BYTESTREAM_TYPE` — which is what a
Unity game playing an intro video reports as a black screen. The engine
declares `media` in `engine-info.json`'s `features`.

- The plugin set is the one a game's video goes through: `isomp4` and
  `matroska` for the container, `videoparsersbad` and `audioparsers` for the
  elementary streams, `applemedia` for VideoToolbox, `libav` for the audio
  codecs, and `vpx`, `opus`, `vorbis`, `flac`, `mpg123`, `theora`, `asf`, `avi`
  and `wavparse` beside them. H.264 decodes on the GPU: `decodebin` picks
  `vtdec_hw` on its own.
- `libav` carries the AAC decoder. Upstream's `applemedia` exposes
  VideoToolbox for video but no AudioToolbox decoder, and nothing else in the
  tree decodes AAC, so an MP4 whose audio pad cannot link takes the whole
  pipeline down with it.
- Everything shipped is LGPL. The FFmpeg inside `libav` reports `LGPL version
  2.1 or later` from `avutil_license`, `avcodec_license` and `avformat_license`,
  built with `nonfree` and `version3` disabled and no GPL option; cerbero's
  `recipes/ffmpeg.recipe` declares `License.LGPLv2_1Plus` to match. The GPL
  plugins live in packages this build never merges — `a52dec` and `dtsdec` in
  codecs-gpl, `x264` and `x265` in codecs-gpl-restricted, `dvdread` and
  `resindvd` in dvd-gpl — and no packaged library links `liba52`, `libdca`,
  `libx264`, `libx265` or `libdvdread`.
- The libraries land flat in `wine/lib` and the plugins one directory below.
  Upstream's install names are already `@rpath`-relative and its plugins carry
  an `@loader_path/..` rpath, so the tree is relocatable as it arrives.
  `libz`, `libbz2`, `libintl` and `libMoltenVK` stay the engine's own: they are
  shared with wine itself, and a second file under the same install name would
  be the one some modules resolved to.
- `winegstreamer` finds its plugins from its own path rather than from the
  environment, so a bottle needs no new variables and a GStreamer the host has
  installed is not searched. A plugin built against another installation would
  bring that installation's `libgstreamer` into the process beside ours.
- The registry that caches which plugin supplies which element is written
  under the prefix, so switching engines rescans rather than reading a cache
  naming paths the new engine does not have.

- Provenance: `sevo:run` carries the Steam app id. Steam puts
  `SteamAppId` and `SteamGameId` in the child's **Windows** environment, which
  lives in the PEB's process parameters; the unix environ a wine process
  inherits never sees them, so every line read `appid=none`. The PEB block is
  read first and the unix environment answers for the app's own `SEVO_*`
  launches.
- An unrecoverable stack overflow is followed by the addresses the spent stack
  repeats and how often. A thread that recursed writes the same return address
  once per frame, so the counts name the loop, and `WINEDEBUG=+loaddll` in the
  same run names the module it sits in.

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
