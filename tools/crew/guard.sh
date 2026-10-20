#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# PreToolUse hook (.claude/settings.json): the crew's hard rules (docs/CREW.md), enforced whatever an agent decides.
# Reads the tool call as JSON on stdin; exits 2 with the reason on stderr to block it, 0 to let it run.
#
# For every session in this repo:
#   - no force-push;
#   - no running the beta build, which can reach the live Soulseek network (CLAUDE.md). The maintainer runs it by hand.
# By crew role ($CREW_ROLE, set by tools/crew/*.sh when they start a session):
#   - minion: no push to main, no merge of a PR;
#   - minion: edits only inside its own worktree;
#   - bitch: reviews only, so no push, commit, merge or rebase, and no file edits;
#   - nightman: edits only the crew's own files, never code.
set -uo pipefail
input="$(cat)"
role="${CREW_ROLE:-}"

verdict=$(CREW_INPUT="$input" CREW_ROLE="$role" python3 -I - <<'PY'
import json, os, re, sys

data = json.loads(os.environ.get("CREW_INPUT") or "{}")
role = os.environ.get("CREW_ROLE", "")
tool = data.get("tool_name", "")
args = data.get("tool_input", {}) or {}

def block(reason):
    print(reason)
    sys.exit(0)

if tool == "Bash":
    command = args.get("command", "")
    flat = " ".join(command.split())
    pushes = re.findall(r"\bgit\s+push\b[^;&|]*", flat)
    for push in pushes:
        if re.search(r"(\s--force(-with-lease)?\b|\s-f\b|\s\+\S)", push):
            block("Force-pushing is never allowed (docs/CREW.md).")
    if re.search(r"build/beta\b", flat) and re.search(
            r"(\bopen\b|/Contents/MacOS/Selecta|run-headless|--debug-server|SELECTA_BUILD=)", flat):
        block("The beta build can reach the live Soulseek network: only the maintainer runs it, by hand (CLAUDE.md).")
    if role == "minion":
        if re.search(r"\bgh\s+pr\s+merge\b", flat):
            block("Minions don't merge PRs: tell the nightman the PR is ready (docs/CREW.md).")
        for push in pushes:
            if re.search(r"(\s|:)main(\s|$)", push + " "):
                block("Minions never push to main: push your crew/ branch and open a PR (docs/CREW.md).")
    if role == "bitch":
        if pushes or re.search(r"\bgit\s+(commit|merge|rebase|reset|cherry-pick)\b", flat) or \
                re.search(r"\bgh\s+pr\s+(merge|close|create)\b", flat):
            block("Reviewers only read and review: no commits, pushes or merges (docs/CREW.md).")

if tool in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
    path = os.path.realpath(args.get("file_path") or args.get("notebook_path") or "")
    scratch = path.startswith(("/tmp/", "/private/tmp/", "/private/var/folders/", "/var/folders/"))
    if role == "nightman":
        allowed = ("/.crew/", "/docs/CREW.md", "/.claude/agents/", "/.claude/skills/", "/tools/crew/")
        if not scratch and not any(part in path for part in allowed):
            block("The nightman doesn't write code: give the change to a minion (docs/CREW.md). "
                  "It edits only .crew/, docs/CREW.md, .claude/agents/, .claude/skills/ and tools/crew/.")
    if role == "bitch" and not scratch:
        block("Reviewers don't edit files: put the finding in the review (docs/CREW.md).")
    if role == "minion" and not scratch:
        # Only inside its own worktree: the main checkout and the other minions' worktrees are off limits.
        match = re.search(r"^(.*/\.claude/worktrees/[^/]+)(/|$)", os.path.realpath(data.get("cwd", "")))
        if not match or not (path == match.group(1) or path.startswith(match.group(1) + "/")):
            block("Minions edit files only inside their own worktree (docs/CREW.md).")
PY
)
if [[ -n "$verdict" ]]; then
    echo "$verdict" >&2
    exit 2
fi
exit 0
