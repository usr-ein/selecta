#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the pinned Soulfind for a manual session: on PORT (default 2242), with a throwaway database that is removed
# when it stops (Ctrl-C). Point Selecta, selecta-cli or a reference peer at 127.0.0.1:PORT.
#
# Soulfind listens on every interface (it has no option to bind 127.0.0.1 alone), so this refuses to start without
# --all-interfaces. On a network you don't trust, turn on the macOS firewall's "Block all incoming connections"
# first. See README.md.
#
#   devenv/run-soulfind.sh --all-interfaces [PORT] [--debug]
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
if [[ "${1:-}" != "--all-interfaces" ]]; then
    echo "run-soulfind: Soulfind listens on all interfaces, not just 127.0.0.1: anyone on your network can reach it." >&2
    echo "run-soulfind: read devenv/README.md (firewall advice), then run: $0 --all-interfaces [PORT] [--debug]" >&2
    exit 2
fi
shift
port="${1:-2242}"
shift || true
if ! [[ "$port" =~ ^[0-9]+$ ]] || ((port < 1024 || port > 65535)); then
    echo "run-soulfind: PORT must be a number from 1024 to 65535" >&2
    exit 2
fi

binary="$here/.cache/soulfind/current/soulfind"
if [[ ! -x "$binary" ]]; then
    echo "run-soulfind: no Soulfind yet; fetch the pinned build first: devenv/fetch-soulfind.sh" >&2
    exit 1
fi
scratch="${TMPDIR:-/tmp}"
data="$(mktemp -d "${scratch%/}/soulfind-data.XXXXXX")"
trap 'rm -rf "$data"' EXIT

if [[ "$(uname -s)" == Darwin ]]; then
    # With the system SQLite, Soulfind stops at start-up ("SQLite error 7"); Homebrew's works (brew install sqlite).
    for prefix in /opt/homebrew/opt/sqlite /usr/local/opt/sqlite; do
        if [[ -d "$prefix/lib" ]]; then
            export DYLD_LIBRARY_PATH="$prefix/lib"
            break
        fi
    done
    if [[ -z "${DYLD_LIBRARY_PATH:-}" ]]; then
        echo "run-soulfind: Homebrew's sqlite isn't installed (brew install sqlite); trying the system one" >&2
    fi
fi

echo "run-soulfind: Soulfind on port $port, for clients at 127.0.0.1:$port; database in $data (removed on exit)"
"$binary" --port "$port" --database "$data/soulfind.db" "$@"
