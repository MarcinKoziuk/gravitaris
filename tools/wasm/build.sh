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
# emcc's own shell wrapper execs $EMSDK_PYTHON if set, else falls back to
# whatever `python3` is on PATH. emscripten requires 3.10+; macOS ships an
# ancient CommandLineTools python3 (3.9.x) that plenty of machines resolve by
# default, which fails with an assertion deep inside emcc.py that looks
# unrelated to PATH at a glance. Point it at emsdk's own vendored interpreter
# explicitly so this can't happen regardless of the calling shell's default.
EMSDK_PYTHON="$(ls -d "$EMSDK_FOUND"/python/*/bin/python3 2>/dev/null | head -1)"
if [[ -n "$EMSDK_PYTHON" ]]; then
    export EMSDK_PYTHON
fi
export PATH="$EMSDK_FOUND/upstream/emscripten:$EMSDK_FOUND:$PATH"
echo "Using emsdk at: $EMSDK_FOUND"
echo "Using python: ${EMSDK_PYTHON:-$(command -v python3)} ($("${EMSDK_PYTHON:-python3}" --version))"
emcc --version | head -1

# --- Args --------------------------------------------------------------------
RECONFIGURE=0
BUILD_TYPE="Debug"
for arg in "$@"; do
    case "$arg" in
        --reconfigure)     RECONFIGURE=1 ;;
        --release)         BUILD_TYPE="Release" ;;
        # -O2 + NDEBUG but no -flto -- used to bisect Release-only bugs (is it
        # the optimization level, or LTO?).
        --relwithdebinfo)  BUILD_TYPE="RelWithDebInfo" ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# out-wasm is a single-config (Ninja) build tree, so CMAKE_BUILD_TYPE is baked
# at configure time. Switching Debug<->Release therefore REQUIRES a reconfigure
# -- otherwise `cmake --build` happily rebuilds the previously-configured type
# and the artifacts land under a different <Config>/bin than this run expects
# (which previously made the index.html copy below fail with a confusing "No
# such file or directory"). Detect the cached type and reconfigure on a
# mismatch.
CACHED_TYPE=""
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    CACHED_TYPE="$(grep -E '^CMAKE_BUILD_TYPE:' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2)"
fi

if [[ "$RECONFIGURE" == "1" || ! -f "$BUILD_DIR/build.ninja" || "$CACHED_TYPE" != "$BUILD_TYPE" ]]; then
    emcmake cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

cmake --build "$BUILD_DIR" --target GravitarisNG -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

OUT_DIR="$BUILD_DIR/$BUILD_TYPE/bin"
cp "$SCRIPT_DIR/index.html" "$OUT_DIR/index.html"

echo
echo "Build type:   $BUILD_TYPE"
echo "Build output: $OUT_DIR/ (GravitarisNG.js/.wasm/.data, index.html)"
echo "Serve it with, e.g.: (cd $OUT_DIR && python3 -m http.server 8080)"
