#!/bin/bash
# Pack the payloads package-engine.sh takes from a donor engine as one tarball,
# engine-deps-<first 16 hex digits of its sha256>.tar.xz, so an engine can be
# packaged from this repository, the build tree and that one file:
# `package-engine.sh <version> --deps <tarball>`.
#
# usage: pack-engine-deps.sh <donor engine directory> <output directory>
#
# The tarball holds one directory, engine-deps/, laid out as the engine is:
#
#   wine/lib/*.dylib                     the dependency libraries, links included
#   wine/share/wine/gecko, mono          wine-gecko and wine-mono
#   wine/lib/wine/x86_64-unix/vulkan     the ICD manifest that names libMoltenVK
#   wine/lib/wine/x86_64-unix/winemetal.so
#                                        the unix half of DXMT's Metal back end
#   dxmt/, dxvk/                         the renderer bundles, DXMT's 32-bit half included
#   engine-deps.json                     every file with its sha256 and size, the donor,
#                                        the upstream sources the donor's engine-info.json names
#
# Nothing comes from the donor's module trees or from wine/lib/external, where
# the app puts the user's own copy of Apple's Game Porting Toolkit, and the
# payloads are read for the toolkit by content before they are packed.
#
# The tar stream is the same bytes for the same payloads (names sorted, no
# times or extended attributes, every entry root:wheel), and its sha256 is printed; the
# compressed file's bytes also depend on the xz that wrote them. The donor is
# only read.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=engine-gates.sh
. "$HERE/engine-gates.sh"

DONOR="${1:?usage: pack-engine-deps.sh <donor engine directory> <output directory>}"
OUTPUT="${2:?usage: pack-engine-deps.sh <donor engine directory> <output directory>}"
[ -d "$DONOR/wine/lib" ] || { echo "no engine at $DONOR"; exit 1; }
[ -f "$DONOR/engine-info.json" ] || { echo "$DONOR has no engine-info.json"; exit 1; }
mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd)"
WORK="$(mktemp -d "$OUTPUT/engine-deps.XXXXXX")"
TREE="$WORK/engine-deps"

echo "==> collecting from $DONOR"
mkdir -p "$TREE/wine/lib/wine/x86_64-unix" "$TREE/wine/share/wine"
find "$DONOR/wine/lib" -maxdepth 1 -name '*.dylib' -exec cp -a {} "$TREE/wine/lib/" \;
for d in gecko mono; do
    [ -d "$DONOR/wine/share/wine/$d" ] && cp -R "$DONOR/wine/share/wine/$d" "$TREE/wine/share/wine/$d"
done
cp -R "$DONOR/wine/lib/wine/x86_64-unix/vulkan" "$TREE/wine/lib/wine/x86_64-unix/vulkan"
cp "$DONOR/wine/lib/wine/x86_64-unix/winemetal.so" "$TREE/wine/lib/wine/x86_64-unix/"
for d in dxmt dxvk; do
    [ -d "$DONOR/$d" ] && cp -R "$DONOR/$d" "$TREE/$d"
done

# --- Apple's toolkit never ships, under any name ---
gate_no_apple_toolkit "$TREE" || { echo "refusing to pack"; trash "$WORK" 2>/dev/null || true; exit 1; }

echo "==> writing engine-deps.json and the tar stream"
python3 - "$TREE" "$DONOR" "$WORK/engine-deps.tar" <<'PY'
import hashlib, json, os, sys, tarfile

tree, donor, tar_path = sys.argv[1:4]

def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()

with open(os.path.join(donor, "engine-info.json")) as f:
    info = json.load(f)

files = []
for root, dirs, names in os.walk(tree):
    dirs.sort()
    for name in sorted(names):
        full = os.path.join(root, name)
        entry = {"path": os.path.relpath(full, tree)}
        if os.path.islink(full):
            entry["link"] = os.readlink(full)
        else:
            entry["sha256"] = sha256(full)
            entry["size"] = os.path.getsize(full)
        files.append(entry)
files.sort(key=lambda entry: entry["path"])

def versions(directory):
    path = os.path.join(tree, "wine/share/wine", directory)
    return sorted(os.listdir(path)) if os.path.isdir(path) else []

manifest = {
    "schema": 1,
    "donor": {"version": info.get("version"), "commit": info.get("commit")},
    "sources": {
        "dxmt": info.get("dxmt"),
        "dxvk": info.get("dxvk"),
        "gecko": versions("gecko"),
        "mono": versions("mono"),
        "wine/lib": "the donor's dependency libraries, as it carried them",
    },
    "bytes": sum(entry.get("size", 0) for entry in files),
    "files": files,
}
with open(os.path.join(tree, "engine-deps.json"), "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")

def plain(member):
    member.uid = member.gid = 0
    member.uname, member.gname = "root", "wheel"
    member.mtime = 0
    if member.isdir() or member.mode & 0o100:
        member.mode = 0o755
    else:
        member.mode = 0o644
    member.pax_headers = {}
    return member

paths = ["engine-deps.json"] + [entry["path"] for entry in files]
directories = set()
for path in paths:
    while (path := os.path.dirname(path)):
        directories.add(path)
with tarfile.open(tar_path, "w", format=tarfile.PAX_FORMAT) as tar:
    tar.add(tree, arcname="engine-deps", recursive=False, filter=plain)
    for directory in sorted(directories):
        tar.add(os.path.join(tree, directory), arcname=f"engine-deps/{directory}", recursive=False, filter=plain)
    for path in paths:
        tar.add(os.path.join(tree, path), arcname=f"engine-deps/{path}", recursive=False, filter=plain)
print(f"    {len(files)} files, {manifest['bytes']:,} bytes")
print(f"    tar stream sha256 {sha256(tar_path)}")
PY

gate_names_nobody "$TREE" || { echo "refusing to pack"; trash "$WORK" 2>/dev/null || true; exit 1; }

echo "==> compressing"
xz -T0 -9 "$WORK/engine-deps.tar"
gate_names_nobody "$WORK/engine-deps.tar.xz" || { trash "$WORK" 2>/dev/null || true; exit 1; }
SHA256="$(shasum -a 256 "$WORK/engine-deps.tar.xz" | cut -d' ' -f1)"
NAME="engine-deps-$(printf '%s' "$SHA256" | cut -c1-16).tar.xz"
[ -e "$OUTPUT/$NAME" ] && { echo "$OUTPUT/$NAME already exists — the payloads are packed"; trash "$WORK" 2>/dev/null || true; exit 0; }
mv "$WORK/engine-deps.tar.xz" "$OUTPUT/$NAME"
cp "$TREE/engine-deps.json" "$OUTPUT/${NAME%.tar.xz}.json"
echo "$SHA256  $NAME" > "$OUTPUT/$NAME.sha256"
trash "$WORK" 2>/dev/null || true
echo "==> $OUTPUT/$NAME"
echo "    $(stat -f %z "$OUTPUT/$NAME") bytes, sha256 $SHA256"
