# Building the engine on macOS

This repository is Wine. The `main` branch is upstream wine-staging (tag
`wine-staging-base`, wine 11.16) plus the commits that make it the engine
Sevoflurane ships. `git diff wine-staging-base` is the whole change.

The build happens **out of tree**, in `$DORMISON_BUILD` (default
`$HOME/dormison-build/`), so the repository stays code only:

```
$DORMISON_BUILD/
  wine-src -> <this repository>            (symlink)
  deps/         dependency headers and dylibs (Intel Homebrew bottles, extracted)
  deps-raw/     the bottles as downloaded
  gstreamer/    GStreamer, universal, from upstream's macOS packages
  wine-staging/ the staging patch tree the base commit was made from
  build/        configure output and objects; incremental
  stage/        `make install-lib` output that package-engine.sh assembles from
  releases/     engine tarballs
```

## Toolchain

x86_64 mingw (`x86_64-w64-mingw32-gcc`, `i686-w64-mingw32-gcc`), Xcode's
clang for the unix side, bison 3 ahead of the system one on `PATH`
(`brew install mingw-w64 bison`), and the Intel dependency bottles in
`deps/`.

The dependencies need no x86_64 Homebrew. libinotify (`winebus.so`,
`wineserver`) and GStreamer (`winegstreamer.so`) are linked at build time;
everything else — freetype, gnutls, MoltenVK,
the Vulkan loader, libpng, jpeg-turbo, mpg123, gettext and gnutls's own
dependencies — is `dlopen`ed by soname at runtime, so configure needs only
their headers plus an x86_64 dylib to read the soname from. Fetch the
`sonoma` bottles of those formulas from Homebrew's registry (`tahoe` has no
Intel bottles), extract them, and merge `include/`, `lib/` and `bin/` into
`deps/`, rewriting each `.pc` file's prefix to that directory.

SDL2 is the exception: Homebrew no longer ships `sdl2` (`sdl2-compat` is
SDL3 underneath). `winebus.so` `dlopen`s it like the others, and the donor
engine already carries an x86_64 `wine/lib/libSDL2-2.0.0.dylib`, so take
both halves from there. Put the `include/` of the matching SDL release
(`strings libSDL2-2.0.0.dylib | grep SDL-release` names it; 2.32.8 today)
in `deps/include/SDL2`, copy the dylib into `deps/lib`, set its id with
`install_name_tool -id @rpath/libSDL2-2.0.0.dylib`, and link
`libSDL2.dylib` to it. The id matters: the donor's reads `libSDL2.dylib`,
and configure looks for a `libSDL2-2.0*` install name. Keep SDL in: the
IOHID backend alone does not get an Xbox Wireless Controller over
Bluetooth (`045E:02E0`) to a game, and the SDL backend does.

## A build that corrupts Genshin's asset reads

The engines published as b1 and b2 corrupt data the game reads during a scene
load: Genshin logs `HK4EUpload: error asr_003: BlobSignature not match
[...blocks/00/<blk>@<offset>]` by the dozen, or Unity's `The file 'CAB-…' is
corrupted!` / `Mismatched serialization`, then hangs on the loading screen or
dies with `c0000005`. The files on disk are intact, Wine's file I/O reads them
correctly (14,700 overlapped, unbuffered and mapped reads per block, compared
byte for byte), and nothing in the launch environment matters: it happens
with DXMT and with D3DMetal, with and without the presenter, the GPU identity,
AVX, Sevoflurane's launcher bundle, or the fps unlocker. b1 survived five
hours through a plain `wine steam.exe GenshinImpact.exe` script; b2 fails on
the first or second load through either path.

The same b2 commit built on an M4 Max with Xcode 27.0 (27A5194q, Apple clang
21.0.0, `ld-1328.2`) and `configure-from-engine.sh` below, installed over a
copy of the published b2 (its arm64 wineserver, loader, renderer payloads and
`winegstreamer.so` kept), ran 2 h 10 min and 21 scene loads without a single
bad read, and Yaagl's wine-11.0 with the same DXMT 0.80 is clean too. The
published `x86_64-unix/*.so` carry `LC_BUILD_VERSION` tool `ld 27037.1`
against the same 27.0 SDK; the local ones `ld 1328.2`. Both use chained
fixups; neither contains AVX code. So the difference is the toolchain (or a
flag that differs from the recipe in this file, or the i386+x86_64 build),
not the source. Until that is pinned down, build the engine with Xcode 27.0's
linker, or at least run Genshin through a few teleports before publishing.

## Configuring against an installed engine

`configure-from-engine.sh <src> <build> [<engine>]` replaces `deps/`: the
headers come from this Mac's Homebrew (arch-neutral), the x86_64 dylibs
configure links and reads sonames from are the engine's own `wine/lib`, and
the sonames are pinned to the file names the engine's unix halves `dlopen`
(configure's `otool -L` parsing misreads `@rpath` install names otherwise and
writes `"\tlibgnutls.dylib (compatibility version …)"` into `config.h`, which
leaves `secur32` without schannel and the HoYoverse sign-in with "connection
failed"). It is x86_64-only and leaves GStreamer out, so keep the engine's
`winegstreamer.so`/`.dll` and i386 files. `make -k`: the `fonts/*.fon` targets
abort because `sfnt2fon` has no rpath to that freetype; nothing else needs
them.

## Configure

`build-macos/configure.sh` runs exactly this block against `$DORMISON_BUILD`; use it
rather than retyping the flags, since a tree configured with a different set rebuilds
everything and can differ in what it links.

```bash
W=${DORMISON_BUILD:-$HOME/dormison-build}
cd $W/build && \
CC='clang -arch x86_64 -std=gnu23 -m64' \
CFLAGS='-g -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0' \
CPPFLAGS="-I$W/deps/include" \
LDFLAGS="-L$W/deps/lib -Wl,-rpath,@loader_path/../lib -Wl,-rpath,@loader_path/../../" \
PKG_CONFIG=/usr/bin/false \
FREETYPE_CFLAGS="-I$W/deps/include/freetype2" FREETYPE_LIBS="-L$W/deps/lib -lfreetype" \
SDL2_CFLAGS="-I$W/deps/include/SDL2" SDL2_LIBS="-L$W/deps/lib -lSDL2" \
GSTREAMER_CFLAGS="$(PKG_CONFIG_LIBDIR=$W/gstreamer/lib/pkgconfig pkg-config --cflags \
  gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0)" \
GSTREAMER_LIBS="$(PKG_CONFIG_LIBDIR=$W/gstreamer/lib/pkgconfig pkg-config --libs \
  gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0 | \
  sed 's/-Wl,-rpath,[^ ]*//g')" \
OBJC=gcc PATH="/opt/homebrew/opt/bison/bin:$PATH" \
../wine-src/configure --build=x86_64-apple-darwin$(uname -r) --host=x86_64-apple-darwin$(uname -r) \
  --enable-archs=i386,x86_64 --enable-win64 --disable-tests --without-x \
  --with-gstreamer --without-ffmpeg --with-sdl --without-oss --without-alsa \
  --without-pulse --without-dbus --without-udev --without-usb --without-v4l2 \
  --without-gphoto --without-krb5 --without-netapi --without-fontconfig \
  --without-gssapi --without-opengl --without-capi --without-cups --without-pcap \
  --without-pcsclite --with-mingw --prefix=$W/stage
```

Configure reads every dependency from `deps/`. With
`pkg-config` or `brew` reachable, configure resolves FreeType to Homebrew's
arm64 build and `sfnt2fon` fails to link for x86_64; the two `FREETYPE_*`
variables and the disabled `pkg-config` keep configure on `deps/` whatever
the shell's PATH carries. GStreamer is the one dependency `deps/` does not
hold: `build-macos/fetch-gstreamer.sh` puts upstream's universal packages in
`gstreamer/`, and the two `GSTREAMER_*` variables read that tree through a
`PKG_CONFIG_LIBDIR` scoped to the command substitution, so the disabled
`pkg-config` inside configure still sees nothing else. The `sed` drops the
`-Wl,-rpath` that the relocatable `.pc` files emit: it names this machine's
build tree, and `winegstreamer.so` would carry that path for the life of the
engine. The engine's own `@loader_path/../../` reaches the same libraries.
Run the fetch script before configuring; without it configure builds no
`winegstreamer` and games that play a video through Media Foundation get a
black screen.

## The driver's Swift half

`dlls/winemac.drv/swift/` is built by its own makefile into
`libwinemacswift.a`, which `dlls/winemac.drv/Makefile.in` names in `UNIX_LIBS`.
`add_unix_libraries` in `tools/makedep.c` makes a static archive named by
path in `UNIX_LIBS` a prerequisite of the unix lib as well as a link input,
so wine's `make` relinks `winemac.so` after the archive changed. What no
makefile orders is the archive's own build: wine's makefiles do not know
Swift. **Build the archive before wine**, whenever a Swift source changed:

```bash
make -C $W/wine-src/dlls/winemac.drv/swift
```

The archive carries a build id, `sevo:winemacswift=<16 hex>`: the sha256 of
the Swift sources and their makefile, so the same sources give the same id
whenever they are built (`make -s -C dlls/winemac.drv/swift build-id` prints
the id the sources give now). The makefile generates `BuildID.swift` from it,
the driver prints it in its `sevo:run` line (`swift=…`), and
`package-engine.sh` reads it out of the archive and the staged `winemac.so`
with `strings`: it refuses to package when the archive's id is not the one the
sources give now (a Swift source changed after the archive was built) or when
the staged driver carries a different id (the driver was not relinked, or
`install-lib` was not re-run). Timestamps are not consulted: wine's installer
writes a fresh destination file, so a stale driver is younger than the archive
it was not linked against.

## Release names

A release is `dormison-r<N>` and a beta `dormison-b<N>`, the same `N` when the beta is what
becomes the release. `kagerou publish dormison -v b<N> -p` writes `channels.beta`;
`-v r<N>` writes `channels.stable`. The app shows either as "Dormison r<N>" / "Dormison b<N>"
and takes the channel Settings › Engine names (`sevo engine channel beta`).

## Build, stage, package

Packaging needs a donor: an engine Sevoflurane has already installed (the
newest `dormison-r*` or `dormison-b*` under `~/Library/Application Support/Sevoflurane/Engines`,
or the directory `SEVO_LIVE_ENGINE` names). The build produces wine; the
dependency dylibs (an x86_64 set built with MacPorts), the renderer bundles,
D3DMetal, gecko, mono and the dock shim are copied from the donor. A clone
with no engine installed builds wine but cannot package one.

```bash
make -C $W/wine-src/dlls/winemac.drv/swift   # before wine, see above
make -j10 -C $W/build                        # incremental, minutes
make -j10 -C $W/build install-lib
build-macos/package-engine.sh dormison-b1   # → ~/Library/Application Support/Sevoflurane/Engines/<version>
```

`package-engine.sh` takes wine from `stage/` and everything wine does not
produce (dependency dylibs, DXMT, DXVK, D3DMetal's shims, gecko, mono) from
the live engine — the one the app has installed, or the directory
`SEVO_LIVE_ENGINE` names — builds the dock shim, the Steam stub and the
Discord bridge from `build-macos/`, rewrites install names to `@rpath`,
re-signs, and writes `engine-info.json` (what the app reads) and
`engine-manifest.json` beside it.
To ship a shim change alone, build it the same way into the installed engine
and ad-hoc sign it (`codesign -s - -f`): the app loads it fresh with every
spawn, so a running client picks it up at the next launch.

### What packaging checks

Before it copies anything, `package-engine.sh` runs the gates in
`build-macos/engine-gates.sh`, each over what the files contain and never
over when they were written. `build-macos/tests/packaging-gates.sh` runs
them against made-up files, including a stale driver that is younger than
the archive, and exits 0 when every case is refused or accepted as it should
be.

- **The tree is committed.** `git status` must show no change to a tracked
  file, or the script refuses; `--allow-dirty` packages anyway and the
  manifest records `"dirty": true`. Untracked files do not count.
- **The Swift archive, its sources and the staged `winemac.so` carry one
  build id** (`sevo:winemacswift=…`, above).
- **`wineserver` and `ntdll.so` speak the same server protocol, and it is
  the one the source defines.** Both carry `sevo:server-protocol=<N>`
  (`server/main.c`, `dlls/ntdll/unix/server.c`), read with `strings` and
  compared with `SERVER_PROTOCOL_VERSION` in `include/wine/server_protocol.h`.
  The server is the native one when `build-native-server.sh` has built it,
  so a native tree left behind by a protocol change is refused rather than
  packaged beside a newer ntdll. A binary built from a source without the
  stamp is refused too: rebuild it.

Any refusal names the file and the command that repairs it.

### The manifest

`engine-manifest.json` lists every file in the engine with its sha256 (or the
target of a symlink) and where it came from: `build` (wine, from `stage/`),
`native-server`, `source` (built from `build-macos/`), `deps` (the pinned
downloads under `deps/` and `deps-raw/`), `gstreamer`, `donor` (copied from
the live engine) or `packaging` (written by the script). It also records the
source HEAD, `git describe`, whether the tree was dirty, the donor's path and
its own `engine-info.json` version and commit, the server's architecture and
protocol version, and the Swift build id. The payloads under `donor` are the
part a release still takes from an installed engine; the publish plan's deps
tarball is the step that replaces them, and the manifest names which files
that concerns. A file the script did not record an origin for fails the
packaging, so the list is complete by construction.

DXMT is packaged for both architectures. The x86_64 DLLs sit flat at the top
of `dxmt/`, which is where the app reads them from; the 32-bit ones go in
`dxmt/i386-windows/`, taken from the release tarball's own `i386-windows`
directory under `deps-raw/dxmt/<version>/` and falling back to the live
engine's copy. There is no 32-bit unix half to ship — the 32-bit DLLs reach
Metal through wow64 unix calls into the same 64-bit `winemetal.so`. Without
them a 32-bit D3D11 game renders through wined3d whatever renderer is picked.

Run a few games and the DirectX 12 samples against an engine before
installing it over the one the app runs; install with the client stopped,
keeping the previous file beside the new one.

## Fonts

`share/wine/fonts` in the engine holds wine's own faces (from `fonts/` in this
repository, built by `make install-lib`) plus two the build does not produce:
`ipag.ttf` (IPAGothic, fixed pitch) and `ipagp.ttf` (IPAPGothic,
proportional), from the IPA fonts 003.03 archive that
`build-macos/fetch-fonts.sh` downloads into `deps/fonts/` under a pinned URL
and sha256. `package-engine.sh` runs the script and copies the two faces with
`IPA_Font_License_Agreement_v1.0.txt` beside them; the IPA Font License v1.0
allows redistribution of the unmodified faces when that text comes along.
win32u scans the directory at every boot (`load_file_system_fonts` in
`dlls/win32u/font.c`), so a face there needs no registry entry. What makes a
Windows family name land on it is `HKCU\Software\Wine\Fonts\Replacements`,
written by the `[Fonts]` section of `loader/wine.inf.in`: MS Gothic reads
IPAGothic, the proportional Gothic families (MS PGothic, MS UI Gothic, Meiryo,
Yu Gothic) read IPAPGothic, and the Mincho families read the Mac's
`Hiragino Mincho ProN W3`, which win32u enumerates from CoreText under that
name, one family per weight. A replacement is skipped when the bottle has a
real font of that name, so a game that installs its own MS Gothic keeps it.
The inf carries a UTF-8 byte order mark because setupapi reads an inf with no
mark in the bottle's ANSI code page, which would garble the Japanese names.

To add a face: extend `fetch-fonts.sh` (or a sibling script) with the
archive, its sha256 and its license, add the copy to `package-engine.sh`, and
add one `HKCU,%FontReplStr%,"<Windows name>",,"<family as win32u enumerates
it>"` line per name to `[Fonts]`. The exact family string is the one the bottle's font
enumeration reports.

## msync+

msync+ started from CrossOver 26.3's msync (LGPL) and is maintained as commits on
`main`: the backend in `server/msync.c` and `dlls/ntdll/unix/msync.c`, and its
hooks in the server and ntdll files around them. A rebase carries it like any
other commit. The code, `WINEMSYNC` and the log prefixes keep the name msync.

A thread that dies between an object's compare-and-swap and the wake after it
leaves the object's sleepers asleep on a word that reads available. wineserver
wakes every object behind a dead process's handles when its last thread leaves
(`msync_process_killed`) and again at confirmed death (`msync_process_died`),
and a thread terminated in a live process gets the same pass once confirmed
(`msync_thread_died`). A pass that finds a sleeper logs
`sevo:msync backstop <exit|confirmed|thread> pid=… exe=… objects=… woke=…`.

`kill -USR2 <wineserver pid>` runs the lost-wake sweep: two passes 100 ms
apart, and every object that stayed available and unchanged between them is
woken. Each one that had a sleeper is reported as `sevo:msync lost-wake`, with
the processes that hold it and any of the last 16 deaths that shared it, on
wineserver's stderr and in `<prefix>/.sevo-msync-sweep.log`. `sevo sync sweep`
sends the signal and prints the log. `bispectral/lost-wake` measures the
backstop: it kills a process that sets an event another process sleeps on.

## DXMT

DXMT's `d3d11.dll` and `dxgi.dll` are a binary payload from the upstream
release tarball, so a change to DXMT itself is a patch here plus a rebuilt
payload, not an engine commit.
`patches/dxmt-log-airconv-failure.patch` applies to dxmt **v0.80** and makes
a failed DXBC-to-AIR translation name its stage and its shader hash at ERR
level; without it the only trace is one `Shader not found?` per dropped
Dispatch. Rebuilding the payload with it follows DXMT's own
`docs/DEVELOPMENT.md`, whose prerequisites — an x86_64 LLVM 15 built from
source, Xcode's Metal toolchain, mingw-w64 — are hours of setup on a machine
that has none of them:

```bash
cd path/to/dxmt && git worktree add /tmp/dxmt-v080 v0.80
cd /tmp/dxmt-v080
git apply $DORMISON/build-macos/patches/dxmt-log-airconv-failure.patch
# LLVM 15 for x86_64, per DXMT's docs/DEVELOPMENT.md § Setup LLVM
meson setup --cross-file build-win64.txt -Dnative_llvm_path=./toolchains/llvm \
  -Dwine_build_path=$DORMISON_BUILD/build build --buildtype release
meson compile -C build
cp build/src/d3d11/d3d11.dll "<engine>/dxmt/d3d11.dll"
codesign -s - -f "<engine>/dxmt/d3d11.dll"
```

Record the rebuild in the engine's `engine-info.json` `dxmt` field so a
payload that is no longer the stock v0.80 says so.

## Renderer provenance

winemac.drv prints which renderer answered for every process
(`sevo:gfx … renderer=… d3d11=<sha8>`). The values
come from `<engine>/renderer-hashes`, which the app writes when it stages a
renderer into the engine tree: `key=value` lines, `#` comments, the keys
`renderer` and `toolkit` plus one lower-case sha256 per staged DLL under its
own base name.

```
# written by Sevoflurane at renderer staging
renderer=dxmt
toolkit=v0.80
d3d11=1f0c…
d3d12=…
dxgi=…
```

A missing file or a missing key prints `unknown`; the renderer alone then
falls back to what `WINEDLLOVERRIDES` implies, which separates DXVK from
DXMT and leaves D3DMetal, wined3d and auto indistinguishable.

## Patches not on main

`patches/` holds diffs that exist but are not applied.
`kernelbase-steamwebhelper-in-process-gpu.patch` hangs the webhelper at
init from its second boot on, and the Vulkan portability patch covers what
it was for.

## Releasing

```bash
build-macos/publish-engine.sh b1 --key <ed25519.pem> --channel beta --dry-run   # pack, sign, check; nothing leaves the Mac
build-macos/publish-engine.sh b1 --key <ed25519.pem> --channel beta             # tag, GitHub release, manifest
build-macos/publish-engine.sh r1 --key <ed25519.pem>                            # a release, on the stable channel
```

A release is the engine directory `package-engine.sh` assembled, staged as
a copy, packed as `dormison-r<N>.tar.xz`, and published as GitHub release
`r<N>` of this repository with five assets: the tarball, `.sha256`, `.sig`,
`engine-info.json`, and the diff against `wine-staging-base`. The manifest
Sevoflurane reads (`engine.json` on this repository's `manifest` release, beside the
`shaders` release that holds the presenter's shader packages)
then names it as `channels.stable` (or `channels.beta`) and goes up with its
own `engine.json.sig`. The release body is the `## r<N>` section of
`CHANGES.md`, or `--notes-file`.

What the app verifies, and the script therefore produces:

- `.sig` files are raw Ed25519 signatures, base64, over the asset's bytes.
  The public half is pinned in the app (`EngineSignature.swift`); the app
  refuses a manifest without a valid `engine.json.sig` and a tarball without
  a valid `.sig`, and accepts assets only from
  `github.com/kageroumado/*/releases/download/`.
- The manifest is decoded by `sevo engine check-manifest` before anything
  uploads, so a shape the installed app cannot read never reaches it.
- After the upload, the tarball's digest as GitHub reports it is compared
  with the local sha256; the manifest is published only when they agree.

The script refuses a dirty tree, a version that is not `r<N>` or `b<N>` (a beta
goes to the beta channel and a release to stable), a tag that
exists locally or on origin, an engine packaged from a commit other than
HEAD, and a HEAD that is not on `origin/main`. Binaries carry the ad-hoc
signature `package-engine.sh` left; `--identity "Developer ID Application:
…"` re-signs every Mach-O in the staged copy with a secure timestamp,
leaving the installed engine as it was.

## Rebasing onto the next wine-staging

```bash
git fetch origin
git tag wine-staging-base-11.17 <the new staging commit>
git rebase --onto wine-staging-base-11.17 wine-staging-base main
```

Resolve the conflicts, reconfigure, build, test the result, and publish
with `publish-engine.sh`.

## The native server

wineserver runs no guest code, so it is the one process in the engine that need not
be translated; running natively removes Rosetta translation from the server
side of every wait, handle and APC a game makes. Packaged against the Rosetta
server it measures at parity with it (p50 0.98×, p99 1.01×, wall 1.02×, server CPU
0.96×); its case is one translated process fewer. `build-native-server.sh` configures a second,
arm64 tree for `server/` under `$DORMISON_BUILD/server-native` with
`-DDORMISON_X86_64_GUEST`, which makes `server/registry.c` report the x86 machines,
and builds a universal libinotify beside it; `server/mach.c` decides per process
whether a client is translated. `package-engine.sh` takes that server and that
libinotify when the tree exists, and `engine-info.json` says `"server": "arm64"`.

```bash
build-macos/build-native-server.sh            # once per source change; --reconfigure after a rebase
```

The x86_64 build's own wineserver stays in `stage/` untouched, so an engine packaged
without the native tree uses the translated server.
