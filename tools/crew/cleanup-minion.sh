#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A minion's last step (docs/CREW.md, step 9): once its PR is merged, removes its worktree (build folder included) and
# its local branch. Run it from the minion's worktree. Refuses while the branch isn't in origin/main, so nothing
# unmerged is ever lost; `--abandon` removes it anyway (only when the nightman says the pitch is dropped).
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
git -C "$root" fetch -q origin
if [[ "${1:-}" != "--abandon" ]] && ! git -C "$root" merge-base --is-ancestor "$branch" origin/main; then
    echo "$branch isn't merged into origin/main yet"
    exit 1
fi
cd "$root"
git worktree remove --force "$tree"
git worktree prune
git branch -D "$branch" >/dev/null
echo "removed $tree and the branch $branch"
