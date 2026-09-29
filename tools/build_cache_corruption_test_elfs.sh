#!/bin/bash
# Build test ELFs that simulate a corrupted WebKit AppCache.
#
# Produces:
#   installer-simulate-once.elf   - every cache download fails until the user
#                                   confirms "Clear WebKit Data & Retry", then
#                                   the post-clear retry succeeds (tests the
#                                   full repair flow end to end)
#   installer-simulate-always.elf - every cache download always fails, so the
#                                   retry also errors (tests the terminal
#                                   "Cache issue persists" path)
#
# The build ends with a normal (SIMULATE=0) installer.elf restored.
#
# Usage from the repo root:
#   bash tools/build_cache_corruption_test_elfs.sh
# or inside the SDK container:
#   docker run --rm -v "$(pwd)":/src -w /src psviethoa-webkit-autoloader-sdk \
#       bash tools/build_cache_corruption_test_elfs.sh

set -e
cd "$(dirname "$0")/.."

build() {
    local mode="$1"
    if [ -x /opt/ps5-payload-sdk/bin/prospero-clang ]; then
        make clean all "SIMULATE=$mode"
    else
        echo "  (no local SDK - building via Docker psviethoa-webkit-autoloader-sdk)"
        docker run --rm -v "$(pwd)":/src -w /src psviethoa-webkit-autoloader-sdk \
            make clean all "SIMULATE=$mode"
    fi
}

echo "=== [1/3] SIMULATE=1 (fail until clear, then heal) ==="
build 1
cp installer.elf installer-simulate-once.elf

echo "=== [2/3] SIMULATE=2 (always fail) ==="
build 2
cp installer.elf installer-simulate-always.elf

echo "=== [3/3] Restoring normal SIMULATE=0 build ==="
build 0

echo ""
echo "Done. Test payloads (send to PS5 via elfldr / Payload Manager):"
ls -la installer.elf installer-simulate-once.elf installer-simulate-always.elf
