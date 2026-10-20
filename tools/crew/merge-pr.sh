#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The nightman's merge (docs/CREW.md, step 8): merges one crew PR once review approved it and CI's gating jobs are
# green, then brings the main checkout's `main` up to date and pushes it to every remote (GitHub and the mirror).
# Refuses, saying why, when the PR isn't ready.
#
#   tools/crew/merge-pr.sh <PR number>
set -euo pipefail
pr="${1:?usage: merge-pr.sh <PR number>}"
root="$(git -C "$(dirname "$0")" rev-parse --path-format=absolute --git-common-dir)"
root="${root%/.git}"

labels=$(gh pr view "$pr" --json labels --jq '[.labels[].name] | join(",")')
state=$(gh pr view "$pr" --json state --jq .state)
if [[ "$state" != "OPEN" ]]; then
    echo "PR #$pr is $state"
    exit 1
fi
if [[ ",$labels," != *",review:approved,"* ]]; then
    echo "PR #$pr isn't approved (labels: ${labels:-none})"
    exit 1
fi
# CI: every job but the experimental ones (Windows, and TSan, which reports without failing) must have passed.
checks=$(gh pr checks "$pr" --json name,bucket --jq '.[] | select(.name | test("Windows|tsan"; "i") | not) | "\(.bucket) \(.name)"' || true)
if [[ -z "$checks" ]]; then
    echo "PR #$pr has no CI checks yet"
    exit 1
fi
if grep -qvE '^(pass|skipping) ' <<<"$checks"; then
    echo "PR #$pr isn't green yet:"
    grep -vE '^(pass|skipping) ' <<<"$checks" | sed 's/^/  /'
    exit 1
fi

# Squash only: the repo allows nothing else, and GitHub deletes the head branch itself. The minion's local branch goes
# with its worktree (cleanup-minion.sh).
gh pr merge "$pr" --squash
# The main checkout stays on a clean main: the nightman never edits code there.
if [[ "$(git -C "$root" branch --show-current)" != "main" ]] || [[ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]]; then
    echo "merged on GitHub; the main checkout isn't a clean main, so it wasn't updated"
    exit 2
fi
git -C "$root" fetch -q origin
git -C "$root" merge -q --ff-only origin/main
git -C "$root" push -q origin main
echo "merged PR #$pr; main is $(git -C "$root" log -1 --format='%h %s' main)"
