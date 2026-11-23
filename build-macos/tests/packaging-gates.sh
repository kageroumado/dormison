#!/bin/bash
# The packaging gates (engine-gates.sh) against files made up in a temporary
# directory. The first case is the one the 2026-09-26 review demonstrated:
# wine's installer writes a fresh winemac.so with the old bytes, so the stale
# driver is younger than the archive it was not linked against, and a
# timestamp comparison lets it through. A content identity must refuse it.
#
#   build-macos/tests/packaging-gates.sh      exit 0 when every case behaves
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../engine-gates.sh
. "$HERE/../engine-gates.sh"

T="$(mktemp -d -t packaging-gates)"
trap 'rm -rf "$T"' EXIT
FAILED=0

pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; FAILED=1; }

# A file that carries one embedded stamp among other bytes, like a binary.
binary_with() {
    local path="$1" stamp="$2"
    { head -c 512 /dev/zero; printf 'padding-before\n%s\0padding-after\n' "$stamp"; head -c 512 /dev/zero; } > "$path"
}

expect_refusal() {
    local name="$1" needle="$2" output
    shift 2
    if output="$("$@" 2>&1)"; then
        fail "$name: accepted, expected a refusal"
        return
    fi
    case "$output" in
        *"$needle"*) pass "$name" ;;
        *) fail "$name: refused, but the message does not name the cause: $output" ;;
    esac
}

expect_acceptance() {
    local name="$1" output
    shift
    if output="$("$@" 2>&1)"; then
        pass "$name"
    else
        fail "$name: refused: $output"
    fi
}

# --- the Swift archive and the staged driver ---
OLD=0123456789abcdef
NEW=fedcba9876543210
binary_with "$T/archive.a" "sevo:winemacswift=$NEW"
binary_with "$T/winemac.so" "sevo:winemacswift=$OLD"
# The stale driver is younger than the archive, as install-lib leaves it.
touch -t 202601010000 "$T/archive.a"
touch "$T/winemac.so"
[ "$T/winemac.so" -nt "$T/archive.a" ] || { echo "setup: the stale driver must be the younger file"; exit 2; }
expect_refusal "stale winemac.so younger than the archive is refused" \
    "linked against Swift build $OLD, the archive is $NEW" \
    gate_swift_build_id "$T/archive.a" "$T/winemac.so" "$NEW"

binary_with "$T/winemac-current.so" "sevo:winemacswift=$NEW"
expect_acceptance "driver linked against the archive is accepted" \
    gate_swift_build_id "$T/archive.a" "$T/winemac-current.so" "$NEW"

expect_refusal "archive older than its sources is refused" \
    "the sources are now $OLD" \
    gate_swift_build_id "$T/archive.a" "$T/winemac-current.so" "$OLD"

binary_with "$T/unstamped.so" "no-stamp-here"
expect_refusal "driver without a build id is refused" \
    "carries no sevo:winemacswift stamp" \
    gate_swift_build_id "$T/archive.a" "$T/unstamped.so" "$NEW"

{ binary_with "$T/two-ids.so" "sevo:winemacswift=$NEW"; binary_with "$T/second" "sevo:winemacswift=$OLD"; cat "$T/second" >> "$T/two-ids.so"; }
expect_refusal "driver carrying two build ids is refused" \
    "2 different sevo:winemacswift stamps" \
    gate_swift_build_id "$T/archive.a" "$T/two-ids.so" "$NEW"

# --- the server and ntdll ---
printf '#define SERVER_PROTOCOL_VERSION 963\n' > "$T/server_protocol.h"
binary_with "$T/wineserver" "sevo:server-protocol=963"
binary_with "$T/ntdll.so" "sevo:server-protocol=963"
expect_acceptance "server and ntdll at the source's protocol are accepted" \
    gate_server_pairing "$T/wineserver" "$T/ntdll.so" "$T/server_protocol.h"

binary_with "$T/wineserver-old" "sevo:server-protocol=962"
expect_refusal "server behind ntdll is refused" \
    "speaks server protocol 962, $T/ntdll.so speaks 963" \
    gate_server_pairing "$T/wineserver-old" "$T/ntdll.so" "$T/server_protocol.h"

printf '#define SERVER_PROTOCOL_VERSION 964\n' > "$T/server_protocol-bumped.h"
expect_refusal "pair behind the source is refused" \
    "the source at HEAD defines 964" \
    gate_server_pairing "$T/wineserver" "$T/ntdll.so" "$T/server_protocol-bumped.h"

binary_with "$T/wineserver-unstamped" "wineserver without a stamp"
expect_refusal "server without a stamp is refused" \
    "carries no sevo:server-protocol stamp" \
    gate_server_pairing "$T/wineserver-unstamped" "$T/ntdll.so" "$T/server_protocol.h"

# --- the tree ---
git -C "$T" init -q repo
git -C "$T/repo" -c user.name=test -c user.email=test@example.invalid -c commit.gpgsign=false \
    commit -q --allow-empty -m "empty"
echo tracked > "$T/repo/file"
git -C "$T/repo" add file
git -C "$T/repo" -c user.name=test -c user.email=test@example.invalid -c commit.gpgsign=false commit -q -m "file"
expect_acceptance "clean tree is accepted" gate_clean_tree "$T/repo" 0
echo changed > "$T/repo/file"
expect_refusal "dirty tree is refused" "uncommitted changes" gate_clean_tree "$T/repo" 0
expect_acceptance "dirty tree is accepted with --allow-dirty" gate_clean_tree "$T/repo" 1
echo untracked > "$T/repo/scratch"
git -C "$T/repo" checkout -q -- file
expect_acceptance "an untracked file alone does not make the tree dirty" gate_clean_tree "$T/repo" 0

# --- the macOS floor: a file built for a newer macOS than macos-min ---
printf 'int main(void) { return 0; }\n' > "$T/floor.c"
mkdir -p "$T/floor-ok/lib" "$T/floor-new/lib" "$T/floor-mixed/lib"
clang -arch arm64 -mmacosx-version-min=26.0 -o "$T/floor-ok/lib/old-enough" "$T/floor.c"
clang -arch arm64 -arch x86_64 -mmacosx-version-min=15.0 -o "$T/floor-ok/lib/universal" "$T/floor.c"
printf 'MZ not a Mach-O\n' > "$T/floor-ok/lib/module.dll"
expect_acceptance "binaries at or below the floor are accepted" gate_macos_floor "$T/floor-ok" 26.0
clang -arch arm64 -mmacosx-version-min=27.0 -o "$T/floor-new/lib/too-new" "$T/floor.c"
expect_refusal "a binary built for a newer macOS is refused" "lib/too-new needs macOS 27.0" gate_macos_floor "$T/floor-new" 26.0
clang -arch x86_64 -mmacosx-version-min=26.0 -o "$T/x" "$T/floor.c"
clang -arch arm64 -mmacosx-version-min=27.0 -o "$T/y" "$T/floor.c"
lipo -create -output "$T/floor-mixed/lib/mixed" "$T/x" "$T/y"
expect_refusal "a universal binary with one slice too new is refused" "lib/mixed needs macOS 27.0" gate_macos_floor "$T/floor-mixed" 26.0

# --- the release gate: a script given the engine directory ---
mkdir -p "$T/staged engine"
printf '#!/bin/bash\n[ -d "$1" ] && [ $# = 1 ] && echo "probed $1"\n' > "$T/gate-pass.sh"
printf '#!/bin/bash\necho "FAIL  rpc-listeners  HUNG at 64 of 70"\nexit 1\n' > "$T/gate-fail.sh"
cp "$T/gate-pass.sh" "$T/gate-plain.sh"
chmod +x "$T/gate-pass.sh" "$T/gate-fail.sh"
expect_acceptance "a gate that exits 0 lets the engine through" gate_release "$T/gate-pass.sh" "$T/staged engine"
case "$(gate_release "$T/gate-pass.sh" "$T/staged engine")" in
    "probed $T/staged engine") pass "the gate is given the engine directory as its one argument" ;;
    *) fail "the gate is given the engine directory as its one argument" ;;
esac
expect_refusal "a gate that exits 1 refuses the engine" "refused $T/staged engine (exit 1)" gate_release "$T/gate-fail.sh" "$T/staged engine"
expect_refusal "a refusing gate's own lines are shown" "HUNG at 64 of 70" gate_release "$T/gate-fail.sh" "$T/staged engine"
expect_refusal "a gate that is not executable is refused" "is not an executable file" gate_release "$T/gate-plain.sh" "$T/staged engine"
expect_refusal "a gate that does not exist is refused" "is not an executable file" gate_release "$T/no-such-gate.sh" "$T/staged engine"

# --- Apple's toolkit, by what a file carries and never by its name ---
mkdir -p "$T/clean/wine/lib/wine/x86_64-windows" "$T/clean/wine/lib/wine/x86_64-unix" "$T/apple/wine/lib/wine/x86_64-windows" "$T/bridge/wine/lib"
binary_with "$T/clean/wine/lib/wine/x86_64-windows/d3d11.dll" "wine's own d3d11, which says D3DMetal in a message"
ln -s ../../external/libd3dshared.dylib "$T/clean/wine/lib/wine/x86_64-unix/d3d11.so"
expect_acceptance "an engine with none of the toolkit is accepted, its bridge links included" gate_no_apple_toolkit "$T/clean"
binary_with "$T/apple/wine/lib/wine/x86_64-windows/nvngx.dll" "/Library/Caches/com.apple.xbs/Sources/D3DMetalDLLsBase/D3D4Mac/nvngx/nvngx.c"
expect_refusal "a toolkit DLL under another name is refused" "wine/lib/wine/x86_64-windows/nvngx.dll" gate_no_apple_toolkit "$T/apple"
binary_with "$T/bridge/wine/lib/libsomething.dylib" "D3DMetalWineThread"
expect_refusal "the toolkit's bridge under another name is refused" "wine/lib/libsomething.dylib" gate_no_apple_toolkit "$T/bridge"

# --- the staged copy: what the app does to an engine is undone, and the rest is held to the manifest ---
manifest_of() {
    python3 - "$1" <<'PY'
import hashlib, json, os, sys
root = sys.argv[1]
components = []
for directory, _, names in os.walk(root):
    for name in sorted(names):
        path = os.path.join(directory, name)
        entry = {"path": os.path.relpath(path, root), "from": "build"}
        if os.path.islink(path):
            entry["link"] = os.readlink(path)
        else:
            entry["sha256"] = hashlib.sha256(open(path, "rb").read()).hexdigest()
        components.append(entry)
json.dump({"components": components}, open(os.path.join(root, "engine-manifest.json"), "w"))
PY
}
E="$T/engine"
mkdir -p "$E/wine/lib/wine/x86_64-windows" "$E/wine/lib/wine/x86_64-unix" "$E/wine/lib/external" "$E/dxmt"
binary_with "$E/wine/lib/wine/x86_64-windows/d3d11.dll" "wine's own d3d11"
binary_with "$E/wine/lib/wine/x86_64-windows/kernel32.dll" "wine's own kernel32"
binary_with "$E/dxmt/d3d11.dll" "DXMT's d3d11"
binary_with "$E/dxmt/winemetal.dll" "DXMT's winemetal"
ln -s ../../external/libd3dshared.dylib "$E/wine/lib/wine/x86_64-unix/d3d11.so"
manifest_of "$E"
expect_acceptance "an engine as packaged matches its manifest" gate_matches_manifest "$E"

# As the app leaves it with D3DMetal staged: the toolkit placed, its DLLs over
# wine's with the builtin set aside, the stubs under their export names with
# bridge links, and the provenance file.
cp -R "$E" "$T/ran-d3dmetal"
R="$T/ran-d3dmetal"
mkdir -p "$R/wine/lib/wine/x86_64-windows-original" "$R/wine/lib/external/D3DMetal.framework"
cp "$R/wine/lib/wine/x86_64-windows/d3d11.dll" "$R/wine/lib/wine/x86_64-windows-original/d3d11.dll"
binary_with "$R/wine/lib/wine/x86_64-windows/d3d11.dll" "/Library/Caches/com.apple.xbs/Sources/D3DMetalDLLsBase/D3D4Mac/d3d11/d3d11.c"
binary_with "$R/wine/lib/wine/x86_64-windows/nvapi.dll" "/Library/Caches/com.apple.xbs/Sources/D3DMetalDLLsBase/D3D4Mac/nvapi/nvapi.c"
ln -s ../../external/libd3dshared.dylib "$R/wine/lib/wine/x86_64-unix/nvapi.so"
binary_with "$R/wine/lib/external/libd3dshared.dylib" "D3DMetalWineThread"
binary_with "$R/wine/lib/external/D3DMetal.framework/D3DMetal" "/Library/Caches/com.apple.xbs/Binaries/D3DMetal"
printf 'renderer=d3dmetal\n' > "$R/renderer-hashes"
expect_refusal "an engine the app ran D3DMetal on does not match its manifest" "not the file that was packaged" gate_matches_manifest "$R"
expect_refusal "and carries the toolkit" "wine/lib/wine/x86_64-windows/nvapi.dll" gate_no_apple_toolkit "$R"
stage_as_packaged "$R" > "$T/staging.log"
expect_acceptance "undone, it matches its manifest" gate_matches_manifest "$R"
expect_acceptance "and carries none of the toolkit" gate_no_apple_toolkit "$R"
[ -d "$R/wine/lib/external" ] && [ -z "$(ls -A "$R/wine/lib/external")" ] && pass "wine/lib/external stays, empty" || fail "wine/lib/external stays, empty"

# With DXMT staged: its DLLs over wine's, and winemetal.dll with nothing set aside.
cp -R "$E" "$T/ran-dxmt"
R="$T/ran-dxmt"
mkdir -p "$R/wine/lib/wine/x86_64-windows-original"
cp "$R/wine/lib/wine/x86_64-windows/d3d11.dll" "$R/wine/lib/wine/x86_64-windows-original/d3d11.dll"
cp "$R/dxmt/d3d11.dll" "$R/dxmt/winemetal.dll" "$R/wine/lib/wine/x86_64-windows/"
stage_as_packaged "$R" > "$T/staging.log"
expect_acceptance "an engine the app ran DXMT on, undone, matches its manifest" gate_matches_manifest "$R"

# What nobody can account for stays, and is refused by name.
cp -R "$E" "$T/stranger"
binary_with "$T/stranger/wine/lib/wine/x86_64-windows/stranger.dll" "nobody's"
stage_as_packaged "$T/stranger" > "$T/staging.log"
expect_refusal "a file the manifest does not list is refused" "wine/lib/wine/x86_64-windows/stranger.dll: not in the manifest" gate_matches_manifest "$T/stranger"
cp -R "$E" "$T/changed"
binary_with "$T/changed/wine/lib/wine/x86_64-windows/kernel32.dll" "another kernel32"
expect_refusal "a changed file is refused" "kernel32.dll: not the file that was packaged" gate_matches_manifest "$T/changed"
cp -R "$E" "$T/short"
mv "$T/short/dxmt/d3d11.dll" "$T/short-d3d11.dll"
expect_refusal "a missing file is refused" "dxmt/d3d11.dll: missing" gate_matches_manifest "$T/short"
mkdir "$T/no-manifest"
expect_refusal "an engine with no manifest is refused" "no readable engine-manifest.json" gate_matches_manifest "$T/no-manifest"

[ "$FAILED" = 0 ] && echo "every gate behaved" || exit 1
