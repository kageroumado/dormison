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

The dependencies need no x86_64 Homebrew. Only libinotify (`winebus.so`,
`wineserver`) is linked at build time — ffmpeg and gstreamer would be, and
the configure line below leaves them out; everything else — freetype, gnutls, MoltenVK,
the Vulkan loader, libpng, jpeg-turbo, mpg123, gettext and gnutls's own
dependencies — is `dlopen`ed by soname at runtime, so configure needs only
their headers plus an x86_64 dylib to read the soname from. Fetch the
`sonoma` bottles of those formulas from Homebrew's registry (`tahoe` has no
Intel bottles), extract them, and merge `include/`, `lib/` and `bin/` into
`deps/`, rewriting each `.pc` file's prefix to that directory. Homebrew no
longer ships `sdl2`, hence `--without-sdl`: `winebus.sys` uses its IOHID
backend, which is the one that matters on macOS anyway.

## Configure

```bash
W=${DORMISON_BUILD:-$HOME/dormison-build}
cd $W/build && \
CC='clang -arch x86_64 -std=gnu23 -m64' \
CFLAGS='-g -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0' \
CPPFLAGS="-I$W/deps/include" \
LDFLAGS="-L$W/deps/lib -Wl,-rpath,@loader_path/../lib -Wl,-rpath,@loader_path/../../" \
PKG_CONFIG=/usr/bin/false \
FREETYPE_CFLAGS="-I$W/deps/include/freetype2" FREETYPE_LIBS="-L$W/deps/lib -lfreetype" \
OBJC=gcc PATH="/opt/homebrew/opt/bison/bin:$PATH" \
../wine-src/configure --build=x86_64-apple-darwin$(uname -r) --host=x86_64-apple-darwin$(uname -r) \
  --enable-archs=i386,x86_64 --enable-win64 --disable-tests --without-x \
  --without-gstreamer --without-ffmpeg --without-sdl --without-oss --without-alsa \
  --without-pulse --without-dbus --without-udev --without-usb --without-v4l2 \
  --without-gphoto --without-krb5 --without-netapi --without-fontconfig \
  --without-gssapi --without-opengl --without-capi --without-cups --without-pcap \
  --without-pcsclite --with-mingw --prefix=$W/stage
```

Configure reads every dependency from `deps/`. With
`pkg-config` or `brew` reachable, configure resolves FreeType to Homebrew's
arm64 build and `sfnt2fon` fails to link for x86_64; the two `FREETYPE_*`
variables and the disabled `pkg-config` keep configure on `deps/` whatever
the shell's PATH carries.

## The driver's Swift half

`dlls/winemac.drv/swift/` is built by its own makefile into
`libwinemacswift.a`, which `dlls/winemac.drv/Makefile.in` names in `UNIX_LIBS`.
Wine's makefile generator passes a plain path in `UNIX_LIBS` to the link line
and makes no prerequisite out of it (`add_unix_libraries` in
`tools/makedep.c` resolves only `-lfoo` to a built library), and the top-level
Makefile's prologue comes from `configure.ac`, so there is nowhere in the
repository to express the ordering. **Build the archive before wine**, whenever
a Swift source changed:

```bash
make -C $W/wine-src/dlls/winemac.drv/swift
```

`package-engine.sh` refuses to package when the archive is missing or newer
than the staged `winemac.so`, which is the shape a forgotten rebuild takes.

## Build, stage, package

Packaging needs a donor: an engine Sevoflurane has already installed (the
newest `dormison-r*` under `~/Library/Application Support/Sevoflurane/Engines`,
or the directory `SEVO_LIVE_ENGINE` names). The build produces wine; the
dependency dylibs (an x86_64 set built with MacPorts), the renderer bundles,
D3DMetal, gecko, mono and the dock shim are copied from the donor. A clone
with no engine installed builds wine but cannot package one.

```bash
make -C $W/wine-src/dlls/winemac.drv/swift   # before wine, see above
make -j10 -C $W/build                        # incremental, minutes
make -j10 -C $W/build install-lib
build-macos/package-engine.sh dormison-r1   # → ~/Library/Application Support/Sevoflurane/Engines/<version>
```

`package-engine.sh` takes wine from `stage/` and everything wine does not
produce (dependency dylibs, DXMT, DXVK, D3DMetal's shims, gecko, mono, the
dock shim) from the live engine — the one the app has installed, or the
directory `SEVO_LIVE_ENGINE` names — rewrites install names to
`@rpath`, re-signs, and writes `engine-info.json` with the commit subjects
as the patch list, and builds the dock shim from `dock-shim/` into the engine.
To ship a shim change alone, build it the same way into the installed engine
and ad-hoc sign it (`codesign -s - -f`): the app loads it fresh with every
spawn, so a running client picks it up at the next launch.

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

## msync

`msync-src/` is CrossOver 26.3's msync (six files, LGPL) and
`apply-msync.py` is the port that placed them and rewrote the eight hook
sites in 11.16. It is already applied on `main`; it stays here for the next
rebase, where the anchors will need checking.

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
(`sevo:gfx … renderer=… d3d11=<sha8>`, see `CHANGES.md` § r5). The values
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
build-macos/publish-engine.sh r3 --key <ed25519.pem> --dry-run   # pack, sign, check; nothing leaves the Mac
build-macos/publish-engine.sh r3 --key <ed25519.pem>             # tag, GitHub release, manifest
build-macos/publish-engine.sh r4 --key <ed25519.pem> --channel beta
```

A release is the engine directory `package-engine.sh` assembled, staged as
a copy, packed as `dormison-r<N>.tar.xz`, and published as GitHub release
`r<N>` of this repository with five assets: the tarball, `.sha256`, `.sig`,
`engine-info.json`, and the diff against `wine-staging-base`. The manifest
Sevoflurane reads (`engine.json` on the app repository's `engine` release)
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

The script refuses a dirty tree, a version that is not `r<N>`, a tag that
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
