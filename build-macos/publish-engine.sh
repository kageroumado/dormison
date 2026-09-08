#!/bin/bash
# Publish one engine release: stage the engine directory package-engine.sh
# assembled, pack it, sign the tarball and the manifest with the engine key,
# tag r<N> here, create GitHub release r<N> of this repository (tarball,
# .sha256, .sig, engine-info.json, the diff against wine-staging-base), then
# name the release in the manifest the app reads and upload the manifest
# with its signature.
#
# usage: publish-engine.sh r<N> [--channel stable|beta] [--dry-run]
#                          [--engine <dir>] [--key <ed25519.pem>]
#                          [--identity "<Developer ID Application: …>"]
#                          [--notes-file <file>] [--min-app-version <x.y>]
#
# The engine key is required (--key, or DORMISON_ED25519_KEY): every release
# carries a signature the app verifies against the key pinned in
# EngineSignature.swift, so an unsigned release is one no app will install.
# --identity re-signs every Mach-O in the staged copy with a Developer ID
# certificate and a secure timestamp instead of the ad-hoc signature
# package-engine.sh leaves; the installed engine is untouched either way.
# --dry-run does everything up to the tag and the uploads.
#
# Needs `gh` signed in to an account that can write both repositories,
# `openssl` 3 (Ed25519), and `sevo` (SEVO, PATH, or the installed app's
# Helpers) for the manifest gate.
set -euo pipefail

VERSION="${1:?usage: publish-engine.sh r<N> [--channel stable|beta] [--dry-run] [--engine <dir>] [--key <pem>] [--identity <id>] [--notes-file <file>]}"
shift
CHANNEL=stable
DRY_RUN=0
ENGINE_DIR=""
KEY="${DORMISON_ED25519_KEY:-}"
IDENTITY="${DORMISON_SIGNING_IDENTITY:-}"
NOTES_FILE=""
MIN_APP_VERSION="${SEVO_MIN_APP_VERSION:-1.0}"
while [ $# -gt 0 ]; do
    case "$1" in
        --channel) CHANNEL="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        --engine) ENGINE_DIR="$2"; shift 2 ;;
        --key) KEY="$2"; shift 2 ;;
        --identity) IDENTITY="$2"; shift 2 ;;
        --notes-file) NOTES_FILE="$2"; shift 2 ;;
        --min-app-version) MIN_APP_VERSION="$2"; shift 2 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done

[[ "$VERSION" =~ ^r[0-9]+$ ]] || { echo "version must be r<N>, got $VERSION"; exit 2; }
[[ "$CHANNEL" =~ ^(stable|beta)$ ]] || { echo "channel must be stable or beta"; exit 2; }
[ -n "$KEY" ] || { echo "no engine key: pass --key <ed25519.pem> or set DORMISON_ED25519_KEY"; exit 2; }
[ -f "$KEY" ] || { echo "engine key not found at $KEY"; exit 2; }
[ -z "$NOTES_FILE" ] || [ -f "$NOTES_FILE" ] || { echo "notes file not found at $NOTES_FILE"; exit 2; }

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
ROOT="${DORMISON_BUILD:-$HOME/Developer/build/dormison}"
ENGINES="$HOME/Library/Application Support/Sevoflurane/Engines"
NAME="dormison-$VERSION"
[ -n "$ENGINE_DIR" ] || ENGINE_DIR="$ENGINES/$NAME"
RELEASES="${DORMISON_RELEASES:-$ROOT/releases}"
STAGE="$RELEASES/stage/$NAME"
TARBALL="$RELEASES/$NAME.tar.xz"
ENGINE_REPO=kageroumado/dormison
APP_REPO=kageroumado/sevoflurane
ENGINE_REPO_URL="https://github.com/$ENGINE_REPO"

# --- the tool the manifest gate runs: this build's decoder and pinned key ---
SEVO="${SEVO:-}"
[ -n "$SEVO" ] || SEVO="$(command -v sevo || true)"
[ -n "$SEVO" ] || [ ! -x /Applications/Sevoflurane.app/Contents/Helpers/sevo ] || SEVO=/Applications/Sevoflurane.app/Contents/Helpers/sevo
[ -n "$SEVO" ] && [ -x "$SEVO" ] || { echo "sevo not found (SEVO=, PATH, or the installed app) — the manifest gate needs it"; exit 1; }

# --- preconditions: the engine, the tree, the tag, the remote ---
[ -d "$ENGINE_DIR" ] || { echo "no engine at $ENGINE_DIR — run package-engine.sh $NAME first"; exit 1; }
[ -f "$ENGINE_DIR/engine-info.json" ] || { echo "$ENGINE_DIR has no engine-info.json"; exit 1; }
[ -z "$(git -C "$REPO" status --porcelain)" ] || { echo "the tree has uncommitted changes"; exit 1; }
git -C "$REPO" rev-parse -q --verify "refs/tags/$VERSION" >/dev/null && { echo "tag $VERSION exists locally"; exit 1; }
[ -z "$(git -C "$REPO" ls-remote --tags origin "refs/tags/$VERSION")" ] || { echo "tag $VERSION exists on origin"; exit 1; }
PACKAGED_COMMIT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["commit"])' "$ENGINE_DIR/engine-info.json")"
HEAD_COMMIT="$(git -C "$REPO" rev-parse HEAD)"
[ "$PACKAGED_COMMIT" = "$HEAD_COMMIT" ] || { echo "the engine was packaged from $PACKAGED_COMMIT, HEAD is $HEAD_COMMIT"; exit 1; }
git -C "$REPO" fetch -q origin
git -C "$REPO" merge-base --is-ancestor HEAD origin/main || { echo "HEAD is not on origin/main — push first, the tag must point at a published commit"; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "gh is not signed in"; exit 1; }
gh release view "$VERSION" --repo "$ENGINE_REPO" >/dev/null 2>&1 && { echo "release $VERSION exists on $ENGINE_REPO"; exit 1; }
[ -e "$TARBALL" ] && { echo "$TARBALL exists — refusing to overwrite"; exit 1; }
WINE_VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["wine"])' "$ENGINE_DIR/engine-info.json")"

# --- stage a copy: the installed engine stays what the app runs ---
mkdir -p "$RELEASES/stage"
echo "==> staging $ENGINE_DIR → $STAGE"
rsync -a --delete "$ENGINE_DIR/" "$STAGE/"

# --- Developer ID: every Mach-O in the staged copy, timestamped ---
is_macho() {
    local magic
    magic="$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')"
    case "$magic" in
        cffaedfe|cefaedfe|feedfacf|feedface|cafebabe|bebafeca) return 0 ;;
        *) return 1 ;;
    esac
}
if [ -n "$IDENTITY" ]; then
    echo "==> signing with $IDENTITY"
    SIGNED=0
    while IFS= read -r -d '' f; do
        is_macho "$f" || continue
        codesign --force --sign "$IDENTITY" --timestamp "$f"
        codesign --verify --strict "$f"
        SIGNED=$((SIGNED + 1))
    done < <(find "$STAGE" -type f -print0)
    echo "    $SIGNED files signed"
fi

# --- pack, hash, sign ---
mkdir -p "$RELEASES"
echo "==> packing $TARBALL"
tar -C "$RELEASES/stage" -cf - "$NAME" | xz -T0 -9 > "$TARBALL"
SHA256="$(shasum -a 256 "$TARBALL" | cut -d' ' -f1)"
SIZE="$(stat -f %z "$TARBALL")"
echo "$SHA256  $NAME.tar.xz" > "$TARBALL.sha256"
cp "$STAGE/engine-info.json" "$RELEASES/$NAME-engine-info.json"
git -C "$REPO" diff wine-staging-base > "$RELEASES/$NAME.patch"
echo "    $SIZE bytes, sha256 $SHA256"

PUBKEY="$RELEASES/stage/engine-ed25519.pub.pem"
openssl pkey -in "$KEY" -pubout -out "$PUBKEY"
sign_file() {
    local file="$1" raw
    raw="$(mktemp)"
    openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$file" -out "$raw"
    openssl pkeyutl -verify -pubin -inkey "$PUBKEY" -rawin -in "$file" -sigfile "$raw" >/dev/null
    base64 < "$raw" > "$file.sig"
    rm -f "$raw"
}
echo "==> signing $NAME.tar.xz"
sign_file "$TARBALL"

# --- the manifest: the current one, with this release in its channel ---
URL="$ENGINE_REPO_URL/releases/download/$VERSION/$NAME.tar.xz"
MANIFEST="$RELEASES/engine.json"
echo "==> manifest: $CHANNEL -> $VERSION"
gh release download engine --repo "$APP_REPO" --pattern engine.json --output "$MANIFEST" --clobber
python3 - "$MANIFEST" "$CHANNEL" "$NAME" "$URL" "$SHA256" "$SIZE" "$MIN_APP_VERSION" "$STAGE/engine-info.json" <<'PY'
import json, sys
path, channel, version, url, sha256, size, min_app, info_path = sys.argv[1:]
manifest = json.load(open(path))
info = json.load(open(info_path))
manifest["schema"] = max(manifest.get("schema", 1), 2)
manifest.setdefault("channels", {})[channel] = {
    "version": version, "minAppVersion": min_app, "url": url,
    "sha256": sha256, "sizeBytes": int(size),
    "notes": f"wine {info['wine']}; commit {info['commit'][:12]}",
}
components = manifest.setdefault("components", {})
for key in ("dxmt", "dxvk"):
    source = info.get(key)
    if not source:
        continue
    tested = components.setdefault(key, [])
    if not any(entry.get("url") == source for entry in tested):
        tested.insert(0, {"version": source.rsplit("/", 2)[1].lstrip("v"), "url": source,
                          "sha256": None, "notes": f"run with {version}"})
json.dump(manifest, open(path, "w"), indent=2)
open(path, "a").write("\n")
print(json.dumps(manifest["channels"][channel], indent=2))
PY
sign_file "$MANIFEST"
echo "==> manifest gate: $SEVO engine check-manifest"
"$SEVO" engine check-manifest "$MANIFEST" --sig "$MANIFEST.sig"

# --- release notes: the CHANGES.md section for this version, or the file given ---
NOTES="$RELEASES/$NAME-notes.md"
if [ -n "$NOTES_FILE" ]; then
    cp "$NOTES_FILE" "$NOTES"
elif [ -f "$HERE/CHANGES.md" ] && grep -q "^## $VERSION\b" "$HERE/CHANGES.md"; then
    awk -v v="$VERSION" '/^## /{p = ($2 == v)} p && !/^## /' "$HERE/CHANGES.md" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' > "$NOTES"
else
    echo "no section for $VERSION in build-macos/CHANGES.md and no --notes-file; the release body is the default one"
    : > "$NOTES"
fi
cat >> "$NOTES" <<EOT

Wine $WINE_VERSION. \`engine-info.json\` says what the tarball carries; \`$NAME.patch\` is the change against \`wine-staging-base\`.
Verify: \`shasum -a 256 -c $NAME.tar.xz.sha256\`; \`$NAME.tar.xz.sig\` is the Ed25519 signature Sevoflurane checks before installing.
EOT

if [ "$DRY_RUN" = 1 ]; then
    echo "==> dry run: not tagging, uploading or publishing the manifest"
    echo "    $TARBALL"
    echo "    $TARBALL.sha256  $TARBALL.sig"
    echo "    $MANIFEST  $MANIFEST.sig"
    echo "    $NOTES"
    exit 0
fi

echo "==> tag $VERSION"
git -C "$REPO" tag -s "$VERSION" -m "Engine $VERSION"
git -C "$REPO" push origin "$VERSION"

PRERELEASE=()
[ "$CHANNEL" = beta ] && PRERELEASE=(--prerelease)
echo "==> release $VERSION on $ENGINE_REPO"
gh release create "$VERSION" --repo "$ENGINE_REPO" --verify-tag --title "Engine $VERSION" \
    --notes-file "$NOTES" "${PRERELEASE[@]}" \
    "$TARBALL" "$TARBALL.sha256" "$TARBALL.sig" "$RELEASES/$NAME-engine-info.json" "$RELEASES/$NAME.patch"

# --- what GitHub holds is what was signed ---
UPLOADED="$(gh api "repos/$ENGINE_REPO/releases/tags/$VERSION" -q ".assets[] | select(.name == \"$NAME.tar.xz\") | .digest")"
[ "$UPLOADED" = "sha256:$SHA256" ] || { echo "GitHub reports $UPLOADED for the tarball, expected sha256:$SHA256 — the manifest was NOT published"; exit 1; }

echo "==> manifest on $APP_REPO"
gh release upload engine "$MANIFEST" "$MANIFEST.sig" --repo "$APP_REPO" --clobber
echo "==> published $VERSION as $CHANNEL"
