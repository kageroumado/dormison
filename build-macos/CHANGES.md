# Engine releases

One section per release, written when the work lands. `publish-engine.sh`
uses the section for `r<N>` as the GitHub release body.

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
