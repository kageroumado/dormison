# Engine releases

One section per release, written when the work lands. `publish-engine.sh`
uses the section for `r<N>` as the GitHub release body.

## r20

- A WaitForMultipleObjects WaitAll on msync returns once the pump has consumed its set: the
  pump takes the registration into a committing state the thread cannot cancel out of,
  consumes every member under the freeze, then publishes the grant, and the thread waits
  through the committing state. The pump used to publish the grant first and consume after,
  so a thread could return from a completed WaitAll and read a semaphore it had just taken at
  its old count (`waitall-query.exe`: 4 such reads in 1.1 million rounds on r19b, 0 on r20).
  NtQuerySemaphore, NtQueryEvent and NtQueryMutant on msync objects wait for the pump's freeze
  to end, so a query never reads a WaitAll set half consumed, and a dying thread's mutexes are
  abandoned only after any grant the pump is committing to it has landed.
- A WASAPI stream's volume is applied to its own samples, each channel at its own gain: a
  render stream's as CoreAudio pulls them, a capture stream's as its frames are converted, so
  a muted capture session records silence and a stream at left 0, right 1 plays the left
  channel silent. The stream used to set its AudioUnit's one gain to the loudest channel's
  volume, so every channel played at that one, and a capture stream's volume and mute
  changed nothing. `SEVO_COREAUDIO_DEVICE_BUFFER=1` still writes the device's controls instead.
- The Metal presenter encodes one frame at a time whichever route brings it: the renderer's
  thread on the hooked or the blocking direct route and the ready signal's listener take one
  lock from the encoding through the present of the drawable. A switch from display sync off
  to on while the listener was still presenting an earlier frame used to let the renderer's
  thread wait one second for it and then encode beside it, on the same scaler, intermediates
  and render pass. A deferred frame whose ready value has not come within that second is
  dropped, at the transition and at the window's detach, rather than holding the frame behind
  it or the presenter.
- The Steam stub reads the machine field of the Steamworks dll's PE header and decides its
  bitness once: a dll of the sibling's bitness goes to the sibling stub with a hand-off marker
  in its environment, so the sibling never hands back, and a dll of neither machine ends the
  stub with one log line naming the file and its machine. The stub used to load first and
  hand off on `ERROR_BAD_EXE_FORMAT`, and a dll both bitnesses rejected with that error had
  the two stubs re-execute each other without end. Each stub also looks for the dll of its
  own bitness first within a directory, so a handed-off invocation finds the one it can load.
- `setStat` reads the stat's type from Steamworks: the typed getters answer only for a stat
  of their type, so the one that answers names the setter, and the answer is kept per stat
  name. Only when neither getter answers yet is the type guessed from the value, integral to
  `SetStatInt32` and fractional to `SetStatFloat`, and the log says so. The value is
  range-checked as the typed value before the cast; a non-finite value is refused. The stub
  used to send every integral value to `SetStatInt32`, which Steamworks rejects for a stat
  configured as FLOAT, and cast to `int` before any check.
- The Discord bridge's client teardown is bounded and cannot miss its own cancellation.
  Whichever side sees its end close sets a `closing` flag the pipe reader checks before every
  `ReadFile`, and the cancellation is repeated every 50 ms until the reader says it has left
  its loop, for at most five seconds; the wait for the relay thread and the flush that lets
  the game drain the pipe end after five seconds too, the flush by a watchdog that cancels it.
  One `CancelSynchronousIo` used to be issued once, which found nothing to cancel when the
  socket's EOF arrived before the reader entered `ReadFile`, and the final `FlushFileBuffers`
  waited as long as a client that never read. The 30 s grace before the bridge becomes a
  system process and the backslash `\\?\unix` socket addressing are as they were.
- Packaging. `package-engine.sh` checks content, never timestamps: the Swift archive carries a
  build id (the sha256 of its sources), the driver prints it in `sevo:run` (`swift=…`), and
  the script refuses a staged `winemac.so` that carries another id or an archive whose id is
  not the one the sources give now; `wineserver` and `ntdll.so` carry
  `sevo:server-protocol=<N>` and must agree with each other and with
  `include/wine/server_protocol.h`; the tree must be committed unless `--allow-dirty` is
  passed. `tools/makedep` makes a static archive named by path in `UNIX_LIBS` a prerequisite
  of the unix lib, so `winemac.so` relinks after the archive changed. `engine-manifest.json`
  beside `engine-info.json` lists every file with its sha256 and origin (build, native-server,
  source, deps, gstreamer, donor, packaging), the source HEAD and dirtiness, the donor's
  identity, the server's protocol and the Swift build id. The archive used to be a link
  argument with no prerequisite, the script compared the archive's and the driver's
  modification times, which wine's installer resets, and took any native server it found;
  `build-macos/tests/packaging-gates.sh` shows a stale driver younger than the archive
  refused.
- A game's own loader copy opened by LaunchServices with no program, from its Dock tile or
  from Finder, opens `sevoflurane://play/<id>` so the game starts through Sevoflurane, where
  it printed wine's usage and quit.
- A process started with `SEVO_QUIET=1` in its environment is infrastructure to the dock
  shim whatever its exe name: its windows never order in and it never becomes a Dock app.
  Sevoflurane sets it on the frame-rate unlocker it starts beside Genshin Impact, whose
  window took the foreground from the game, which then minimized itself into the Dock at
  its next display-mode change, on entering the world.
- A critical section is released free, and the releasing thread, or any running thread, may
  take it again before a woken waiter arrives; a woken waiter that loses re-queues. Wine
  handed the lock to the waiter and queued every thread arriving meanwhile behind it: on
  `order.exe` a contended section went back to the releasing thread 78–82 % of the time at
  2–4 threads and 62 % at 16, against Windows' 98 % and 87 %; now 92–94 %. `LockCount`
  carries the documented post-2003-SP1 encoding (bit 0 clear while held, bit 1 clear while
  one wake is outstanding, the rest the complement of the queued waiter count), so a program
  reading it reads what Windows shows. Per-thread shares at 16 threads are less even than
  Windows' (a 16× spread over 400 000 acquisitions, no thread starved). On syncprof's
  `critical-section` shape at 16 and 64 threads the round takes a third less wall time and half the
  involuntary context switches, with p99 1.2–1.8× higher; `barrier-chain` and `job-fork-join`, which
  wait on SRW locks and events, are unchanged.

## r19

- A fullscreen game in exclusive fullscreen keeps the menu bar and the Dock as r17 left them
  while it holds the cursor for mouse-look, where r18 switched them to hidden under the
  captured display and left a black strip across the top with every click that far off
  (Subnautica 2's Fullscreen on a MacBook Pro). The borderless window that covers the screen
  keeps r18's fix: no menu bar or Dock on edge pressure during mouse-look.
- A presented Metal window logs one `sevo:presenter presents:` line when its first frame
  arrives and again whenever it changes: whether frames come through the hooked command
  buffer or the drawable's own present after a Metal 4 queue's signal, which present call the
  game makes (`present`, `present(at:)`, `present(afterMinimumDuration:)`), and the game
  layer's display sync. Whether a present may wait on the display follows from these, and
  the log could not say which applied.
- A game on the Metal 4 route (D3DMetal on M3 and later) with display sync off no longer
  waits in its present call. The present records the frame and returns; a listener on the
  game queue's ready signal takes the on-screen drawable, draws the final pass and presents,
  every frame in order. The present used to take the on-screen drawable on the game's queue
  worker and hold it about one GPU frame (p99 13–19 ms on an M3 Pro), which added 1↔2
  refresh alternation to the game's frame times. With display sync on, the present still
  waits for the on-screen drawable, which is what paces the game to the display.

## r18

Fixes from the 2026-09-26 sync, runtime and graphics reviews.

- A write into another process's memory that spans pages with different protections leaves
  each page's protection as it was. The Rosetta code-invalidation step restored the first
  page's allocation protection over the whole range, so a data page could lose write access
  or gain execute. It now walks the range region by region and also invalidates memory that
  was allocated RW and later made executable.
- `ReleaseMutex` / `NtReleaseMutant` on an msync mutex reports NT's previous count (-1 at
  depth two, 0 at depth one), where it reported the recursion depth.
- WaitForMultipleObjects with WaitAll on msync is granted by wineserver: the set is
  registered with the server's pump, which freezes every member (bit 31 of the object's
  high word, which every fast path's compare-and-swap now respects), grants the set only
  when all of it is available at that instant, consumes it itself and wakes the one thread
  it went to. The client no longer takes members one at a time and puts them back, so a
  WaitAll can no longer succeed on a set that was never all signaled (`waitall-phantom`
  measured 4–13 such grants per 20 000 waits on every earlier release), a semaphore or event
  is never consumed by a wait that then fails, and a contended WaitAll costs one wake per
  grant where each signal used to wake every waiter parked on the object. A poll whose set
  visibly lacks a member is refused without the round trip. A thread's registration ends
  with the thread: wineserver cancels it before it abandons the thread's mutexes.
- A terminated thread's handle is signaled once its Mach thread is gone. wineserver passed
  the Mach thread port name to `kill()` as a PID, so it could report a live thread dead.
- An msync object wakes its waiters again when the wake call is interrupted, and an object
  with 65,535 client references refuses another rather than wrapping its shared count.
- The TLS slot Mono reads at `%gs:0x1780` is Darwin pthread key 752; ntdll now reserves that
  key at startup, so a native library can no longer receive it and overwrite the pointer.
- D3DMetal's module tokens follow the module's reference count, so a toolkit
  `GetProcAddress` keeps working after one of several `FreeLibrary` calls.
- Audio capture holds only the newest ring's worth of frames when CoreAudio delivers more
  than the capture ring holds, where it wrote past the end of the ring.
- An OpenGL frame the presenter is refreshing can no longer be handed back to the game for
  drawing, and a window that switches from a legacy to a core-profile context gets new
  framebuffers in the new context's namespace, where it bound names from the old one.
- The native FPS overlay counts the presenter's frames once the presenter is the source, as
  the app and the frame graph do.
- Activating the window that is already active reports success; the staging import returned
  an uninitialized value there.
- An allocation retried after native views are released searches the normal address range;
  the staging allocator retried over an empty range when no upper limit was given.
- A fullscreen game that holds the cursor for mouse-look (clipped and hidden) hides the menu
  bar and the Dock outright, and they return to auto-hide when the cursor shows or the clip
  ends. Auto-hide revealed them whenever the pinned pointer pushed against a screen edge,
  one point inside it or not.
- A process that has presented prints `sevo:exit pid=<pid> wpid=<wine pid> windows closed`
  when its last on-screen window goes away, hidden or destroyed (a destroyed main window
  reaches the driver only as the hide; hidden helper windows live until exit), and
  `… windows reopened` when one shows again, so the app can tell a crash during the
  program's own teardown from one while it was playing.
- wined3d says in the default log whether a presented OpenGL window's frame size reached the
  presenter, and if not, why: exclusive fullscreen, a destination that is not the whole
  client area, or a back buffer as large as the window. One `sevo:presenter … wined3d` line
  each time the answer or the sizes change.
- The presenter's readout names the engine that is running, from the build name
  `wine --version` prints, and falls back to `SEVO_ENGINE_NAME` only when ntdll gives none.
  The app writes that variable from its own idea of the active engine, which lagged behind
  an engine installed while it ran.

## r17

- `wine --version` names the release, `dormison-r17 (Staging)`, where it printed the distance
  from the last tag (`r15-32-ge4e4545136c` on r16). The build reads the name from
  `DORMISON_VERSION`, then from the top `## r<N>` heading of this file, and only then from
  `git describe`. `publish-engine.sh` refuses an engine whose `wine --version` names another
  release.
- Music and every other program playing through the same output keep playing while a game
  does. winecoreaudio shrank the device's IO buffer to each stream's period, and the HAL runs
  the device at the smallest size any process asks for, so every client rendered more often
  with less slack; it now only ever grows the size. A stream's volume is a gain on its own
  AudioUnit, where it was written to the device's volume, which is the Mac's output volume.
  `SEVO_COREAUDIO_DEVICE_BUFFER=1` restores both device-wide writes, for an A/B; the dropout
  itself is not yet measured against it.
- A window the program moves or resizes while it answers a frame change from macOS goes
  where the program put it. The driver skipped every placement made during that answer, so
  a Unity game switching to "Fullscreen Window" stayed at the 1728×1084 frame macOS had
  clamped it to under the menu bar while Wine placed the mouse by the program's 1728×1117
  rectangle at the top of the screen: clicks landed up to 33 pt away from the buttons
  (Megabonk). The sequence is read from the 2026-09-22 capture, not reproduced;
  `WINEDEBUG=+macdrv` names each placement the driver now passes on.
- A movie in an MPEG-1 or MPEG-2 program stream (`.mpg`, `.vob`) plays, with its sound.
  The engine's GStreamer had the parsers and decoders for both streams but no demuxer for
  the container, so DirectShow's MPEG-1 splitter and Media Foundation's source both failed
  on the file: a visual novel's opening movie, which is often such a file played through
  DirectShow, stayed silent or was skipped. `mpegpsdemux` is now bundled (about 100 KB,
  nothing new behind it). Found with Wonderful Everyday (BGI) in the 2026-09-22 retest.
- The upscaler gets a Direct3D 9 game's own frame when the game runs on wined3d (OpenGL) in
  a window larger than its picture. wined3d stretched the back buffer over the whole client
  area with a linear filter before the presenter saw it, so a visual novel drawing 800×600
  into a screen-sized window handed the upscaler a 1728×1117 picture that was already
  scaled, and View › Upscaler changed nothing visible. A window the presenter shows is now
  marked (`__wine_sevo_gl_presenter`); wined3d puts the frame into its drawable at the back
  buffer's size and names that size (`__wine_sevo_gl_source`), and the driver hands the
  presenter only that part. The `sevo:presenter gl source` line gives the game's own size.
  Found with Wonderful Everyday (BGI, 32-bit) in the 2026-09-22 retest.
- A fullscreen game holds the cursor during mouse-look: while the cursor is hidden, a clip
  to the whole screen the game's window covers keeps it one point inside the edges, where
  the menu bar and the Dock revealed as the camera turned. Upstream treats a clip covering
  every screen as no clip, which on Windows changes nothing and on a Mac lets the pointer
  reach the edges macOS reveals things at. Found with Subnautica 2 and the Wukong benchmark in
  the 2026-09-25 retests.
- Unity games built on Mono from 2018.4 to 2020.3 start. Mono's JIT reads a TLS value
  inline through `%gs`, which on macOS is the pthread TSD, where the TEB's TLS slots read as
  zero: TABS and Aka Manto faulted on `[null+0x10]` about 170 times and ended in 3–4 s.
  TlsAlloc now hands out indexes in the expansion array, whose pointer each thread mirrors
  at `%gs:0x1780`, as CrossOver does.

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
- A process whose wineserver has died ends within a second, also when every thread is parked.
  A wait on one object sleeps on the object's shared-memory word and never sends the server
  anything, so the dead-port check above never ran: `services.exe`, `winedevice.exe`,
  `explorer.exe /desktop` and idle games stayed alive with no server, one of them at 5 % CPU,
  and a harness that killed wineservers left 1 500 of them on this Mac. Each process now holds a
  dead-name notification on the msync server's port in one parked thread and ends when it
  fires.
- The frame-rate counter can show how the frames were paced, not only how many there were.
  View › Show Frame Time Graph (⌥⌘G, `Mac Driver\FrameRateGraph=Y`, `SEVO_FPS_GRAPH=1`)
  turns the capsule into a card: the rate, the 1 % low and the slowest frame of the last ten
  seconds, and every frame of the last five seconds drawn at the time it took. Its source is
  a ring of the last 994 frame timestamps in the stats page, after the fields it had
  (`sevo_stats.h`), which the app also reads to keep a trace of every frame of a run
  (`sevo perf`). An app that predates the ring reads the page as before.
- A healthy game is no longer offered to be ended as "not responding" when it destroyed its
  own window rather than answering a close: a request that left the queue unanswered counts
  as settled, and the sheet leaves when its window does.
- Two presenter races that could end a game are closed: an OpenGL game's first frame could
  arrive before its view was attached and stop the process, and a D3DMetal game recreating
  its swapchain could ask a presenter the main thread had just freed for a drawable. The
  upscaler chosen from the View menu mid-game is read as one snapshot.
- wineserver reuses every freed sync object from a stack rather than scanning live objects
  from the last one freed, which made each creation slower the more objects a game held.
  Both sides' object page tables are allocated once, so no thread reads one while another
  moves it, and a page that cannot be mapped ends the process with a line saying so.
- A program's own env file (`.sevo/apps/<exe>.env`, `debug.env`) can no longer set
  `WINEMSYNC`, `WINEESYNC`, `WINEPREFIX`, `WINESERVER` or `WINEARCH`: a process that
  disagrees with the running wineserver about its sync mode exits at start. `bottle.env`
  still can. `KEY = value` names `KEY`.
- A shader package whose pass size works out infinite, not a number or past 16384 pixels
  takes the hooked texture's size instead of ending the game.
- A program in the background no longer keeps the Mac's display awake. DirectInput declared
  user activity to macOS for every controller event of any process that read one, at most
  once a second: a drifting stick under the Steam client, whose windows are hidden, held the
  display on with nothing on screen. It now declares activity only for the process that owns
  the foreground window. A program that turned the screen saver off holds the display only
  while it is the active app. `sevo holds` names what holds the display on a Mac.
- On M3 and later, a D3D12 game's frame goes on screen only once the game has finished
  drawing it. D3DMetal's Metal 4 queue tells the drawable when its work is done
  (`signalOnCommandQueue:`), and the presenter answered with nothing, so its final pass
  could sample a half-drawn frame. The renderer's queue now signals a shared event and the
  present waits for it. Checked on an M3 Pro with D3DMetal 4.0 beta 2: without r16's
  selector answers the samples end with `waitOnCommandQueue:` unrecognized within seconds;
  with them every present waits for the signal, at 120 fps.
- wineserver no longer maps an object page at a stack-garbage hint. `get_shm` passed an
  uninitialized address to `mach_vm_map` with `VM_FLAGS_ANYWHERE`, which the kernel treats as
  the place to search from: usually harmless, sometimes `KERN_NO_SPACE` or
  `KERN_INVALID_ARGUMENT`, after which the server zeroed a page at that garbage and died with
  `wineserver crashed` — a playtest's `msync: error: mach_vm_map failed with 3: (os/kern) no space
  available` under Subnautica 2, twice, and every client's flood after it. The hint is zero,
  and a map that still fails ends the server with a line naming the pid, the page and the
  kernel's text instead of touching the address.
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
- The Japanese font families a Windows program asks for resolve to Japanese faces. The engine
  ships IPAGothic and IPAPGothic (IPA Font License v1.0) in `share/wine/fonts`, and wine.inf
  maps MS Gothic, MS PGothic, MS UI Gothic, Meiryo, Meiryo UI, Yu Gothic and Yu Gothic UI
  onto them — MS Gothic fixed pitch, the rest proportional — and MS Mincho, MS PMincho and
  Yu Mincho onto the Mac's Hiragino Mincho ProN, under their English and Japanese names
  (`ＭＳ ゴシック`, `メイリオ`, `游明朝`, …). A bottle without them laid Arial out at the
  fixed-pitch advances a Gothic face would have, and the text overlapped. Found with
  Wonderful Everyday (BGI.exe).
- A program's tray icon can stay out of the Mac menu bar. With `Mac Driver\StatusItems=N`
  the driver declines every tray call, and explorer keeps the icon in its own tray window,
  which `Explorer\ShowSystray=N` keeps hidden. Sevoflurane sets it in its bottle, so Steam's
  icon never appears: it showed from the client's start until the app ended explorer once
  Steam was up.
- On M3 and later, a D3D12 game with the upscaler on no longer flashes stale frames. D3DMetal's
  Metal 4 queue presents the drawable itself, and the presenter put the real drawable on screen
  with its own `present` before the command buffer holding the final pass was committed; a
  drawable presented that way waits only for work already scheduled, so the screen showed what
  it last held. It is presented through that buffer now. Found in the 2026-09-25 retest.

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
  The DLL hashes identify the renderer, since every renderer spoofs the
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
