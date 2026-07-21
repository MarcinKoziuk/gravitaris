#!/usr/bin/env bash
# One-shot local dev loop for the WebAssembly client: builds the wasm client
# (tools/wasm/build.sh) and the native gravitaris-server, runs the server,
# serves a freshly-built copy of the client from a cache-busted URL, and
# opens it in the default browser already pointed at the local server.
#
# Windows-only for now (the native server build shells out to vcvars64.bat +
# CLion's bundled cmake, same as docs' manual build recipe) -- debug-only for
# now too; both are candidates to generalize later if this is ever run from
# somewhere other than this one dev machine.
#
# Usage: tools/wasm/run-dev.sh [--no-server] [--no-browser] [--reconfigure]
#                               [--http-port N] [--ws-port N]
#
# Ctrl+C stops both the server and the local HTTP server this script started
# and cleans up after itself. Re-run any time to rebuild + get a fresh
# cache-busted URL -- no manual cache-clearing ever needed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

RUN_SERVER=1
OPEN_BROWSER=1
RECONFIGURE_ARG=""
HTTP_PORT=8080
WS_PORT=17890
for arg in "$@"; do
    case "$arg" in
        --no-server)    RUN_SERVER=0 ;;
        --no-browser)   OPEN_BROWSER=0 ;;
        --reconfigure)  RECONFIGURE_ARG="--reconfigure" ;;
        --http-port=*)  HTTP_PORT="${arg#*=}" ;;
        --ws-port=*)    WS_PORT="${arg#*=}" ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# Converts an MSYS-style path (/c/Program Files/...) to the backslash form
# Windows tools (cmd.exe, paths embedded in a .bat) expect.
to_win_path() {
    local p="$1"
    p="${p/#\/c\//C:/}"
    p="${p//\//\\}"
    printf '%s' "$p"
}

echo "==> Building wasm client (Debug)"
"$SCRIPT_DIR/build.sh" $RECONFIGURE_ARG

WASM_OUT_DIR="$REPO_ROOT/out/wasm-Debug/Debug/bin"

# --- Native gravitaris-server ------------------------------------------------
# Same CLion-bundled cmake + vcvars64.bat recipe used to build GravitarisNG.exe
# by hand (see CLAUDE.md / project memory) -- located here via glob so a CLion
# point release or a different VS edition/year doesn't silently break this.
# Override with CMAKE_EXE / VCVARS_BAT / NATIVE_BUILD_DIR env vars if none of
# the candidates below match.
SERVER_EXE="$REPO_ROOT/out/windows-msvc-debug/Debug/bin/gravitaris-server.exe"
if [[ "$RUN_SERVER" == "1" ]]; then
    if [[ -z "${CMAKE_EXE:-}" ]]; then
        shopt -s nullglob
        CANDIDATES=(/c/Program\ Files/JetBrains/CLion*/bin/cmake/win/x64/bin/cmake.exe)
        shopt -u nullglob
        [[ ${#CANDIDATES[@]} -gt 0 ]] && CMAKE_EXE="${CANDIDATES[-1]}"
    fi
    if [[ -z "${VCVARS_BAT:-}" ]]; then
        shopt -s nullglob
        CANDIDATES=(/c/Program\ Files/Microsoft\ Visual\ Studio/20{22,19}/{Community,Professional,Enterprise,BuildTools}/VC/Auxiliary/Build/vcvars64.bat)
        shopt -u nullglob
        [[ ${#CANDIDATES[@]} -gt 0 ]] && VCVARS_BAT="${CANDIDATES[0]}"
    fi
    NATIVE_BUILD_DIR="${NATIVE_BUILD_DIR:-$REPO_ROOT/out/windows-msvc-debug}"

    if [[ -z "${CMAKE_EXE:-}" || -z "${VCVARS_BAT:-}" ]]; then
        echo "error: could not find CLion's cmake and/or vcvars64.bat." >&2
        echo "Set CMAKE_EXE and VCVARS_BAT explicitly, e.g.:" >&2
        echo "  CMAKE_EXE=/c/.../cmake.exe VCVARS_BAT=/c/.../vcvars64.bat tools/wasm/run-dev.sh" >&2
        exit 1
    fi

    echo "==> Building native gravitaris-server (Debug)"
    # A generated .bat, not an inline `cmd //c "..."` one-liner: MSYS bash's
    # argv->Win32-command-line re-quoting mangles nested quoted paths (the
    # vcvars/cmake paths below have spaces), so round-tripping through a
    # plain-text file sidesteps that entirely. The .bat's own path has no
    # spaces (repo lives under .../GravitarisNG/out/), so invoking *it*
    # doesn't need quoting either.
    BUILD_SERVER_BAT="$REPO_ROOT/out/.build-server.bat"
    mkdir -p "$REPO_ROOT/out"
    {
        echo "@echo off"
        echo "call \"$(to_win_path "$VCVARS_BAT")\" >nul"
        echo "\"$(to_win_path "$CMAKE_EXE")\" --build \"$(to_win_path "$NATIVE_BUILD_DIR")\" --target gravitaris-server"
    } > "$BUILD_SERVER_BAT"
    cmd //c "$BUILD_SERVER_BAT"
fi

# --- (Re)start the server ----------------------------------------------------
if [[ "$RUN_SERVER" == "1" ]]; then
    EXISTING_PID="$(tasklist //FI "IMAGENAME eq gravitaris-server.exe" //FO CSV //NH 2>/dev/null \
        | tr -d '"' | cut -d, -f2)"
    if [[ "$EXISTING_PID" =~ ^[0-9]+$ ]]; then
        echo "==> Stopping previous gravitaris-server.exe (PID $EXISTING_PID)"
        taskkill //F //PID "$EXISTING_PID" >/dev/null 2>&1 || true
        sleep 1
    fi

    SERVER_LOG="$REPO_ROOT/out/server-dev.log"
    echo "==> Starting gravitaris-server on ws://127.0.0.1:$WS_PORT (log: $SERVER_LOG)"
    "$SERVER_EXE" "$WS_PORT" > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!

    for _ in $(seq 1 30); do
        if grep -q "listening on" "$SERVER_LOG" 2>/dev/null; then break; fi
        sleep 0.2
    done
fi

# --- Cache-busted serve directory --------------------------------------------
# The Emscripten glue script (GravitarisNG.js) locates its .wasm/.data
# siblings relative to its own script directory, not by a name it derives
# from the <script> tag -- so a fresh, uniquely-named directory per run
# cache-busts all three together for free, more reliably than renaming just
# the .js file (which the glue script's internal wasm/data references
# wouldn't follow). Old dirs are cheap to leave behind under out/ (gitignored)
# but pruned here anyway so repeated runs don't pile them up.
TOKEN="$(date +%s)-$RANDOM"
SERVE_ROOT="$WASM_OUT_DIR/serve"
SERVE_DIR="$SERVE_ROOT/$TOKEN"
rm -rf "$SERVE_ROOT"/*/ 2>/dev/null || true
mkdir -p "$SERVE_DIR"
cp "$WASM_OUT_DIR/GravitarisNG.js" "$WASM_OUT_DIR/GravitarisNG.wasm" "$WASM_OUT_DIR/GravitarisNG.data" "$SERVE_DIR/"
# Belt-and-suspenders per the user's ask: also cache-bust the <script> tag
# itself with an explicit query param, in case a proxy/browser somehow keys
# on URL rather than path.
sed "s/GravitarisNG\.js\"/GravitarisNG.js?v=$TOKEN\"/" "$SCRIPT_DIR/index.html" > "$SERVE_DIR/index.html"

# --- HTTP server --------------------------------------------------------------
PY="$(command -v python || true)"
[[ -z "$PY" ]] && PY="$(command -v python3 || true)"
if [[ -z "$PY" ]]; then
    echo "error: no working python found on PATH to serve the client." >&2
    exit 1
fi

# A prior run's http.server can outlive this script if its terminal was
# closed rather than Ctrl+C'd (the EXIT/INT/TERM trap below never runs, so
# it's orphaned but keeps the port). If left alone, the *next* run's
# directory-prune above (rm -rf "$SERVE_ROOT"/*/) deletes that orphan's
# still-being-served directory out from under it -- symptom seen in
# practice: the browser opens to a bare Python directory listing with no
# game at all, because the orphan is still what's actually bound to the
# port, not the freshly started server below. Only kills it if it's
# actually python.exe, to avoid taking down an unrelated service if the
# user happens to have something else on this port.
for PID in $(netstat -ano | grep "LISTENING" | grep -E ":${HTTP_PORT}[[:space:]]" \
        | awk '{print $NF}' | sort -u); do
    IMAGE="$(tasklist //FI "PID eq $PID" //FO CSV //NH 2>/dev/null | tr -d '"' | cut -d, -f1)"
    if [[ "$IMAGE" == python* ]]; then
        echo "==> Stopping stale http.server on port $HTTP_PORT (PID $PID)"
        taskkill //F //PID "$PID" >/dev/null 2>&1 || true
    else
        echo "warning: port $HTTP_PORT is in use by '$IMAGE' (PID $PID), not python -- leaving it alone." >&2
        echo "         pass --http-port=N to use a different port." >&2
    fi
done

HTTP_LOG="$REPO_ROOT/out/http-dev.log"
echo "==> Serving $SERVE_DIR on http://127.0.0.1:$HTTP_PORT (log: $HTTP_LOG)"
(cd "$SERVE_DIR" && "$PY" -m http.server "$HTTP_PORT" > "$HTTP_LOG" 2>&1) &
HTTP_PID=$!

cleanup() {
    echo
    echo "==> Cleaning up"
    [[ -n "${HTTP_PID:-}" ]] && kill "$HTTP_PID" 2>/dev/null || true
    if [[ "$RUN_SERVER" == "1" && -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

sleep 0.5

URL="http://127.0.0.1:$HTTP_PORT/?v=$TOKEN"
[[ "$RUN_SERVER" == "1" ]] && URL="$URL&connect=ws://127.0.0.1:$WS_PORT"

if [[ "$OPEN_BROWSER" == "1" ]]; then
    echo "==> Opening $URL"
    cmd //c start "" "$URL" >/dev/null 2>&1 || true
else
    echo "==> Client ready at: $URL"
fi

echo "==> Running. Ctrl+C to stop."
if [[ "$RUN_SERVER" == "1" ]]; then
    tail -f "$SERVER_LOG" &
    TAIL_PID=$!
    trap 'kill "$TAIL_PID" 2>/dev/null || true; cleanup' EXIT INT TERM
fi
wait "$HTTP_PID"
