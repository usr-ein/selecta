#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts the reviewer for one round of one crew PR (docs/CREW.md, step 6). It runs as the background session
# bitch-<PR>-r<round>, as the `bitch` agent, in the main checkout. It reads the PR on GitHub and the minion's
# worktree, and changes nothing.
#
#   tools/crew/spawn-bitch.sh <PR number> [round: 1 (default), 2 or 3]
set -euo pipefail
pr="${1:?usage: spawn-bitch.sh <PR number> [round]}"
round="${2:-1}"
root="$(git -C "$(dirname "$0")" rev-parse --path-format=absolute --git-common-dir)"
root="${root%/.git}"
branch=$(gh pr view "$pr" --json headRefName --jq .headRefName)
if [[ "$branch" != crew/* ]]; then
    echo "PR #$pr comes from $branch, not a crew branch"
    exit 1
fi
part="${branch#crew/}"            # P-NNN-slug
pitch=$(cut -d- -f1-2 <<<"$part") # P-NNN
pitch_file=$(find "$root/.crew/pitches" -maxdepth 1 -name "$pitch-*.md" | head -1)
name="bitch-$pr-r$round"

cd "$root"
git fetch -q origin "$branch"
claude --bg --name "$name" --agent bitch --permission-mode bypassPermissions --effort high \
    --settings '{"env":{"CREW_ROLE":"bitch","CLAUDE_CODE_EFFORT_LEVEL":"high"}}' \
    --add-dir "$root/.crew" \
    "Review PR #$pr, round $round. Pitch: ${pitch_file:-unknown (ask the nightman)}. Branch: $branch. The minion's \
worktree, read only: $root/.claude/worktrees/$part. The coordinator is the session named nightman. Follow your \
instructions, then stop."
echo "started $name"
