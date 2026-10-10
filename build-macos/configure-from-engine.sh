#!/bin/bash
# Configure a Dormison tree against the dependencies of an installed engine instead of a
# deps/ directory of Intel Homebrew bottles: headers come from this Mac's (arm64) Homebrew,
# which are arch-neutral, and the x86_64 dylibs configure reads sonames from come from the
# engine's own wine/lib. The result links and dlopens exactly what the shipped engine does.
#
# Usage: configure-from-engine.sh <src-dir> <build-dir> [<engine-dir>]
#   engine-dir defaults to the engine Sevoflurane has active. Needs: xcode clang, mingw-w64,
#   bison 3, and Homebrew's gnutls, freetype, libpng, mpg123, gettext, molten-vk, vulkan-headers.
# SDL: if $DORMISON_BUILD/deps-sdl exists (README § winebus) it is used, else --without-sdl.
# GStreamer is left out: keep the engine's winegstreamer.so/.dll pair as it is.
#
# After it: make -k -j8. The fonts/*.fon targets abort (sfnt2fon runs x86_64 against the
# engine's freetype without an rpath); every dll and program still builds, and an engine
# already has its fonts.
set -eu
SRC="$1"; BLD="$2"
W=${DORMISON_BUILD:-$HOME/dormison-build}
HB=${HOMEBREW_PREFIX:-/opt/homebrew}
if [ -n "${3:-}" ]; then ENGINE="$3"; else
  ENGINE="$HOME/Library/Application Support/Sevoflurane/Engines/$(defaults read glass.kagerou.sevoflurane.shared engine 2>/dev/null | sed 's/^managed://' || true)"
  [ -d "$ENGINE/wine/lib" ] || ENGINE=$(ls -d "$HOME/Library/Application Support/Sevoflurane/Engines"/dormison-* | tail -1)
fi
ELIB="$ENGINE/wine/lib"
[ -d "$ELIB" ] || { echo "no engine wine/lib at $ENGINE" >&2; exit 1; }
mkdir -p "$BLD"
# Link directory: the engine's dylibs by their own names, SDL left to deps-sdl, mpg123's
# unversioned name added (the engine ships only libmpg123.1.dylib).
LIB="$BLD/deps-lib"; rm -rf "$LIB"; mkdir -p "$LIB"
for f in "$ELIB"/*.dylib; do n=$(basename "$f"); case "$n" in libSDL2*) continue;; esac; ln -s "$f" "$LIB/$n"; done
[ -e "$LIB/libmpg123.dylib" ] || ln -s "$ELIB/libmpg123.1.dylib" "$LIB/libmpg123.dylib"
# Sonames: the file names the engine's own unix halves dlopen.
so() { ls "$ELIB" | grep -E "$1" | head -1; }
SONAME_GNUTLS=$(so '^libgnutls\.[0-9]+\.dylib$')
SONAME_FREETYPE=$(so '^libfreetype\.[0-9]+\.dylib$')
SONAME_PNG=$(so '^libpng16\.[0-9]+\.dylib$')
SONAME_MPG123=$(so '^libmpg123\.[0-9]+\.dylib$')
SONAME_MOLTENVK=libMoltenVK.dylib
SONAME_VULKAN=$(strings -n 8 "$ELIB/wine/x86_64-unix/win32u.so" 2>/dev/null | grep -E '^libvulkan\.[0-9.]+\.dylib$' | head -1)
[ -n "$SONAME_VULKAN" ] || SONAME_VULKAN=$(so '^libvulkan\.1\.[0-9.]+\.dylib$')
echo "engine: $ENGINE"; echo "sonames: $SONAME_GNUTLS $SONAME_FREETYPE $SONAME_PNG $SONAME_MPG123 $SONAME_MOLTENVK $SONAME_VULKAN"
SDL_FLAG=--without-sdl; SDL_ENV=()
if [ -d "$W/deps-sdl/lib" ]; then SDL_FLAG=--with-sdl; SDL_ENV=(SDL2_CFLAGS="-I$W/deps-sdl/include/SDL2" SDL2_LIBS="-L$W/deps-sdl/lib -lSDL2"); fi
cd "$BLD"
env CC='clang -arch x86_64 -std=gnu23 -m64' \
  CFLAGS='-g -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0' \
  CPPFLAGS="-I$HB/include" \
  LDFLAGS="-L$LIB -Wl,-rpath,@loader_path/../lib -Wl,-rpath,@loader_path/../../" \
  PKG_CONFIG=/usr/bin/false \
  FREETYPE_CFLAGS="-I$HB/include/freetype2" FREETYPE_LIBS="-L$LIB -lfreetype" \
  GNUTLS_CFLAGS="-I$HB/include" GNUTLS_LIBS="-L$LIB -lgnutls" \
  PNG_CFLAGS="-I$HB/include" PNG_LIBS="-L$LIB -lpng16" \
  MPG123_CFLAGS="-I$HB/include" MPG123_LIBS="-L$LIB -lmpg123" \
  "${SDL_ENV[@]}" \
  ac_cv_lib_soname_gnutls="$SONAME_GNUTLS" ac_cv_lib_soname_freetype="$SONAME_FREETYPE" \
  ac_cv_lib_soname_vulkan="$SONAME_VULKAN" ac_cv_lib_soname_MoltenVK="$SONAME_MOLTENVK" \
  ac_cv_lib_soname_png="$SONAME_PNG" ac_cv_lib_soname_mpg123="$SONAME_MPG123" \
  OBJC=gcc PATH="$HB/opt/bison/bin:$PATH" \
  "$SRC/configure" --build=x86_64-apple-darwin$(uname -r) --host=x86_64-apple-darwin$(uname -r) \
    --enable-archs=x86_64 --enable-win64 --disable-tests --without-x \
    $SDL_FLAG --with-freetype --with-gnutls --with-vulkan --without-gstreamer --without-ffmpeg \
    --without-oss --without-alsa --without-pulse --without-dbus --without-udev --without-usb --without-v4l2 \
    --without-gphoto --without-krb5 --without-netapi --without-fontconfig --without-gssapi --without-opengl \
    --without-capi --without-cups --without-pcap --without-pcsclite --with-mingw --prefix="$W/stage"
grep -E 'SONAME_LIB(GNUTLS|FREETYPE|VULKAN|PNG|MPG123)' include/config.h
