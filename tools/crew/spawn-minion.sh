#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts a minion on one pitch (docs/CREW.md, step 3). It makes the worktree .claude/worktrees/P-NNN-slug on a new
# branch crew/P-NNN-slug from origin/main, then starts the background session selecta-minion-P-NNN-slug in it, as the
# `minion` agent. With --resume, it restarts a minion whose session died, on the worktree it left.
#
#   tools/crew/spawn-minion.sh [--resume] P-NNN slug [effort: medium | high (default) | xhigh | max]
#
# The nightman picks the effort per pitch (docs/CREW.md); anything outside medium..max is refused.
set -euo pipefail
resume=0
if [[ "${1:-}" == "--resume" ]]; then
    resume=1
    shift
fi
pitch="${1:?usage: spawn-minion.sh [--resume] P-NNN slug [effort]}"
slug="${2:?usage: spawn-minion.sh [--resume] P-NNN slug [effort]}"
effort="${3:-high}"
case "$effort" in
    medium | high | xhigh | max) ;;
    *)
        echo "effort must be medium, high, xhigh or max (got: $effort)"
        exit 1
        ;;
esac
root="$(git -C "$(dirname "$0")" rev-parse --path-format=absolute --git-common-dir)"
root="${root%/.git}"
name="selecta-minion-$pitch-$slug"
branch="crew/$pitch-$slug"
tree="$root/.claude/worktrees/$pitch-$slug"
pitch_file=$(find "$root/.crew/pitches" -maxdepth 1 -name "$pitch-*.md" | head -1)
if [[ -z "$pitch_file" ]]; then
    echo "no pitch $pitch in $root/.crew/pitches"
    exit 1
fi

if [[ "$resume" -eq 0 ]]; then
    if [[ -e "$tree" ]]; then
        echo "$tree already exists: use --resume to restart its minion"
        exit 1
    fi
    git -C "$root" fetch -q origin
    git -C "$root" worktree add -q -b "$branch" "$tree" origin/main
    prompt="You are $name. Your pitch: $pitch_file. Your worktree is this directory, on branch $branch. The \
coordinator is the session named selecta-nightman. Read CLAUDE.md, docs/CREW.md and your pitch, then build it."
else
    if [[ ! -d "$tree" ]]; then
        echo "no worktree $tree to resume"
        exit 1
    fi
    prompt="You are $name, restarted on your worktree after your session ended. Your pitch: $pitch_file; branch \
$branch. Read docs/CREW.md and your pitch, look at git status and git log to see where you were, tell the nightman \
you're back and where you stand, then carry on."
fi

cd "$tree"
claude --bg --name "$name" --agent minion --permission-mode bypassPermissions --effort "$effort" \
    --settings "{\"env\":{\"CREW_ROLE\":\"minion\",\"CLAUDE_CODE_EFFORT_LEVEL\":\"$effort\"}}" \
    --add-dir "$root/.crew" -- "$prompt"
echo "started $name in $tree"
