#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A minion's last step (docs/CREW.md, step 9): once its PR is merged, removes its worktree (build folder included) and
# its local branch. Run it from the minion's worktree. PRs are squash-merged, so the branch's own commits never reach
# main: it refuses unless the branch's PR is merged with exactly this HEAD and nothing is left uncommitted, so nothing
# unmerged is ever lost. `--abandon` removes it anyway (only when the nightman says the pitch is dropped).
#
#   tools/crew/cleanup-minion.sh [--abandon]
set -euo pipefail
tree="$(git rev-parse --show-toplevel)"
branch="$(git branch --show-current)"
root="$(git rev-parse --path-format=absolute --git-common-dir)"
root="${root%/.git}"
if [[ "$tree" == "$root" ]]; then
    echo "this is the main checkout, not a minion's worktree"
    exit 1
fi
if [[ "$branch" != crew/* ]]; then
    echo "branch $branch isn't a crew branch"
    exit 1
fi
if [[ "${1:-}" != "--abandon" ]]; then
    merged_head="$(gh pr list --head "$branch" --state merged --json headRefOid --jq '.[0].headRefOid // empty')"
    if [[ -z "$merged_head" ]]; then
        echo "$branch has no merged PR yet"
        exit 1
    fi
    if [[ "$(git rev-parse HEAD)" != "$merged_head" ]]; then
        echo "$branch has commits its merged PR doesn't: tell the nightman before removing anything"
        exit 1
    fi
    if [[ -n "$(git status --porcelain)" ]]; then
        echo "$tree has uncommitted changes: tell the nightman before removing anything"
        exit 1
    fi
fi
cd "$root"
git worktree remove --force "$tree"
git worktree prune
git branch -D "$branch" >/dev/null
echo "removed $tree and the branch $branch"
