#!/usr/bin/env bash
# Configures and builds GravitarisNG for WebAssembly/WebGL2 via Emscripten.
# See docs/wasm-plan.md for the phased plan this executes.
#
# Requires emsdk installed and NOT necessarily sourced/activated in the
# calling shell -- this script locates emcc/emcmake itself so it works the
# same from bash, zsh, or fish. Point it at your emsdk checkout via EMSDK_DIR
# if it isn't at one of the default locations below.
#
# Usage: tools/wasm/build.sh [--reconfigure] [--release]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/out-wasm"

# --- Locate emsdk -----------------------------------------------------------
CANDIDATE_DIRS=(
    "${EMSDK_DIR:-}"
    "$HOME/Projects/emsdk"
    "$HOME/emsdk"
)
EMSDK_FOUND=""
for dir in "${CANDIDATE_DIRS[@]}"; do
    if [[ -n "$dir" && -x "$dir/upstream/emscripten/emcc" ]]; then
        EMSDK_FOUND="$dir"
        break
    fi
done
if [[ -z "$EMSDK_FOUND" ]]; then
    echo "error: could not find an emsdk install (emcc not found)." >&2
    echo "Set EMSDK_DIR to your emsdk checkout, e.g.:" >&2
    echo "  EMSDK_DIR=~/Projects/emsdk tools/wasm/build.sh" >&2
    exit 1
fi
export PATH="$EMSDK_FOUND/upstream/emscripten:$EMSDK_FOUND:$PATH"
echo "Using emsdk at: $EMSDK_FOUND"
emcc --version | head -1

# --- Args --------------------------------------------------------------------
RECONFIGURE=0
BUILD_TYPE="Debug"
for arg in "$@"; do
    case "$arg" in
        --reconfigure) RECONFIGURE=1 ;;
        --release) BUILD_TYPE="Release" ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

if [[ "$RECONFIGURE" == "1" || ! -f "$BUILD_DIR/build.ninja" ]]; then
    emcmake cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

cmake --build "$BUILD_DIR" --target GravitarisNG -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

echo
echo "Build output: $BUILD_DIR/$BUILD_TYPE/bin/GravitarisNG.js (+ .wasm, .data once data is preloaded)"
echo "Serve it with, e.g.: (cd $BUILD_DIR/$BUILD_TYPE/bin && python3 -m http.server 8080)"
