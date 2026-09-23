#!/bin/bash
# Fetch the IPA fonts the engine ships beside wine's own.
#
# usage: fetch-fonts.sh
#
# IPAGothic (ipag.ttf, fixed pitch) and IPAPGothic (ipagp.ttf, proportional)
# are the faces the Japanese family names a Windows program asks for --
# MS Gothic, MS PGothic, MS UI Gothic, Meiryo, Yu Gothic -- resolve to, through
# the Replacements wine.inf writes. They are the release archive of IPA fonts
# 003.03, distributed under the IPA Font License v1.0, which allows
# redistribution with the license text alongside; package-engine.sh copies
# the two faces and the license into the engine's share/wine/fonts.
#
# The archive is pinned by URL and sha256 and expanded once into
# DORMISON_BUILD/deps/fonts; a second run verifies and returns.
set -euo pipefail

ROOT="${DORMISON_BUILD:-$HOME/dormison-build}"
OUT="$ROOT/deps/fonts"
ARCHIVE="IPAfont00303.zip"
URL="https://moji.or.jp/wp-content/ipafont/IPAfont/$ARCHIVE"
SHA256="f755ed79a4b8e715bed2f05a189172138aedf93db0f465b4e20c344a02766fe5"
DIR="$OUT/IPAfont00303"

mkdir -p "$OUT"

if [ ! -f "$OUT/$ARCHIVE" ]; then
    echo "==> downloading $ARCHIVE"
    curl -fL --retry 3 -o "$OUT/$ARCHIVE.part" "$URL"
    mv "$OUT/$ARCHIVE.part" "$OUT/$ARCHIVE"
fi

actual="$(shasum -a 256 "$OUT/$ARCHIVE" | awk '{print $1}')"
if [ "$actual" != "$SHA256" ]; then
    echo "$OUT/$ARCHIVE: sha256 $actual, expected $SHA256"
    exit 1
fi

for f in ipag.ttf ipagp.ttf IPA_Font_License_Agreement_v1.0.txt; do
    [ -f "$DIR/$f" ] && continue
    echo "==> expanding $ARCHIVE"
    unzip -qo "$OUT/$ARCHIVE" -d "$OUT"
    break
done

for f in ipag.ttf ipagp.ttf IPA_Font_License_Agreement_v1.0.txt; do
    [ -f "$DIR/$f" ] || { echo "missing $DIR/$f after expanding $ARCHIVE"; exit 1; }
done

echo "==> fonts at $DIR"
