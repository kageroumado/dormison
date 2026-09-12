#!/bin/bash
# Fetch the GStreamer that winegstreamer builds against and the engine ships.
#
# usage: fetch-gstreamer.sh [version]   default 1.28.7
#
# Upstream's macOS packages are universal, so one download serves both the
# x86_64 slices the engine runs and the headers configure reads. They install
# into /Library/Frameworks; this expands them into DORMISON_BUILD/gstreamer
# instead, leaving anything the machine already has alone.
#
# Only the components the engine uses are merged. The GPL ones -- codecs-gpl,
# codecs-gpl-restricted, dvd-gpl -- are left out: the engine ships under its
# own terms and those plugins would set the terms of the whole tree.
set -euo pipefail

VERSION="${1:-1.28.7}"
ROOT="${DORMISON_BUILD:-$HOME/dormison-build}"
RAW="$ROOT/deps-raw/gstreamer"
OUT="$ROOT/gstreamer"
BASE="https://gstreamer.freedesktop.org/data/pkg/osx/$VERSION"

# codecs-restricted carries asfdemux, which is how a Windows Media file is read.
# libav carries the AAC decoder: upstream's applemedia has VideoToolbox for
# video but no AudioToolbox decoder, so an MP4's audio has no other route.
COMPONENTS="base-system-1.0 gstreamer-1.0-core gstreamer-1.0-playback \
gstreamer-1.0-codecs gstreamer-1.0-codecs-restricted gstreamer-1.0-system \
gstreamer-1.0-effects gstreamer-1.0-libav"
DEVEL="base-system-1.0-devel gstreamer-1.0-core-devel gstreamer-1.0-codecs-devel"

mkdir -p "$RAW" "$OUT"

for kind in "" "-devel"; do
    pkg="gstreamer-1.0$kind-$VERSION-universal.pkg"
    if [ ! -f "$RAW/$pkg" ]; then
        echo "==> downloading $pkg"
        curl -fL --retry 3 -o "$RAW/$pkg.part" "$BASE/$pkg"
        mv "$RAW/$pkg.part" "$RAW/$pkg"
    fi
    dir="$RAW/expanded$kind"
    [ -d "$dir" ] || { echo "==> expanding $pkg"; pkgutil --expand-full "$RAW/$pkg" "$dir"; }
done

echo "==> merging components into $OUT"
for c in $COMPONENTS; do
    src="$RAW/expanded/$c-$VERSION-universal.pkg/Payload"
    [ -d "$src" ] || { echo "missing component $c"; exit 1; }
    ditto "$src" "$OUT"
done
for c in $DEVEL; do
    src="$RAW/expanded-devel/${c%-devel}-devel-$VERSION-universal.pkg/Payload"
    [ -d "$src" ] || { echo "missing devel component $c"; exit 1; }
    ditto "$src" "$OUT"
done

# The .pc files resolve their own prefix from ${pcfiledir}, so a moved tree
# still describes itself; configure needs no rewriting of them.
echo "==> done: $OUT"
echo "    configure reads $OUT/lib/pkgconfig, the engine ships the x86_64 slices"
du -sh "$OUT"
