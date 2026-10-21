# shellcheck shell=bash
# The content checks package-engine.sh runs before it assembles an engine,
# as functions over file paths so tests/packaging-gates.sh can run them
# against files it makes up. Each gate prints its refusal and returns 1, or
# prints nothing and returns 0.
#
# Every check reads what the binaries carry, never when they were written:
# wine's installer writes a fresh destination file, so a stale winemac.so is
# younger than the archive it was not linked against.

# The distinct values of `<prefix>=<value>` a file carries, one per line.
# `strings -a` scans the whole file: a Swift literal sits in __TEXT,__cstring,
# and a static archive is scanned member by member.
stamp_values() {
    strings -a -n 8 "$1" 2>/dev/null | grep -o "$2=[A-Za-z0-9._-]*" | cut -d= -f2- | sort -u
}

# One value, or a refusal when the file carries none or several.
stamp_of() {
    local file="$1" prefix="$2" values count
    values="$(stamp_values "$file" "$prefix")"
    count="$(printf '%s' "$values" | grep -c .)"
    case "$count" in
        1) printf '%s\n' "$values"; return 0 ;;
        0) echo "$file carries no $prefix stamp"; return 1 ;;
        *) echo "$file carries $count different $prefix stamps: $(printf '%s' "$values" | tr '\n' ' ')"; return 1 ;;
    esac
}

# The Swift archive is current with its sources, and the staged winemac.so was
# linked against it: all three carry the same `sevo:winemacswift` build id.
#   gate_swift_build_id <archive> <staged winemac.so> <id the sources give now>
gate_swift_build_id() {
    local archive="$1" driver="$2" current="$3" archive_id driver_id
    archive_id="$(stamp_of "$archive" sevo:winemacswift)" || { echo "$archive_id — run 'make -C dlls/winemac.drv/swift', then rebuild wine and re-run install-lib"; return 1; }
    [ "$archive_id" = "$current" ] || {
        echo "$archive was built from Swift sources with id $archive_id; the sources are now $current — run 'make -C dlls/winemac.drv/swift', rebuild wine and re-run install-lib"
        return 1
    }
    driver_id="$(stamp_of "$driver" sevo:winemacswift)" || { echo "$driver_id — rebuild wine against the archive and re-run install-lib"; return 1; }
    [ "$archive_id" = "$driver_id" ] || {
        echo "$driver was linked against Swift build $driver_id, the archive is $archive_id — rebuild wine and re-run install-lib"
        return 1
    }
}

# The server and ntdll.so speak the protocol the source at HEAD defines: both
# carry `sevo:server-protocol=<N>` and N is SERVER_PROTOCOL_VERSION in
# include/wine/server_protocol.h.
#   gate_server_pairing <wineserver> <ntdll.so> <server_protocol.h>
gate_server_pairing() {
    local server="$1" ntdll="$2" header="$3" source_version server_version ntdll_version
    source_version="$(sed -n 's/^#define SERVER_PROTOCOL_VERSION \([0-9][0-9]*\)$/\1/p' "$header")"
    [ -n "$source_version" ] || { echo "$header defines no SERVER_PROTOCOL_VERSION"; return 1; }
    server_version="$(stamp_of "$server" sevo:server-protocol)" || { echo "$server_version — rebuild the server from a source that carries the stamp (server/main.c)"; return 1; }
    ntdll_version="$(stamp_of "$ntdll" sevo:server-protocol)" || { echo "$ntdll_version — rebuild wine from a source that carries the stamp (dlls/ntdll/unix/server.c) and re-run install-lib"; return 1; }
    [ "$server_version" = "$ntdll_version" ] || {
        echo "$server speaks server protocol $server_version, $ntdll speaks $ntdll_version — rebuild whichever is older from the same source"
        return 1
    }
    [ "$server_version" = "$source_version" ] || {
        echo "$server and $ntdll speak server protocol $server_version, the source at HEAD defines $source_version — rebuild both"
        return 1
    }
}

# The repository has no uncommitted change to a tracked file, unless the
# caller allows one.
#   gate_clean_tree <repo> <1 to allow a dirty tree, 0 to refuse it>
gate_clean_tree() {
    local repo="$1" allow="$2" dirty
    dirty="$(git -C "$repo" status --porcelain --untracked-files=no)"
    [ -z "$dirty" ] && return 0
    [ "$allow" = 1 ] && return 0
    echo "the tree has uncommitted changes (pass --allow-dirty to package it anyway; the manifest will say so):"
    printf '%s\n' "$dirty" | sed 's/^/    /'
    return 1
}

# Every Mach-O under a directory runs on the engine's floor: each slice's
# LC_BUILD_VERSION minos (or LC_VERSION_MIN_MACOSX) is at most the version in
# macos-min. clang stamps the build Mac's own version when no deployment target
# is passed, and dyld then refuses the file on any older macOS.
#   gate_macos_floor <directory> <floor, e.g. 26.0>
gate_macos_floor() {
    python3 - "$1" "$2" <<'PY'
import os, struct, sys

root = sys.argv[1]
floor = tuple((list(map(int, sys.argv[2].split("."))) + [0, 0])[:3])
LC_BUILD_VERSION, LC_VERSION_MIN_MACOSX, PLATFORM_MACOS = 0x32, 0x24, 1

def version(packed):
    return (packed >> 16, (packed >> 8) & 0xFF, packed & 0xFF)

def slice_minimums(data, offset):
    magic = struct.unpack_from("<I", data, offset)[0]
    if magic not in (0xFEEDFACF, 0xFEEDFACE):
        return []
    ncmds = struct.unpack_from("<I", data, offset + 16)[0]
    cursor = offset + (32 if magic == 0xFEEDFACF else 28)
    found = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, cursor)
        if cmd == LC_BUILD_VERSION and struct.unpack_from("<I", data, cursor + 8)[0] == PLATFORM_MACOS:
            found.append(version(struct.unpack_from("<I", data, cursor + 12)[0]))
        elif cmd == LC_VERSION_MIN_MACOSX:
            found.append(version(struct.unpack_from("<I", data, cursor + 8)[0]))
        cursor += size
    return found

def minimums(path):
    with open(path, "rb") as f:
        head = f.read(4)
        if head not in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe"):
            return []
        data = head + f.read()
    if head == b"\xca\xfe\xba\xbe":
        count = struct.unpack_from(">I", data, 4)[0]
        if count > 16:
            return []  # a Java class file shares the fat magic
        offsets = [struct.unpack_from(">I", data, 8 + 20 * i + 8)[0] for i in range(count)]
        return [m for o in offsets for m in slice_minimums(data, o)]
    return slice_minimums(data, 0)

refused = []
for directory, _, files in os.walk(root):
    for name in files:
        path = os.path.join(directory, name)
        if os.path.islink(path):
            continue
        newest = max(minimums(path), default=None)
        if newest and newest > floor:
            shown = ".".join(map(str, newest if newest[2] else newest[:2]))
            refused.append(f"    {os.path.relpath(path, root)} needs macOS {shown}")
if refused:
    print(f"{len(refused)} file(s) need a newer macOS than the engine's floor {sys.argv[2]} (build-macos/macos-min):")
    print("\n".join(sorted(refused)))
    sys.exit(1)
PY
}
