#!/usr/bin/env bash
# One-shot local dev loop for the WebAssembly client: builds the wasm client
# (tools/wasm/build.sh) and the native gravitaris-server, runs the server,
# serves a freshly-built copy of the client from a cache-busted URL, and
# opens it in the default browser already pointed at the local server.
#
# Works on Windows (native server via CLion's bundled cmake + vcvars64.bat,
# same as docs' manual build recipe) and macOS/Linux (native server via
# system cmake, no toolchain shelling-out needed). Debug-only build type is
# not assumed anymore, but this has mostly only been exercised in Debug.
#
# Usage: tools/wasm/run-dev.sh [--release|--relwithdebinfo] [--no-server]
#                               [--no-browser] [--reconfigure]
#                               [--http-port N] [--ws-port N]
#
# Build type flags match tools/wasm/build.sh's (passed straight through to
# it); no flag means Debug. Applies to the native gravitaris-server build
# too, in its own out tree -- a type that hasn't been built here before gets
# a fresh CMake configure automatically (slow the first time, same as any
# other from-scratch config).
#
# Ctrl+C stops both the server and the local HTTP server this script started
# and cleans up after itself. Re-run any time to rebuild + get a fresh
# cache-busted URL -- no manual cache-clearing ever needed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# MSYS/Cygwin bash on Windows report their POSIX-emulation kernel name here,
# not "Windows" -- this is the same signal MSYS bash itself uses internally.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) IS_WIN=1 ;;
    *) IS_WIN=0 ;;
esac

BUILD_TYPE="Debug"
BUILD_FLAG=""
RUN_SERVER=1
OPEN_BROWSER=1
RECONFIGURE_ARG=""
HTTP_PORT=8123
WS_PORT=17890
for arg in "$@"; do
    case "$arg" in
        --release)         BUILD_TYPE="Release"; BUILD_FLAG="--release" ;;
        --relwithdebinfo)  BUILD_TYPE="RelWithDebInfo"; BUILD_FLAG="--relwithdebinfo" ;;
        --no-server)    RUN_SERVER=0 ;;
        --no-browser)   OPEN_BROWSER=0 ;;
        --reconfigure)  RECONFIGURE_ARG="--reconfigure" ;;
        --http-port=*)  HTTP_PORT="${arg#*=}" ;;
        --ws-port=*)    WS_PORT="${arg#*=}" ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# out/windows-msvc-<type> mirrors build.sh's own out/wasm-<type> naming, in
# lowercase to match the one native tree that already existed before this
# flag did (out/windows-msvc-debug, hand-configured via CLion).
NATIVE_BUILD_TYPE_DIR="$(printf '%s' "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"

# Converts an MSYS-style path (/c/Program Files/...) to the backslash form
# Windows tools (cmd.exe, paths embedded in a .bat) expect.
to_win_path() {
    local p="$1"
    p="${p/#\/c\//C:/}"
    p="${p//\//\\}"
    printf '%s' "$p"
}

echo "==> Building wasm client ($BUILD_TYPE)"
"$SCRIPT_DIR/build.sh" $BUILD_FLAG $RECONFIGURE_ARG

WASM_OUT_DIR="$REPO_ROOT/out/wasm-$BUILD_TYPE/$BUILD_TYPE/bin"

# --- Native gravitaris-server ------------------------------------------------
if [[ "$IS_WIN" == "1" ]]; then
    # Same CLion-bundled cmake + vcvars64.bat recipe used to build
    # GravitarisNG.exe by hand (see CLAUDE.md / project memory) -- located
    # here via glob so a CLion point release or a different VS edition/year
    # doesn't silently break this. Override with CMAKE_EXE / VCVARS_BAT /
    # NATIVE_BUILD_DIR env vars if none of the candidates below match.
    NATIVE_BUILD_DIR="${NATIVE_BUILD_DIR:-$REPO_ROOT/out/windows-msvc-$NATIVE_BUILD_TYPE_DIR}"
    SERVER_EXE="$NATIVE_BUILD_DIR/$BUILD_TYPE/bin/gravitaris-server.exe"
else
    # macOS/Linux use the system cmake directly, no per-platform toolchain
    # dance needed. Defaults to the top-level out/ tree that's already
    # hand-configured there (out/CMakeCache.txt, out/Debug/bin/...); override
    # with NATIVE_BUILD_DIR if you'd rather keep it in its own out/native-<type>
    # tree like the wasm build does.
    NATIVE_BUILD_DIR="${NATIVE_BUILD_DIR:-$REPO_ROOT/out}"
    SERVER_EXE="$NATIVE_BUILD_DIR/$BUILD_TYPE/bin/gravitaris-server"
fi

if [[ "$RUN_SERVER" == "1" ]]; then
    if [[ "$IS_WIN" == "1" ]]; then
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

        if [[ -z "${CMAKE_EXE:-}" || -z "${VCVARS_BAT:-}" ]]; then
            echo "error: could not find CLion's cmake and/or vcvars64.bat." >&2
            echo "Set CMAKE_EXE and VCVARS_BAT explicitly, e.g.:" >&2
            echo "  CMAKE_EXE=/c/.../cmake.exe VCVARS_BAT=/c/.../vcvars64.bat tools/wasm/run-dev.sh" >&2
            exit 1
        fi

        echo "==> Building native gravitaris-server ($BUILD_TYPE)"
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
            # A build type that hasn't been used here before has no configured
            # tree yet (out/windows-msvc-debug is the one CLion set up by hand;
            # release/relwithdebinfo start out empty) -- configure it first, same
            # one-time cost tools/wasm/build.sh's own --reconfigure path pays.
            if [[ "$RECONFIGURE_ARG" == "--reconfigure" || ! -f "$NATIVE_BUILD_DIR/build.ninja" ]]; then
                echo "\"$(to_win_path "$CMAKE_EXE")\" -S \"$(to_win_path "$REPO_ROOT")\" -B \"$(to_win_path "$NATIVE_BUILD_DIR")\" -G Ninja -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
            fi
            echo "\"$(to_win_path "$CMAKE_EXE")\" --build \"$(to_win_path "$NATIVE_BUILD_DIR")\" --target gravitaris-server"
        } > "$BUILD_SERVER_BAT"
        cmd //c "$BUILD_SERVER_BAT"
    else
        echo "==> Building native gravitaris-server ($BUILD_TYPE)"
        CACHED_TYPE=""
        if [[ -f "$NATIVE_BUILD_DIR/CMakeCache.txt" ]]; then
            CACHED_TYPE="$(grep -E '^CMAKE_BUILD_TYPE:' "$NATIVE_BUILD_DIR/CMakeCache.txt" | cut -d= -f2)"
        fi
        if [[ "$RECONFIGURE_ARG" == "--reconfigure" || ! -f "$NATIVE_BUILD_DIR/build.ninja" || "$CACHED_TYPE" != "$BUILD_TYPE" ]]; then
            cmake -S "$REPO_ROOT" -B "$NATIVE_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        fi
        cmake --build "$NATIVE_BUILD_DIR" --target gravitaris-server -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
    fi
fi

# --- (Re)start the server ----------------------------------------------------
if [[ "$RUN_SERVER" == "1" ]]; then
    if [[ "$IS_WIN" == "1" ]]; then
        EXISTING_PID="$(tasklist //FI "IMAGENAME eq gravitaris-server.exe" //FO CSV //NH 2>/dev/null \
            | tr -d '"' | cut -d, -f2)"
        if [[ "$EXISTING_PID" =~ ^[0-9]+$ ]]; then
            echo "==> Stopping previous gravitaris-server.exe (PID $EXISTING_PID)"
            taskkill //F //PID "$EXISTING_PID" >/dev/null 2>&1 || true
            sleep 1
        fi
    else
        EXISTING_PID="$(pgrep -f "$SERVER_EXE" || true)"
        if [[ -n "$EXISTING_PID" ]]; then
            echo "==> Stopping previous gravitaris-server (PID $EXISTING_PID)"
            kill $EXISTING_PID 2>/dev/null || true
            sleep 1
        fi
    fi

    # Its own real console/terminal window, not piped to a log file: the
    # server reads operator commands (spawn/list/team/quit -- see
    # src/server/main.cpp) from stdin, which only works with a real
    # interactive console attached. A backgrounded/redirected process has no
    # usable stdin for that.
    echo "==> Starting gravitaris-server on ws://127.0.0.1:$WS_PORT in its own console window"
    echo "    (type commands there: spawn [count] [preset] | list | team <peer-id> <color> | quit)"
    if [[ "$IS_WIN" == "1" ]]; then
        # `start` returns immediately once the new window is up, so there's
        # no useful child PID to track the actual server.exe with -- cleanup()
        # below kills it by image name instead, same as the stale-process
        # check above.
        cmd //c start "Gravitaris Server" "$SERVER_EXE" "$WS_PORT"
    else
        # osascript's `do script` opens a fresh Terminal.app window running
        # the command, same real-stdin requirement as the Windows path. As
        # with `start` above, the PID this returns is Terminal's/the shell's,
        # not the server's -- cleanup() below kills by matching the exe path
        # instead of tracking a PID.
        osascript -e "tell application \"Terminal\" to do script \"'$SERVER_EXE' $WS_PORT\"" >/dev/null
    fi

    for _ in $(seq 1 30); do
        if [[ "$IS_WIN" == "1" ]]; then
            if netstat -ano 2>/dev/null | grep "LISTENING" | grep -q ":${WS_PORT}[[:space:]]"; then break; fi
        else
            if lsof -nP -iTCP:"$WS_PORT" -sTCP:LISTEN >/dev/null 2>&1; then break; fi
        fi
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
# actually a python process, to avoid taking down an unrelated service if
# the user happens to have something else on this port.
if [[ "$IS_WIN" == "1" ]]; then
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
else
    for PID in $(lsof -ti tcp:"$HTTP_PORT" -sTCP:LISTEN 2>/dev/null); do
        IMAGE="$(ps -p "$PID" -o comm= 2>/dev/null | xargs -n1 basename 2>/dev/null || true)"
        if [[ "$IMAGE" == python* ]]; then
            echo "==> Stopping stale http.server on port $HTTP_PORT (PID $PID)"
            kill "$PID" 2>/dev/null || true
        else
            echo "warning: port $HTTP_PORT is in use by '$IMAGE' (PID $PID), not python -- leaving it alone." >&2
            echo "         pass --http-port=N to use a different port." >&2
        fi
    done
fi

HTTP_LOG="$REPO_ROOT/out/http-dev.log"
echo "==> Serving $SERVE_DIR on http://127.0.0.1:$HTTP_PORT (log: $HTTP_LOG)"
(cd "$SERVE_DIR" && "$PY" -m http.server "$HTTP_PORT" > "$HTTP_LOG" 2>&1) &
HTTP_PID=$!

cleanup() {
    echo
    echo "==> Cleaning up"
    [[ -n "${HTTP_PID:-}" ]] && kill "$HTTP_PID" 2>/dev/null || true
    if [[ "$RUN_SERVER" == "1" ]]; then
        if [[ "$IS_WIN" == "1" ]]; then
            taskkill //F //IM gravitaris-server.exe >/dev/null 2>&1 || true
        else
            pkill -f "$SERVER_EXE" 2>/dev/null || true
        fi
    fi
}
trap cleanup EXIT INT TERM

sleep 0.5

URL="http://127.0.0.1:$HTTP_PORT/?v=$TOKEN"
[[ "$RUN_SERVER" == "1" ]] && URL="$URL&connect=ws://127.0.0.1:$WS_PORT"

if [[ "$OPEN_BROWSER" == "1" ]]; then
    echo "==> Opening $URL"
    if [[ "$IS_WIN" == "1" ]]; then
        # A generated .bat, not an inline `cmd //c start "" "$URL"` -- same
        # MSYS-argv-to-Win32-cmdline mangling as the server build's .bat (see
        # its own comment), but tripped by `&` specifically here:
        # `?v=...&connect=...` was silently getting split at the `&` as if
        # it were a second shell command, so the browser opened with the
        # connect param truncated off (found via a real playtesting session
        # -- the auto-connect never fired).
        OPEN_URL_BAT="$REPO_ROOT/out/.open-url.bat"
        {
            echo "@echo off"
            echo "start \"\" \"$URL\""
        } > "$OPEN_URL_BAT"
        cmd //c "$OPEN_URL_BAT" >/dev/null 2>&1 || true
    else
        open "$URL" >/dev/null 2>&1 || true
    fi
else
    echo "==> Client ready at: $URL"
fi

echo "==> Running. Ctrl+C to stop (the server's own console window closes separately -- Ctrl+C there or just leave it, cleanup kills it either way)."
wait "$HTTP_PID"
