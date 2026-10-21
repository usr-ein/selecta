#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The nightman's merge (docs/CREW.md, step 8): merges one crew PR once review approved it and CI's gating jobs are
# green, then brings the main checkout's `main` up to date and pushes it to GitHub, then to the Gitea mirror (main only).
# Refuses, saying why, when the PR isn't ready.
#
#   tools/crew/merge-pr.sh <PR number>
set -euo pipefail
pr="${1:?usage: merge-pr.sh <PR number>}"
root="$(git -C "$(dirname "$0")" rev-parse --path-format=absolute --git-common-dir)"
root="${root%/.git}"
# Only the repo's owner may merge. gh may have another of the maintainer's accounts active, so this script uses the
# owner's login for its own commands and leaves the active account alone.
owner=$(gh repo view --json owner --jq .owner.login)
if ! GH_TOKEN=$(gh auth token --user "$owner" 2>/dev/null); then
    echo "gh isn't logged in as $owner, the repo's owner: gh auth login"
    exit 1
fi
export GH_TOKEN

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
# CI: only the Linux jobs gate a PR; macOS and Windows are for main (maintainer, 2026-10-21: "I only want linux
# runners blocking the PR. Other platforms are for main").
checks=$(gh pr checks "$pr" --json name,bucket --jq '.[] | select(.name | test("^Linux"; "i")) | "\(.bucket) \(.name)"' || true)
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
# The Gitea mirror carries main only, no crew branches and no PRs (maintainer, 2026-10-21).
if git -C "$root" remote get-url gitea >/dev/null 2>&1; then
    git -C "$root" push -q gitea main
fi
echo "merged PR #$pr; main is $(git -C "$root" log -1 --format='%h %s' main)"
