#!/bin/bash
# Build wineserver natively for arm64 in its own configure tree, beside the
# x86_64 build. The server runs no guest code, so the engine's server is the
# one process that need not be translated; package-engine.sh takes the result
# from $DORMISON_BUILD/server-native when it exists. Everything else in the
# engine stays the x86_64 build.
#
# usage: build-native-server.sh            configure if needed, then make
#        build-native-server.sh --reconfigure
#
# Needs a universal libinotify under server-native/deps (built here from the
# 20240724 release the first time) and bison 3 ahead of the system one.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
ROOT="${DORMISON_BUILD:-$HOME/dormison-build}"
N="$ROOT/server-native"
INOTIFY=20240724
export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/bin:$PATH"

mkdir -p "$N/build" "$N/deps"

if [ ! -f "$N/deps/lib/libinotify.0.dylib" ]; then
    echo "==> libinotify $INOTIFY, universal"
    mkdir -p "$N/src" && cd "$N/src"
    [ -f "libinotify-$INOTIFY.tar.gz" ] || curl -sL -o "libinotify-$INOTIFY.tar.gz" \
        "https://github.com/libinotify-kqueue/libinotify-kqueue/releases/download/$INOTIFY/libinotify-$INOTIFY.tar.gz"
    tar -xzf "libinotify-$INOTIFY.tar.gz"
    cd "libinotify-$INOTIFY"
    ./configure --prefix="$N/deps" 'CFLAGS=-O2 -arch arm64 -arch x86_64' 'LDFLAGS=-arch arm64 -arch x86_64' > "$N/inotify-configure.log" 2>&1
    make -j6 > "$N/inotify-build.log" 2>&1 && make install >> "$N/inotify-build.log" 2>&1
    lipo -info "$N/deps/lib/libinotify.0.dylib"
fi

cd "$N/build"
if [ "${1:-}" = --reconfigure ] || [ ! -f Makefile ]; then
    echo "==> configuring the arm64 server tree against $REPO"
    "$REPO/configure" --enable-win64 --enable-archs=i386,x86_64 --disable-tests --without-x \
        --without-freetype --without-opengl --without-gnutls --without-vulkan --without-gstreamer \
        --without-sdl --without-ffmpeg --without-oss --without-alsa \
        'CC=clang -arch arm64 -std=gnu23' 'CFLAGS=-g -O2 -DDORMISON_X86_64_GUEST' \
        "CPPFLAGS=-I$N/deps/include" "LDFLAGS=-L$N/deps/lib" > "$N/configure.log" 2>&1
fi

echo "==> make server/wineserver"
make -j6 server/wineserver > "$N/build.log" 2>&1
file server/wineserver
otool -L server/wineserver | grep -i inotify
