#!/usr/bin/env bash
# Prepare the relapse copy used by the frontend and apply our autoloader patch.
#
# relapse lives as a pristine copy in third_party/relapse (a vendored snapshot
# or a submodule checkout — never modified). The frontend needs it under
# frontend/autoloader/relapse, so this script:
#   1. copies third_party/relapse -> frontend/autoloader/relapse (fresh copy)
#   2. applies patches/relapse-autoload.patch to the copy
#
# The copy is gitignored (frontend/autoloader/relapse/), so the source is
# never dirtied. Run after every source update:
#
#   tools/apply_relapse_patch.sh
#
# The Makefile runs this automatically before staging/serving (relapse-prepare).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/third_party/relapse"
DEST="$ROOT/frontend/autoloader/relapse"
PATCH="$ROOT/patches/relapse-autoload.patch"

if [ ! -d "$SOURCE" ] || [ -z "$(ls -A "$SOURCE" 2>/dev/null)" ]; then
    echo "Error: relapse sources are missing (third_party/relapse is empty)."
    echo "Vendor the sources into third_party/relapse (see third_party/VENDORED.md)."
    exit 1
fi

if [ ! -f "$PATCH" ]; then
    echo "Error: patch file not found: $PATCH"
    exit 1
fi

# 1. Fresh copy (drop .git, .github, .gitignore, .gitmodules — anything not
#    needed at runtime).
rm -rf "$DEST"
mkdir -p "$DEST"
cp -R "$SOURCE"/. "$DEST"/
rm -rf "$DEST/.git" "$DEST/.github" "$DEST/.gitignore" "$DEST/.gitmodules"

# 2. Turn the copy into a throwaway git repo so `git apply` can handle the
#    patch. Two commits: pristine relapse, then our autoloader patch.
SRC_HASH=$(git -C "$SOURCE" rev-parse --short HEAD 2>/dev/null || echo vendored)
git -C "$DEST" init -q
git -C "$DEST" config user.name "wkal"
git -C "$DEST" config user.email "wkal@localhost"
git -C "$DEST" add -A
git -C "$DEST" commit -q -m "relapse pristine ($SRC_HASH)"

# 3. Apply the patch
cd "$DEST"
if git apply --check "$PATCH" 2>/dev/null; then
    git apply "$PATCH"
    git add -A
    git commit -q -m "Apply WKAL autoloader patch"
    echo "relapse: copied to $DEST and autoloader patch applied."
elif git apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "relapse: autoloader patch is already applied."
else
    echo "Error: patch does not apply cleanly to $DEST."
    echo "--- git apply says ---"
    git apply --verbose --check "$PATCH" 2>&1 | head -40
    echo "----------------------"
    echo "relapse has likely changed upstream — regenerate patches/relapse-autoload.patch"
    echo "from the pristine third_party/relapse and re-run."
    exit 1
fi

# 4. Sanity check: the patched files must carry our integration markers.
#    Catches a silently truncated/empty patch. These markers only exist when
#    the patch applied (they are not in pristine relapse).
if ! grep -q 'const AUTOLOAD = new URLSearchParams' src/main.js \
    || ! grep -q 'loadAutoloadPayload(p, chain, AUTOLOAD' src/main.js \
    || ! grep -q 'postAutoloadResult({ type: "wkal"' src/main.js \
    || ! grep -q '// WKAL: plain relative path' src/main.js \
    || grep -q 'Date.now' src/main.js \
    || ! grep -q 'export async function loadAutoloadPayload' src/kexp.js \
    || ! grep -q 'mapElf(name, p, chain, "../payloads/"' src/kexp.js \
    || ! grep -q '// WKAL: report chain-level failures' src/site.js; then
    echo "Error: relapse patch verification FAILED — integration markers missing."
    echo "patches/relapse-autoload.patch is incomplete or out of date."
    echo "Regenerate it from the pristine third_party/relapse and re-run."
    exit 1
fi
echo "relapse: patch verification OK (autoload via shared payloads dir,"
echo "         stable offsets URL for AppCache, parent result messages)."
