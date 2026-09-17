#!/bin/bash
# The configure block from README.md, as a script so the two cannot drift: every
# flag here is one r9 was built with, plus SDL (winebus needs it for Bluetooth pads).
# usage: DORMISON_BUILD=<tree> build-macos/configure.sh [extra configure flags]
set -euo pipefail
W=${DORMISON_BUILD:-$HOME/Developer/build/dormison}
cd $W/build
CC='clang -arch x86_64 -std=gnu23 -m64' \
CFLAGS='-g -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0' \
CPPFLAGS="-I$W/deps/include" \
LDFLAGS="-L$W/deps/lib -Wl,-rpath,@loader_path/../lib -Wl,-rpath,@loader_path/../../" \
PKG_CONFIG=/usr/bin/false \
FREETYPE_CFLAGS="-I$W/deps/include/freetype2" FREETYPE_LIBS="-L$W/deps/lib -lfreetype" \
SDL2_CFLAGS="-I$W/deps/include/SDL2" SDL2_LIBS="-L$W/deps/lib -lSDL2" \
GSTREAMER_CFLAGS="$(PKG_CONFIG_LIBDIR=$W/gstreamer/lib/pkgconfig pkg-config --cflags gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0)" \
GSTREAMER_LIBS="$(PKG_CONFIG_LIBDIR=$W/gstreamer/lib/pkgconfig pkg-config --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0 | sed 's/-Wl,-rpath,[^ ]*//g')" \
OBJC=gcc PATH="/opt/homebrew/opt/bison/bin:$PATH" \
../wine-src/configure --build=x86_64-apple-darwin$(uname -r) --host=x86_64-apple-darwin$(uname -r) \
  --enable-archs=i386,x86_64 --enable-win64 --disable-tests --without-x \
  --with-gstreamer --without-ffmpeg --with-sdl --without-oss --without-alsa \
  --without-pulse --without-dbus --without-udev --without-usb --without-v4l2 \
  --without-gphoto --without-krb5 --without-netapi --without-fontconfig \
  --without-gssapi --without-opengl --without-capi --without-cups --without-pcap \
  --without-pcsclite --with-mingw --prefix=$W/stage \
  "$@"
