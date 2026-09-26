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

[ "$FAILED" = 0 ] && echo "every gate behaved" || exit 1
