#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Sends one command to a running Selecta started with --debug-server (see tools/dev/run-headless.sh).
#
#   tools/dev/app.sh state
#   tools/dev/app.sh navigate view=history
#   tools/dev/app.sh screenshot path=/tmp/history.png
#
# Environment: SELECTA_BUILD (default build/dev), SELECTA_DEBUG_SOCKET (default selecta-debug).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${SELECTA_BUILD:-$root/build/dev}"
exec "$build/tools/selecta-cli/selecta-cli" --socket "${SELECTA_DEBUG_SOCKET:-selecta-debug}" app "$@"
