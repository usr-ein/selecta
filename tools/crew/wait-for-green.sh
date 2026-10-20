#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Wakes the nightman when an approved crew PR can be merged (docs/CREW.md, step 8). The nightman runs it in the
# background (Bash run_in_background) and is woken when it exits. It checks every minute, for at most 2 hours, and exits
# as soon as an approved PR's gating jobs have all passed, or one of them has newly failed. It prints every approved PR
# as "ready", "red: <jobs>" or "pending". It never merges: the nightman does, with merge-pr.sh, in the order it chooses.
#
#   tools/crew/wait-for-green.sh
set -euo pipefail
# Reads as the repo's owner, like merge-pr.sh, whatever gh account is active.
owner=$(gh repo view --json owner --jq .owner.login)
if ! GH_TOKEN=$(gh auth token --user "$owner" 2>/dev/null); then
    echo "gh isn't logged in as $owner, the repo's owner: gh auth login"
    exit 1
fi
export GH_TOKEN

# The gating jobs: all but the experimental ones (Windows, and TSan), as in merge-pr.sh.
gating() {
    gh pr checks "$1" --json name,bucket \
        --jq '.[] | select(.name | test("Windows|tsan"; "i") | not) | "\(.bucket) \(.name)"' 2>/dev/null || true
}

deadline=$((SECONDS + 7200))
red_at_start=" " # PRs already red at the start: the nightman knows them. They count again once CI reruns.
first=1
while :; do
    prs=$(gh pr list --label crew --label review:approved --state open --json number --jq '.[].number')
    if [[ -z "$prs" ]]; then
        echo "no approved crew PR to wait for"
        exit 0
    fi
    report=""
    wake=0
    for pr in $prs; do
        checks=$(gating "$pr")
        if [[ -z "$checks" ]] || grep -q '^pending ' <<<"$checks"; then
            report+="#$pr pending"$'\n'
            red_at_start=${red_at_start/ $pr / }
        elif grep -qvE '^(pass|skipping) ' <<<"$checks"; then
            report+="#$pr red: $(grep -vE '^(pass|skipping) ' <<<"$checks" | cut -d' ' -f2- | paste -s -d, -)"$'\n'
            if ((first)); then
                red_at_start+="$pr "
            elif [[ "$red_at_start" != *" $pr "* ]]; then
                wake=1
            fi
        else
            report+="#$pr ready"$'\n'
            wake=1
        fi
    done
    first=0
    if ((wake)); then
        printf '%s' "$report"
        exit 0
    fi
    if ((SECONDS > deadline)); then
        printf 'no change in 2 hours:\n%s' "$report"
        exit 0
    fi
    sleep 60
done
