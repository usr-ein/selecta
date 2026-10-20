#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Sends one command to a running Selecta started with --debug-server (see tools/dev/run-headless.sh).
#
#   tools/dev/app.sh state
#   tools/dev/app.sh navigate view=history
#   tools/dev/app.sh screenshot path=/tmp/history.png
#   tools/dev/app.sh click name=<button or action>
#   tools/dev/app.sh type name=<line edit, text edit or spin box> text=<text> [submit=true]
#       a spin box takes a whole number: type name=settings.network.port text=2416
#   tools/dev/app.sh select name=<list, table or tree> row=<n> [parent=<n>]
#   tools/dev/app.sh select name=<tab widget> row=<n>
#   tools/dev/app.sh select name=<combo box> row=<n> | text=<item text>
#       as the user picks it: select name=settings.preview.source text="YouTube only"
#   tools/dev/app.sh tree [name=<widget>] [depth=<n>]   object names to use above
#
# Values are JSON when they parse as JSON (numbers, true/false, "quoted strings"), plain text otherwise.
#
# Environment: SELECTA_BUILD (default build/dev), SELECTA_DEBUG_SOCKET (default selecta-debug).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${SELECTA_BUILD:-$root/build/dev}"
exec "$build/tools/selecta-cli/selecta-cli" --socket "${SELECTA_DEBUG_SOCKET:-selecta-debug}" app "$@"
