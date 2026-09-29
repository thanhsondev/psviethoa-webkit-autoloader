#!/usr/bin/env bash
# Prepare the umtx2 copy used by the frontend and apply our autoloader patch.
#
# umtx2 lives as a pristine git submodule in third_party/umtx2 (never
# modified). The frontend needs the exploit under frontend/autoloader/umtx2,
# so this script:
#   1. copies third_party/umtx2/document/en/ps5 -> frontend/autoloader/umtx2
#      (flattened: the exploit's document/en/ps5 dir becomes the copy root, so
#      the iframe URL is the short /app/umtx2/index.html)
#   2. prunes the bundled payloads dir down to just elfldr-ps5.elf (the chain
#      boots its own elfldr like stock umtx2; the payload menu is unused)
#   3. applies patches/umtx2-autoload.patch to the copy
#
# The copy is gitignored (frontend/autoloader/umtx2/), so the submodule is
# never dirtied. Run after every submodule update:
#
#   git submodule update --init --recursive
#   tools/apply_umtx2_patch.sh
#
# The Makefile runs this automatically before staging/serving (umtx2-prepare).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/third_party/umtx2"
DEST="$ROOT/frontend/autoloader/umtx2"
PATCH="$ROOT/patches/umtx2-autoload.patch"

if [ ! -d "$SOURCE" ] || [ -z "$(ls -A "$SOURCE" 2>/dev/null)" ]; then
    echo "Error: umtx2 sources are missing (third_party/umtx2 is empty)."
    echo "Initialize the submodule (git submodule update --init --recursive)"
    echo "or vendor the sources into third_party/umtx2."
    exit 1
fi

if [ ! -f "$PATCH" ]; then
    echo "Error: patch file not found: $PATCH"
    exit 1
fi

# 1. Fresh copy of the exploit root (document/en/ps5), flattened. Prune the
#    bundled payloads dir down to umtx2's own elfldr-ps5.elf (like stock umtx2)
#    — the autoload payload comes from /app/payloads/ and the payload menu is
#    unused, but the chain boots its own elfldr, not the shared one.
rm -rf "$DEST"
mkdir -p "$DEST"
cp -R "$SOURCE/document/en/ps5"/. "$DEST"/
rm -rf "$DEST/payloads"
mkdir -p "$DEST/payloads"
cp "$SOURCE/document/en/ps5/payloads/elfldr-ps5.elf" "$DEST/payloads/"

# 2. Turn the copy into a throwaway git repo so `git apply` can handle the
#    patch. Two commits: pristine umtx2, then our autoloader patch.
SRC_HASH=$(git -C "$SOURCE" rev-parse --short HEAD 2>/dev/null || echo vendored)
git -C "$DEST" init -q
git -C "$DEST" config user.name "wkal"
git -C "$DEST" config user.email "wkal@localhost"
git -C "$DEST" add -A
git -C "$DEST" commit -q -m "umtx2 pristine (flattened document/en/ps5, submodule $SRC_HASH)"

# 3. Apply the patch
cd "$DEST"
if git apply --check "$PATCH" 2>/dev/null; then
    git apply "$PATCH"
    git add -A
    git commit -q -m "Apply WKAL autoloader patch"
    echo "umtx2: copied to $DEST and autoloader patch applied."
elif git apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "umtx2: autoloader patch is already applied."
else
    echo "Error: patch does not apply cleanly to $DEST."
    echo "umtx2 has likely changed upstream — regenerate patches/umtx2-autoload.patch"
    echo "(diff the pristine third_party/umtx2 against a patched copy)."
    exit 1
fi

# 4. Sanity check: the patched main.js must carry our integration markers, the
#    elfldr must come from the bundled payloads dir (stock umtx2, not the shared
#    one), the confirm() dialogs must be gone, and payloads/ must contain only
#    elfldr-ps5.elf. Catches a silently truncated patch.
if ! grep -q 'wkalAutoloadName = new URLSearchParams' main.js \
    || ! grep -q 'wkalAutoload' main.js \
    || ! grep -q 'load_local_elf("elfldr-ps5.elf")' main.js \
    || grep -q 'confirm(' main.js \
    || grep -q '../shared/' main.js \
    || ! [ -f payloads/elfldr-ps5.elf ] \
    || [ "$(find payloads -maxdepth 1 -type f | wc -l)" -ne 1 ]; then
    echo "Error: umtx2 patch verification FAILED — integration markers missing."
    echo "patches/umtx2-autoload.patch is incomplete or out of date."
    echo "Regenerate it from the pristine submodule and re-run."
    exit 1
fi
echo "umtx2: patch verification OK (mainloop autoload, own elfldr, confirm removed, payloads pruned)."
