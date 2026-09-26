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
