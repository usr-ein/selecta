#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts the crew's coordinator (docs/CREW.md) as the background session `selecta-nightman`, as the `nightman`
# agent, in the main checkout. Talk to it with `claude attach selecta-nightman`, or from `claude agents`.
#
#   tools/crew/start-nightman.sh [effort: high (default) | max]
set -euo pipefail
effort="${1:-high}"
root="$(git -C "$(dirname "$0")" rev-parse --path-format=absolute --git-common-dir)"
root="${root%/.git}"
if claude agents --json 2>/dev/null | grep -q '"name": *"selecta-nightman"'; then
    echo "a nightman session is already running: claude attach selecta-nightman"
    exit 1
fi
cd "$root"
claude --bg --name selecta-nightman --agent nightman --permission-mode bypassPermissions --effort "$effort" \
    --settings "{\"env\":{\"CREW_ROLE\":\"nightman\",\"CLAUDE_CODE_EFFORT_LEVEL\":\"$effort\"}}" \
    "Start your shift: do what 'On start' in your instructions says."
echo "started the nightman: claude attach selecta-nightman"
