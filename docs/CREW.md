# The crew

How agents work on Selecta. The maintainer files **pitches**. The **nightman** coordinates; **minions** build, each
in its own worktree; a **bitch** reviews each pull request with a clean context. Every crew member is a background
Claude Code session started as its agent type (`claude --bg --agent <type>`, listed by `claude agents`). The agent
definitions are in `.claude/agents/` and the scripts in `tools/crew/`. The crew's state is in `.crew/` in the main
checkout (git-ignored).

## Roles

| Role | Does | Never |
|---|---|---|
| **Maintainer** | Files pitches; answers questions; validates new pitches; switches day and night mode; reviews `.crew/DECISIONS.md` in the morning | — |
| **nightman** (`.claude/agents/nightman.md`) | Asks the maintainer the pitch's open questions; splits work; starts minions and bitches; relays questions; checks the crew every 20 minutes; merges approved PRs and tells minions; keeps `.crew/STATE.md` | Writes code, reads diffs, pushes a branch of its own |
| **minion** (`.claude/agents/minion.md`) | One pitch (or one part of one) in its own worktree and branch: code, tests, lint, a PR labelled `review:needed`; fixes what review asks; cleans its worktree up once merged | Works outside its pitch's scope, pushes to main, merges, force-pushes |
| **bitch** (`.claude/agents/bitch.md`) | Reviews one PR against its pitch and the project rules; leaves one review on GitHub; labels it `review:approved` or `review:changes` | Edits code, pushes, merges |

Effort: the nightman always runs at `max`, bitches at `high`. A minion runs at `medium`, `high` (the default), `xhigh` or `max`: the nightman picks per pitch, and asks the maintainer when in doubt.

## A pitch's life

1. **Filed.** The maintainer writes `.crew/pitches/P-NNN-slug.md` from `.crew/pitches/TEMPLATE.md`, or tells the
   nightman, who writes it.
2. **Questions.** The nightman reads it and asks the maintainer its preliminary questions. Ambiguities, choices and
   scope go into the pitch's "Answers". Only then is the pitch `ready`. A pitch filed at night waits for the morning
   unless its questions are minor (see Night mode).
3. **Started.** The nightman splits a big pitch into parts that touch different files, or runs it as one. Each part
   gets a minion: `tools/crew/spawn-minion.sh P-NNN slug [effort]`. That makes the worktree
   `.claude/worktrees/P-NNN-slug` on branch `crew/P-NNN-slug`, from `origin/main`, and starts the session
   `selecta-minion-P-NNN-slug`.
4. **Built.** The minion works in its worktree:
   - a test for each requirement;
   - its own `build/dev`, compiling through the ccache that every worktree shares;
   - the targeted tests, then the full suite once, then `tools/lint.sh` on the directories it touched.
   - if it changes the UI: the changed screens driven in the headless app (`tools/dev/run-headless.sh`, `tools/dev/app.sh`), and screenshots committed as `docs/screens/P-NNN-<what>.png` and shown in the PR. No screenshot when the UI doesn't change.
5. **Pull request.** The minion merges `origin/main` once, then pushes the branch and opens the PR:
   `gh pr create --base main --label review:needed`. The body says what changed, the test and lint results, the
   decisions taken and what's left out. Then it tells the nightman.
6. **Review.** The nightman starts a bitch: `tools/crew/spawn-bitch.sh <PR> <round>`. The bitch reads the pitch, the
   diff and the project's rules, then leaves one review on the PR with BLOCKING and NON-BLOCKING findings. It labels
   the PR `review:approved` or `review:changes`, tells the nightman in a few lines, and stops. Every crew session is
   the maintainer's GitHub account, and GitHub doesn't let an author approve or reject their own PR. So the verdict is
   the label, and the review is a comment.
7. **Changes.** The nightman passes the blocking findings to the minion. The minion fixes them, pushes, and sets the
   label back to `review:needed`. Then a new bitch reviews round 2, and round 3 if needed.
   - After three rounds without approval, the nightman decides at night: merge with a follow-up pitch, or park.
   - By day, it asks the maintainer.
8. **Merged.** The nightman merges once the PR is labelled `review:approved` and CI's gating jobs are green
   (`tools/crew/merge-pr.sh <PR>`): a squash merge (the only kind the repo allows; GitHub deletes the head branch), then local `main` fast-forwarded, then pushed to GitHub, and `main` alone to the Gitea mirror, which carries nothing else. While CI runs, `tools/crew/wait-for-green.sh`, in the background, wakes the nightman when an approved PR is green or newly red. The nightman merges, in the order it chooses. It
   then tells every running minion "main moved". Each merges `origin/main` into its branch when it next builds.
9. **Cleaned up.** The merged minion runs `tools/crew/cleanup-minion.sh`. It removes its worktree with its build
   folder, and its local branch. The minion tells the nightman, and the nightman stops its session.

## Day and night

`.crew/MODE` holds `day` or `night`. The maintainer switches it by telling the nightman.

- **Day:** a minion's question goes to the nightman, and the nightman asks the maintainer and passes the answer on.
- **Night:** the nightman answers itself, so the minion is unblocked, and logs the decision in `.crew/DECISIONS.md`
  with how to reverse it.
  - It parks a question for the morning only when a wrong answer would cost more to undo than a night's wait. That
    means public interfaces, data formats or migrations, a new dependency, UX direction, removing a feature, or
    anything touching network safety. The pitch then waits `blocked`, and the minion stops.
  - The maintainer reads `DECISIONS.md` in the morning, marking each decision keep or reverse.

## Every 20 minutes of work

The nightman gets a check-in reminder while any minion or bitch is running. On each check-in it:

- runs `tools/crew/resources.sh` and acts on any WARN:
  - **disk:** the repo, worktrees included, stays under 10 GB; a minion's `build/dev` is about 1.5 GB;
  - **memory:** pressure and swap;
  - **CPU:** at most three full builds at once;
  - **network safety:** only the maintainer's beta may be on the live Soulseek network.
- looks at each minion: its last commits and how its diff spreads (`git -C <worktree> diff --stat origin/main`),
  compared with its pitch's areas. It doesn't read the code.
- reins in a minion whose scope grows: back to the pitch's boundary, with a follow-up pitch drafted for the
  maintainer to validate or drop.
- asks a minion that has gone quiet (no commit or message for 40 minutes) where it stands, and restarts one that died.
- updates `.crew/STATE.md`.

## Rules for everyone

- `CLAUDE.md` comes first, with its network safety rules above all:
  - never the live Soulseek server;
  - never weaken the governor;
  - never run the beta build (`build/beta`), which can reach the live network;
  - never start any build with `--debug-server` while it can reach the live network.
- Minions and bitches never push to `main`, merge, force-push or rewrite history. `tools/crew/guard.sh` blocks it.
  Only the nightman merges, and only through `tools/crew/merge-pr.sh`.
- One minion per pitch part, and the parts touch different files. A bug a minion finds that affects others goes to
  the nightman, who decides who fixes it and in what order.
- **Messages:**
  - minion → nightman: 10 lines at most;
  - bitch → nightman: 5 lines;
  - the details go in the PR.
- The nightman's context is for direction and progress: the board, pitches, short reports. Never code or diffs.
- **Snags in the process:** run `/crew-feedback <what went wrong>`. It proposes a change to these rules or to the
  agents and logs it in `.crew/FEEDBACK.md`.

## Files

| Path | What | Kept by |
|---|---|---|
| `.crew/MODE` | `day` or `night` | maintainer (via the nightman) |
| `.crew/STATE.md` | the board: pitches, minions, PRs, rounds, what's next | nightman |
| `.crew/pitches/P-NNN-slug.md` | one pitch, with its answers and status | maintainer, nightman |
| `.crew/DECISIONS.md` | decisions taken at night, and open questions | nightman; the maintainer marks them |
| `.crew/FEEDBACK.md` | snags and the process changes made for them | `/crew-feedback` |
| `tools/crew/*.sh` | `start-nightman`, `spawn-minion`, `spawn-bitch`, `merge-pr`, `wait-for-green`, `cleanup-minion`, `resources`, `guard` | committed |
