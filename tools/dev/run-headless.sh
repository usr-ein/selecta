#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts Selecta for inspection: native macOS rendering, nothing shown on screen, the debug socket on, and a
# throwaway data folder so the real library and settings are never touched. Waits until the socket answers.
#
#   tools/dev/run-headless.sh [data-dir]      prints the PID; stop it with: tools/dev/app.sh quit
#
# Environment: SELECTA_BUILD (default build/dev), SELECTA_DEBUG_SOCKET (default selecta-debug).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${SELECTA_BUILD:-$root/build/dev}"
socket="${SELECTA_DEBUG_SOCKET:-selecta-debug}"
data="${1:-$(mktemp -d "${TMPDIR:-/tmp}/selecta-run.XXXXXX")}"
app="$build/src/app/Selecta.app/Contents/MacOS/Selecta"

[[ -x "$app" ]] || { echo "not built: $app (run: cmake --build --preset dev)" >&2; exit 1; }
# Another instance on the socket would answer the pings below in place of the new one.
if SELECTA_BUILD="$build" SELECTA_DEBUG_SOCKET="$socket" "$root/tools/dev/app.sh" ping >/dev/null 2>&1; then
    echo "a Selecta already answers on socket $socket: quit it first (tools/dev/app.sh quit)" >&2
    exit 1
fi
mkdir -p "$data"
"$app" --headless --debug-server --debug-socket "$socket" --data-dir "$data" >"$data/stdout.log" 2>&1 &
pid=$!

for _ in $(seq 1 50); do
    if SELECTA_BUILD="$build" SELECTA_DEBUG_SOCKET="$socket" "$root/tools/dev/app.sh" ping >/dev/null 2>&1; then
        echo "Selecta $pid is up; data in $data; socket $socket"
        exit 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "Selecta exited during start-up; see $data/stdout.log and $data/logs/selecta.log" >&2
        exit 1
    fi
    sleep 0.2
done
echo "Selecta $pid didn't open its debug socket within 10 s" >&2
kill "$pid" 2>/dev/null || true
exit 1
