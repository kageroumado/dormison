#!/bin/bash
# Assemble a Sevoflurane engine directory from a wine `make install-lib` staging tree,
# in the layout the live engine uses. Everything that is not wine itself — the ~95
# dependency dylibs, the DXMT and DXVK bundles, gecko/mono, the dock shim — is copied from
# the live engine, which stays read-only. D3DMetal is not: the app places the user's own
# copy of Apple's toolkit into each engine it runs.
#
# usage: package-engine.sh <version>   e.g. dormison-r1
#
# The wine build tree lives outside the repository (DORMISON_BUILD), and
# the live engine is the one the app has installed (SEVO_LIVE_ENGINE names
# another): build-macos/README.md.
set -euo pipefail

VERSION="$1"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
ROOT="${DORMISON_BUILD:-$HOME/dormison-build}"
STAGE="$ROOT/stage"
ENGINES="$HOME/Library/Application Support/Sevoflurane/Engines"
# The newest installed Dormison unless SEVO_LIVE_ENGINE names another.
LIVE="${SEVO_LIVE_ENGINE:-$ENGINES/$(ls "$ENGINES" 2>/dev/null | grep -E '^dormison-r[0-9]+$' | sort -t- -k2.2 -n | tail -1)}"
OUT="$ENGINES/$VERSION"

[ -d "$STAGE/bin" ] || { echo "no staging tree at $STAGE"; exit 1; }
[ -n "$LIVE" ] && [ -d "$LIVE" ] || { echo "no live engine under $ENGINES (SEVO_LIVE_ENGINE names one)"; exit 1; }
[ -e "$OUT" ] && { echo "$OUT already exists — refusing to overwrite"; exit 1; }

# --- the driver's Swift half is linked into winemac.so, so it is built before
# `make`, not here; wine's makefiles cannot order it. Refuse to package a tree
# where the archive is missing or newer than the winemac.so meant to carry it. ---
SWIFT_A="$REPO/dlls/winemac.drv/swift/libwinemacswift.a"
[ -f "$SWIFT_A" ] || {
    echo "missing $SWIFT_A — run 'make -C $REPO/dlls/winemac.drv/swift', then rebuild wine"
    exit 1
}
if [ "$SWIFT_A" -nt "$STAGE/lib/wine/x86_64-unix/winemac.so" ]; then
    echo "$SWIFT_A is newer than the staged winemac.so — rebuild wine and re-run install-lib"
    exit 1
fi

echo "==> assembling $OUT"
mkdir -p "$OUT/wine/lib/wine" "$OUT/wine/share"

# --- wine proper, from our build ---
cp -R "$STAGE/bin" "$OUT/wine/bin"

for a in x86_64-unix x86_64-windows i386-windows; do
    [ -d "$STAGE/lib/wine/$a" ] && cp -R "$STAGE/lib/wine/$a" "$OUT/wine/lib/wine/$a"
done
cp -R "$STAGE/share/wine" "$OUT/wine/share/wine"

# --- the IPA fonts, beside wine's own: win32u scans share/wine/fonts at every
# boot, and the Replacements wine.inf writes point the Japanese family names
# at IPAGothic and IPAPGothic. The license rides with the faces it covers. ---
echo "==> IPA fonts"
"$HERE/fetch-fonts.sh" | sed 's/^/    /'
for f in ipag.ttf ipagp.ttf IPA_Font_License_Agreement_v1.0.txt; do
    cp "$ROOT/deps/fonts/IPAfont00303/$f" "$OUT/wine/share/wine/fonts/$f"
done

# --- strip the PE builds: install-lib leaves full debug info, 1.3 GB of it ---
echo "==> stripping PE modules"
for f in "$OUT"/wine/lib/wine/x86_64-windows/*.dll "$OUT"/wine/lib/wine/x86_64-windows/*.exe; do
    [ -f "$f" ] && x86_64-w64-mingw32-strip --strip-all "$f" 2>/dev/null
done
for f in "$OUT"/wine/lib/wine/i386-windows/*.dll "$OUT"/wine/lib/wine/i386-windows/*.exe; do
    [ -f "$f" ] && i686-w64-mingw32-strip --strip-all "$f" 2>/dev/null
done

# --- dependency dylibs, from the live engine (built by MacPorts, not rebuilt here) ---
find "$LIVE/wine/lib" -maxdepth 1 -name '*.dylib' -exec cp -a {} "$OUT/wine/lib/" \;
# lib/external is where the app places the user's own D3DMetal (Apple's Game Porting
# Toolkit, which may not be redistributed): the engine ships the folder empty.
mkdir -p "$OUT/wine/lib/external"

# --- the server, native when build-native-server.sh has built it: it runs no
# guest code, so it is the one process that need not be translated. Its
# libinotify is the universal build, which winebus.so shares. ---
NATIVE_SERVER="$ROOT/server-native/build/server/wineserver"
if [ -f "$NATIVE_SERVER" ]; then
    echo "==> native arm64 wineserver"
    cp "$NATIVE_SERVER" "$OUT/wine/bin/wineserver"
    cp "$ROOT/server-native/deps/lib/libinotify.0.dylib" "$OUT/wine/lib/libinotify.0.dylib"
    install_name_tool -id "@rpath/libinotify.0.dylib" "$OUT/wine/lib/libinotify.0.dylib"
    otool -L "$OUT/wine/bin/wineserver" | awk '/libinotify/ {print $1}' | while read -r dep; do
        install_name_tool -change "$dep" "@rpath/libinotify.0.dylib" "$OUT/wine/bin/wineserver"
    done
    lipo -info "$OUT/wine/lib/libinotify.0.dylib" | sed 's/^/    /'
    SERVER_ARCH=arm64
else
    SERVER_ARCH=x86_64
fi

# --- every library configure recorded by name: this build dlopens exactly that
# file (win32u opens SONAME_LIBVULKAN, for one), so a name the live engine
# lacks comes from deps/, which is what configure resolved it against. ---
echo "==> checking the sonames configure recorded"
sed -n 's/^#define SONAME_LIB[A-Z0-9_]* "\(.*\)"/\1/p' "$ROOT/build/include/config.h" | while read -r name; do
    [ -e "$OUT/wine/lib/$name" ] && continue
    src="$ROOT/deps/lib/$name"
    if [ ! -f "$src" ]; then
        echo "    $name: in neither the live engine nor deps; the module that opens it will run without it"
        continue
    fi
    echo "    $name: from deps"
    cp "$src" "$OUT/wine/lib/$name"
    install_name_tool -id "@rpath/$name" "$OUT/wine/lib/$name"
    otool -L "$OUT/wine/lib/$name" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        case "$dep" in
            "$ROOT"/deps/lib/*) install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$OUT/wine/lib/$name" ;;
        esac
    done
    codesign --force --sign - --timestamp=none "$OUT/wine/lib/$name"
done

# --- gecko and mono, which a wine build does not produce ---
for d in gecko mono; do
    [ -d "$LIVE/wine/share/wine/$d" ] && cp -R "$LIVE/wine/share/wine/$d" "$OUT/wine/share/wine/$d"
done

# --- GStreamer, which Media Foundation reaches through winegstreamer.
# The plugins named here are the ones a game's video and audio go through;
# everything else they link is found by walking their load commands, so a
# version that splits a library differently still packages completely.
# Upstream ships universal binaries and the engine runs x86_64, so each file
# is thinned on the way in. Install names are already @rpath-relative and the
# plugins carry an @loader_path/.. rpath, which is why libraries land flat in
# wine/lib and plugins one directory below it: nothing needs rewriting.
# mpegpsdemux reads MPEG-1 and MPEG-2 program streams (.mpg, .vob), the
# movies of DirectShow-era visual novels; the parsers and decoders behind it
# (mpegvideoparse, mpegaudioparse, mpg123, avdec_mpeg2video) are here for
# other files already. ---
GST="$ROOT/gstreamer"
if [ -d "$GST/lib/gstreamer-1.0" ]; then
    echo "==> GStreamer plugins and libraries"
    GST_PLUGINS="coreelements typefindfunctions audioconvert audioresample \
videoconvertscale videofilter app playback isomp4 audioparsers videoparsersbad \
wavparse id3demux avi matroska vpx opus vorbis ogg flac mpg123 theora asf \
mpegpsdemux applemedia deinterlace libav"
    queue=""
    for p in $GST_PLUGINS; do
        f="$GST/lib/gstreamer-1.0/libgst$p.dylib"
        [ -f "$f" ] || { echo "    missing plugin $p"; exit 1; }
        queue="$queue $f"
    done
    seen=""
    while [ -n "$queue" ]; do
        next=""
        for f in $queue; do
            base="$(basename "$f")"
            case " $seen " in *" $base "*) continue ;; esac
            seen="$seen $base"
            # A name the engine already carries stays the engine's: libz, libbz2,
            # libintl and libMoltenVK are shared with wine itself, and a second
            # copy under the same install name would be the one some modules got.
            case "$f" in
                */gstreamer-1.0/*) dest="$OUT/wine/lib/gstreamer-1.0" ;;
                *) dest="$OUT/wine/lib" ;;
            esac
            mkdir -p "$dest"
            if [ -e "$OUT/wine/lib/$base" ] && [ "$dest" = "$OUT/wine/lib" ]; then
                echo "    $base: the engine's own copy is kept"
            else
                lipo "$f" -thin x86_64 -output "$dest/$base" 2>/dev/null || cp "$f" "$dest/$base"
                codesign --force --sign - --timestamp=none "$dest/$base" 2>/dev/null || true
            fi
            for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}' | sed -n 's|^@rpath/||p'); do
                case " $seen " in *" $dep "*) continue ;; esac
                [ -f "$GST/lib/$dep" ] && next="$next $GST/lib/$dep"
            done
        done
        queue="$next"
    done
    FEATURES='"env-files", "discord-bridge", "media"'
else
    echo "warning: no GStreamer at $GST — run build-macos/fetch-gstreamer.sh; video stays silent and black"
    FEATURES='"env-files", "discord-bridge"'
fi

# --- pieces DXMT and D3DMetal contribute that a wine build does not produce ---
# winemetal is DXMT's Metal backend, shipped as a matched .dll/.so pair; nvngx and nvapi64
# likewise come from DXMT. The ICD manifest points the Vulkan loader at libMoltenVK.
cp -R "$LIVE/wine/lib/wine/x86_64-unix/vulkan" "$OUT/wine/lib/wine/x86_64-unix/vulkan"
cp "$LIVE/wine/lib/wine/x86_64-unix/winemetal.so" "$OUT/wine/lib/wine/x86_64-unix/"
for dll in winemetal.dll nvngx.dll nvapi64.dll nvngx-on-metalfx.dll; do
    src="$LIVE/wine/lib/wine/x86_64-windows-original/$dll"
    [ -f "$src" ] || src="$LIVE/wine/lib/wine/x86_64-windows/$dll"
    [ -f "$src" ] && cp "$src" "$OUT/wine/lib/wine/x86_64-windows/$dll"
done

# --- D3DMetal's unix-side shims: symlinks into ../../external/libd3dshared.dylib ---
for so in atidxx64 d3d10 d3d11 d3d12 dxgi nvapi64 nvngx-on-metalfx; do
    ln -sf ../../external/libd3dshared.dylib "$OUT/wine/lib/wine/x86_64-unix/$so.so"
done

# --- renderer bundles, verbatim ---
for d in dxmt dxvk; do
    [ -d "$LIVE/$d" ] && cp -R "$LIVE/$d" "$OUT/$d"
done

# --- DXMT's 32-bit half, in a subdirectory of the same payload dir. A 32-bit
# game's d3d11 is a separate PE from the 64-bit one and has to be swapped in
# its own tree; these reach Metal through wow64 unix calls into the very same
# 64-bit winemetal.so, so there is no 32-bit unix half to ship. The x86_64 set
# stays flat at the top of dxmt/, which is where the app reads it from.
# The release tarball carries both: dxmt-v<n>-builtin.tar.gz has i386-windows
# beside x86_64-windows. ---
DXMT32=""
for d in "$ROOT"/deps-raw/dxmt/*/i386-windows "$LIVE/dxmt/i386-windows"; do
    [ -d "$d" ] && DXMT32="$d"
done
if [ -n "$DXMT32" ]; then
    echo "==> 32-bit DXMT from $DXMT32"
    mkdir -p "$OUT/dxmt/i386-windows"
    cp "$DXMT32"/*.dll "$OUT/dxmt/i386-windows/"
else
    echo "warning: no 32-bit DXMT payload — 32-bit D3D11 goes through wined3d"
fi

# --- the dock shim, from this repository's source: it is the engine's face ---
echo "==> building the dock shim"
clang -arch arm64 -arch x86_64 -O2 -Wall -dynamiclib -framework ApplicationServices \
    -o "$OUT/libsevodockshim.dylib" "$HERE/dock-shim/sevo_dock_shim.c"
codesign -s - -f "$OUT/libsevodockshim.dylib"

# --- the Steamworks stub a natively run game talks to, both bitnesses (steam-stub/README.md) ---
make -s -C "$HERE/steam-stub"
cp "$HERE/steam-stub/sevo-steamstub.exe" "$HERE/steam-stub/sevo-steamstub32.exe" "$OUT/"

# --- the Discord relay a game in the bottle reaches the Mac client through ---
make -s -C "$HERE/discord-bridge"
cp "$HERE/discord-bridge/sevo-discord-bridge.exe" "$OUT/"

# --- make our binaries resolve the bundled dylibs through @rpath ---
echo "==> rewriting install names to @rpath"
fix_rpath() {
    local f="$1" rel="$2"
    otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | \
    while read -r dep; do
        case "$dep" in
            "$ROOT"/deps/lib/*|/usr/local/*|/opt/homebrew/*)
                install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$f" 2>/dev/null ;;
        esac
    done
    otool -l "$f" | grep -q "$rel" || install_name_tool -add_rpath "$rel" "$f" 2>/dev/null || true
}
for f in "$OUT"/wine/bin/*; do [ -f "$f" ] && fix_rpath "$f" "@loader_path/../lib"; done
for f in "$OUT"/wine/lib/wine/x86_64-unix/*.so; do [ -f "$f" ] && ! [ -L "$f" ] && fix_rpath "$f" "@loader_path/../../"; done

echo "==> re-signing (install_name_tool invalidates ad-hoc signatures)"
find "$OUT/wine/bin" "$OUT/wine/lib/wine/x86_64-unix" -type f ! -type l \
    -exec codesign --force --sign - --timestamp=none {} \; 2>/dev/null || true

cat > "$OUT/engine-info.json" <<EOF
{
  "version": "$VERSION",
  "repository": "https://github.com/kageroumado/dormison",
  "commit": "$(git -C "$REPO" rev-parse HEAD)",
  "wine": "11.16 + wine-staging 11.16",
  "dxmt": "https://github.com/3Shain/dxmt/releases/download/v0.80/dxmt-v0.80-builtin.tar.gz",
  "dxvk": "https://github.com/Gcenx/DXVK-macOS/releases/download/v1.10.3-20230507-repack/dxvk-macOS-async-v1.10.3-20230507-repack-builtin.tar.gz",
  "d3dmetal": "Apple Game Porting Toolkit payloads installed by the app under d3dmetal/",
  "sync": "msync (WINEMSYNC=1)",
  "server": "$SERVER_ARCH",
  "renderers": ["auto", "dxmt", "dxvk", "d3dmetal", "wined3d"],
  "features": [$FEATURES]
}
EOF

echo "==> done: $OUT"
du -sh "$OUT"
