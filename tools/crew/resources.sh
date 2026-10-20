#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The crew's health check, run by the nightman on every round (docs/CREW.md): disk, memory, CPU and network safety
# on this Mac. Prints one line per check, OK or WARN, then the details of what warns. Read-only.
#
#   tools/crew/resources.sh
#
# Environment: CREW_DISK_BUDGET_GB (default 10): the repo's size limit, worktrees and build folders included.
set -uo pipefail
root="$(git -C "$(dirname "$0")" rev-parse --path-format=absolute --git-common-dir)"
root="${root%/.git}"
budget_gb="${CREW_DISK_BUDGET_GB:-10}"
warns=()

# --- Disk: the repo with its worktrees and build folders, and the volume.
repo_kb=$(du -sk "$root" 2>/dev/null | cut -f1)
repo_gb=$(awk -v kb="$repo_kb" 'BEGIN { printf "%.1f", kb / 1048576 }')
free_gb=$(df -g "$root" | awk 'NR == 2 { print $4 }')
if awk -v a="$repo_gb" -v b="$budget_gb" 'BEGIN { exit !(a > b) }'; then
    echo "WARN disk: the repo takes ${repo_gb} GB (budget ${budget_gb} GB); ${free_gb} GB free"
    warns+=(disk)
elif [[ "$free_gb" -lt 20 ]]; then
    echo "WARN disk: only ${free_gb} GB free on the volume (repo ${repo_gb} GB)"
    warns+=(disk)
else
    echo "OK   disk: repo ${repo_gb} GB of ${budget_gb} GB; ${free_gb} GB free"
fi

# The compiler cache every worktree shares (cmake/SelectaCcache.cmake) lives outside the repo; ccache keeps it under
# its own max_size.
if command -v ccache >/dev/null; then
    echo "     ccache: $(ccache -s 2>/dev/null | awk -F': ' '/Cache size/ { print $2; exit }')"
fi

# --- Memory: macOS's own pressure figure and swap.
free_pct=$(memory_pressure 2>/dev/null | awk -F': ' '/free percentage/ { gsub("%", "", $2); print $2 }')
swap_used=$(sysctl -n vm.swapusage | awk '{ print $6 }')
if [[ -n "$free_pct" && "$free_pct" -lt 15 ]]; then
    echo "WARN memory: ${free_pct}% free, swap used ${swap_used}"
    warns+=(memory)
else
    echo "OK   memory: ${free_pct:-?}% free, swap used ${swap_used}"
fi

# --- CPU: the 5-minute load against the cores, and how many compiles and test runs are going.
cores=$(sysctl -n hw.ncpu)
load5=$(sysctl -n vm.loadavg | awk '{ print $3 }')
compiles=$(pgrep -f 'clang\+\+|/clang |ld64|/ld |ninja' 2>/dev/null | wc -l | tr -d ' ')
tests=$(pgrep -f 'ctest|selecta_.*_tests' 2>/dev/null | wc -l | tr -d ' ')
if awk -v l="$load5" -v c="$cores" 'BEGIN { exit !(l > c * 1.5) }'; then
    echo "WARN cpu: load ${load5} on ${cores} cores; ${compiles} compile processes, ${tests} test processes"
    warns+=(cpu)
else
    echo "OK   cpu: load ${load5} on ${cores} cores; ${compiles} compile processes, ${tests} test processes"
fi

# --- Network safety (CLAUDE.md): nothing but the maintainer's own beta app may talk to the live Soulseek network.
# The live server's addresses, and any remote 2242/2271 that isn't loopback.
live_ips=$(dig +short server.slsknet.org 2>/dev/null | grep -E '^[0-9.]+$' | tr '\n' '|' | sed 's/|$//')
connections=$(lsof -nP -iTCP -sTCP:ESTABLISHED 2>/dev/null | awk 'NR > 1' |
    grep -E "${live_ips:-208\.76\.170\.59}|->[0-9.]+:(2242|2271) " | grep -vE -- '->127\.0\.0\.1:' || true)
unexpected=""
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    pid=$(awk '{ print $2 }' <<<"$line")
    command=$(ps -o command= -p "$pid" 2>/dev/null || true)
    # The maintainer's beta, started by hand without the debug socket, is the only allowed client.
    if [[ "$command" == *"/build/beta/src/app/Selecta.app/"* && "$command" != *"--debug-server"* ]]; then
        continue
    fi
    unexpected+="${pid} ${command}"$'\n'
done <<<"$connections"
if [[ -n "$unexpected" ]]; then
    echo "WARN network: a process other than the maintainer's beta talks to the live Soulseek network:"
    printf '%s' "$unexpected" | sed 's/^/       /'
    warns+=(network)
else
    beta=$( [[ -n "$connections" ]] && echo "only the maintainer's beta is on the live network" || echo "nothing on the live network")
    echo "OK   network: ${beta}"
fi
# A debug socket on a build that can reach the live network would let a tool drive it.
if pgrep -f 'build/beta/.*--debug-server' >/dev/null 2>&1; then
    echo "WARN network: a beta build runs with --debug-server"
    warns+=(network)
fi

# --- Details for what warns.
for warn in "${warns[@]}"; do
    case "$warn" in
    disk)
        echo "--- disk, largest folders:"
        du -sh "$root"/build/* "$root"/.claude/worktrees/* "$root"/.claude/worktrees/*/build/* 2>/dev/null |
            sort -rh | head -8 | sed 's/^/     /'
        ;;
    memory)
        echo "--- memory, largest processes (MB):"
        ps -axo rss=,comm= | sort -rn | head -5 | awk '{ printf "     %6.0f %s\n", $1 / 1024, $2 }'
        ;;
    cpu)
        echo "--- cpu, busiest processes:"
        ps -axo %cpu=,comm= | sort -rn | head -5 | sed 's/^/     /'
        ;;
    esac
done
[[ ${#warns[@]} -eq 0 ]]
