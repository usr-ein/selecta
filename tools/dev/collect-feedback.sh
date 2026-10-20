#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Gathers what a tester's session left behind (src/app/Diagnostics.h) into one folder for the developer:
#   - the logs, with the action trail and the stalls;
#   - the problem notes (Help → Note a Problem…), with their screenshots, state and log tails;
#   - the stack samples of long stalls;
#   - Selecta's crash reports;
#   - the activity log (every request and the governor's answer) from the database;
#   - settings.ini.
# It only copies: the tester's files stay where they are. Passwords and tokens are in the Keychain, never in these.
#
#   tools/dev/collect-feedback.sh [since yyyy-mm-dd (default: today)] [output folder]
#
# Environment: SELECTA_LOGS, SELECTA_DATA to read another log or data folder (an isolated run's).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
since="${1:-$(date +%Y-%m-%d)}"
out="${2:-$root/local/feedback/$(date +%Y-%m-%d_%H-%M-%S)}"
logs="${SELECTA_LOGS:-$HOME/Library/Logs/Selecta}"
data="${SELECTA_DATA:-$HOME/Library/Application Support/Selecta}"

mkdir -p "$out/crashes"
cp -p "$logs"/selecta*.log "$out/" 2>/dev/null || true
for folder in feedback stalls; do
    if [[ -d "$logs/$folder" ]]; then
        cp -Rp "$logs/$folder" "$out/"
    fi
done
find "$HOME/Library/Logs/DiagnosticReports" -maxdepth 1 -name 'Selecta*' -newermt "$since" \
    -exec cp -p {} "$out/crashes/" \; 2>/dev/null || true
if [[ -f "$data/settings.ini" ]]; then
    cp -p "$data/settings.ini" "$out/"
fi
if [[ -f "$data/selecta.sqlite3" ]] && command -v sqlite3 >/dev/null; then
    since_ms=$(($(date -j -f '%Y-%m-%d %H:%M:%S' "$since 00:00:00" +%s) * 1000))
    sqlite3 -readonly -header -separator $'\t' "file:$data/selecta.sqlite3?mode=ro" \
        "SELECT datetime(at / 1000, 'unixepoch', 'localtime') AS time, operation, subject, outcome, explanation
         FROM activity WHERE at >= $since_ms ORDER BY id;" >"$out/activity.tsv" || echo "couldn't read the activity log" >&2
fi

echo "$out"
(cd "$out" && find . -type f | sort | head -100)
