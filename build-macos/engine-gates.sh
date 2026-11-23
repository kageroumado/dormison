# shellcheck shell=bash
# The content checks package-engine.sh runs before it assembles an engine,
# and the release gate publish-engine.sh runs on the staged copy, as functions
# over file paths so tests/packaging-gates.sh can run them against files it
# makes up. Each gate prints its refusal and returns 1, or prints nothing and
# returns 0; gate_release also lets through what its script prints.
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

# The release gate: a script that takes an engine directory and exits 0 when
# the engine may ship. It runs with the directory as its one argument, and its
# output is the caller's.
#   gate_release <script> <engine directory>
gate_release() {
    local script="$1" engine="$2" status=0
    [ -f "$script" ] && [ -x "$script" ] || { echo "the gate $script is not an executable file"; return 1; }
    "$script" "$engine" || status=$?
    [ "$status" = 0 ] || { echo "the gate $script refused $engine (exit $status)"; return 1; }
}

# An expanded engine-deps tarball holds exactly the files its manifest lists,
# each with the sha256 or the link target recorded for it.
#   gate_deps_tree <the expanded engine-deps directory>
gate_deps_tree() {
    python3 - "$1" <<'PY'
import hashlib, json, os, sys

tree = sys.argv[1]
try:
    with open(os.path.join(tree, "engine-deps.json")) as f:
        listed = {entry["path"]: entry for entry in json.load(f)["files"]}
except (OSError, ValueError, KeyError) as error:
    sys.exit(f"{tree} has no readable engine-deps.json: {error}")

refused = []
found = set()
for root, _, names in os.walk(tree):
    for name in names:
        full = os.path.join(root, name)
        path = os.path.relpath(full, tree)
        if path == "engine-deps.json":
            continue
        found.add(path)
        entry = listed.get(path)
        if entry is None:
            refused.append(f"    {path} is not in the manifest")
        elif os.path.islink(full):
            if entry.get("link") != os.readlink(full):
                refused.append(f"    {path} links to {os.readlink(full)}, the manifest says {entry.get('link')}")
        else:
            digest = hashlib.sha256()
            with open(full, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    digest.update(chunk)
            if entry.get("sha256") != digest.hexdigest():
                refused.append(f"    {path} is not the file the manifest lists")
refused += [f"    {path} is missing" for path in sorted(set(listed) - found)]
if refused:
    print(f"{tree} does not match its engine-deps.json ({len(refused)} file(s)): trash it and expand the tarball again")
    print("\n".join(sorted(refused)[:20]))
    sys.exit(1)
PY
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

# No file under a directory is part of Apple's Game Porting Toolkit, whatever
# it is named: the toolkit may not be redistributed, and the app places the
# user's own copy into an engine it runs. Read from what the files carry: every
# binary Apple built names its build root, /Library/Caches/com.apple.xbs/, and
# the toolkit's bridge, libd3dshared.dylib, names its thread D3DMetalWineThread.
# Links are skipped: the engine's D3DMetal unix halves are links to a bridge
# the engine does not hold.
#   gate_no_apple_toolkit <directory>
gate_no_apple_toolkit() {
    python3 - "$1" <<'PY'
import os, sys

root = sys.argv[1]
MARKERS = (b"/Library/Caches/com.apple.xbs/", b"D3DMetalWineThread")
found = []
for directory, _, names in os.walk(root):
    for name in names:
        path = os.path.join(directory, name)
        if os.path.islink(path):
            continue
        with open(path, "rb") as f:
            data = f.read()
        if any(marker in data for marker in MARKERS):
            found.append("    " + os.path.relpath(path, root))
if found:
    print(f"{len(found)} file(s) under {root} are Apple's Game Porting Toolkit, which may not be redistributed:")
    print("\n".join(sorted(found)))
    sys.exit(1)
PY
}

# Undo what the app does to an engine it runs, in a copy of one: the provenance
# file it writes at renderer staging, the toolkit it places in wine/lib/external,
# the builtins it set aside in <tree>-original (put back), and the renderer DLLs
# and bridge links it staged with nothing to set aside. Only a file
# engine-manifest.json does not list is removed, and only when it is one of
# those; each is named as it goes. What is left is for gate_matches_manifest.
#   stage_as_packaged <copy of an engine directory>
stage_as_packaged() {
    python3 - "$1" <<'PY'
import filecmp, json, os, shutil, sys

root = sys.argv[1]
try:
    with open(os.path.join(root, "engine-manifest.json")) as f:
        listed = {entry["path"] for entry in json.load(f)["components"]}
except (OSError, ValueError, KeyError) as error:
    sys.exit(f"{root} has no readable engine-manifest.json ({error}): package the engine again")

MARKERS = (b"/Library/Caches/com.apple.xbs/", b"D3DMetalWineThread")
BRIDGE = "../../external/libd3dshared.dylib"
modules = os.path.join(root, "wine/lib/wine")

def drop(path, why):
    print(f"    removed {os.path.relpath(path, root)}: {why}")
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    else:
        os.remove(path)

# The builtins a staging set aside go back over the renderer's.
for name in sorted(os.listdir(modules)) if os.path.isdir(modules) else []:
    originals = os.path.join(modules, name)
    if not name.endswith("-original") or not os.path.isdir(originals):
        continue
    tree = originals[: -len("-original")]
    for dll in sorted(os.listdir(originals)):
        shutil.copy2(os.path.join(originals, dll), os.path.join(tree, dll))
        print(f"    restored {os.path.relpath(os.path.join(tree, dll), root)}")
    shutil.rmtree(originals)

def is_apple(path):
    with open(path, "rb") as f:
        data = f.read()
    return any(marker in data for marker in MARKERS)

def is_staged_renderer(path):
    """A module-tree file that is a renderer bundle's own DLL of the same name."""
    relative = os.path.relpath(path, modules).split(os.sep)
    if len(relative) != 2:
        return False
    tree, name = relative
    bundles = ["dxmt/i386-windows"] if tree == "i386-windows" else ["dxmt", "dxvk"]
    for bundle in bundles:
        candidate = os.path.join(root, bundle, name)
        if os.path.isfile(candidate) and filecmp.cmp(candidate, path, shallow=False):
            return True
    return False

for directory, names, files in os.walk(root, topdown=True):
    for name in list(names):
        path = os.path.join(directory, name)
        relative = os.path.relpath(path, root)
        if relative == "d3dmetal" or os.path.dirname(relative) == "wine/lib/external":
            drop(path, "the toolkit the app placed")
            names.remove(name)
    for name in files:
        path = os.path.join(directory, name)
        relative = os.path.relpath(path, root)
        if relative in listed or relative == "engine-manifest.json":
            continue
        if relative == "renderer-hashes":
            drop(path, "the app's record of its renderer staging")
        elif os.path.dirname(relative) == "wine/lib/external":
            drop(path, "the toolkit the app placed")
        elif os.path.islink(path):
            if os.readlink(path) == BRIDGE:
                drop(path, "a bridge link the app made for a staged toolkit DLL")
        elif is_apple(path):
            drop(path, "Apple's toolkit, staged by the app")
        elif is_staged_renderer(path):
            drop(path, "a renderer DLL the app staged from the engine's own bundle")
PY
}

# A directory holds exactly the engine package-engine.sh assembled: every file
# engine-manifest.json lists, with the sha256 or the link target recorded for
# it, and no other.
#   gate_matches_manifest <engine directory>
gate_matches_manifest() {
    python3 - "$1" <<'PY'
import hashlib, json, os, sys

root = sys.argv[1]
try:
    with open(os.path.join(root, "engine-manifest.json")) as f:
        listed = {entry["path"]: entry for entry in json.load(f)["components"]}
except (OSError, ValueError, KeyError) as error:
    sys.exit(f"{root} has no readable engine-manifest.json ({error}): package the engine again")

refused = []
found = set()
for directory, _, names in os.walk(root):
    for name in names:
        path = os.path.join(directory, name)
        relative = os.path.relpath(path, root)
        if relative == "engine-manifest.json":
            continue
        found.add(relative)
        entry = listed.get(relative)
        if entry is None:
            refused.append(f"    {relative}: not in the manifest")
        elif os.path.islink(path):
            if entry.get("link") != os.readlink(path):
                refused.append(f"    {relative}: links to {os.readlink(path)}, packaged as {entry.get('link', 'a file')}")
        elif "sha256" not in entry:
            refused.append(f"    {relative}: a file, packaged as a link to {entry.get('link')}")
        else:
            digest = hashlib.sha256()
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    digest.update(chunk)
            if digest.hexdigest() != entry["sha256"]:
                refused.append(f"    {relative}: not the file that was packaged")
refused += [f"    {relative}: missing" for relative in sorted(set(listed) - found)]
if refused:
    print(f"{root} is not the engine engine-manifest.json describes ({len(refused)} file(s)):")
    print("\n".join(sorted(refused)[:40]))
    sys.exit(1)
PY
}
