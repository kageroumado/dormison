#!/bin/bash
# Assembles the Steamworks SDK header set the bridge generator reads: one
# steamworks_sdk_<ver> directory per version, from Proton's lsteamclient at a pinned
# commit, plus steamworks_sdk_153 (the initial 1.53 release, which Proton skipped):
# 1.53a's headers with the four networking headers of that release from
# gen/sdk-overlay (GameNetworkingSockets' copies, see the README there).
#
# usage: fetch-sdk.sh <sdk-dir>
#
# PROTON_DIR names a Proton checkout to link instead of cloning; otherwise a blob-less
# sparse clone of lsteamclient lands in $DORMISON_BUILD/proton.
set -euo pipefail

SDK="${1:?usage: fetch-sdk.sh <sdk-dir>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PROTON_COMMIT=bc896217ffcd67f892f952b010f898fe7bd0f0a1
ROOT="${DORMISON_BUILD:-$HOME/dormison-build}"

if [ -n "${PROTON_DIR:-}" ] && [ -d "$PROTON_DIR/lsteamclient/steamworks_sdk_165" ]; then
    SOURCE="$PROTON_DIR/lsteamclient"
    echo "==> SDK headers from $SOURCE"
else
    CLONE="$ROOT/proton"
    if [ ! -d "$CLONE/.git" ]; then
        echo "==> cloning Proton's lsteamclient at $PROTON_COMMIT"
        git clone --filter=blob:none --no-checkout --sparse https://github.com/ValveSoftware/Proton.git "$CLONE"
        git -C "$CLONE" sparse-checkout set lsteamclient
    fi
    if [ "$(git -C "$CLONE" rev-parse HEAD 2>/dev/null)" != "$PROTON_COMMIT" ]; then
        git -C "$CLONE" fetch --depth 1 origin "$PROTON_COMMIT"
        git -C "$CLONE" checkout --quiet "$PROTON_COMMIT"
    fi
    SOURCE="$CLONE/lsteamclient"
fi

mkdir -p "$SDK"
count=0
for dir in "$SOURCE"/steamworks_sdk_*; do
    [ -d "$dir" ] || continue
    ln -sfn "$dir" "$SDK/$(basename "$dir")"
    count=$((count + 1))
done
echo "==> $count SDK versions linked"

# 1.53: 1.53a without the fake-IP header (that struct still lived in the types header)
# and with the networking headers of the initial release.
S153="$SDK/steamworks_sdk_153"
if [ -e "$S153" ] && [ ! -L "$S153" ]; then trash "$S153"; fi
[ -L "$S153" ] && unlink "$S153"
cp -R "$SOURCE/steamworks_sdk_153a/" "$S153"
trash "$S153/steamnetworkingfakeip.h"
cp "$HERE"/gen/sdk-overlay/steamworks_sdk_153/*.h "$S153/"
echo "==> steamworks_sdk_153 synthesized ($(ls "$S153" | wc -l | tr -d ' ') files)"
