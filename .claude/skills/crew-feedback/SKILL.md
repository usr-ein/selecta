---
name: crew-feedback
description: Improve the crew workflow (docs/CREW.md) after a snag. Use it when the maintainer runs /crew-feedback, or says the nightman, a minion or a reviewer went wrong in a way the process should prevent. It finds the cause in the crew's rules, agents, scripts or hooks, proposes the smallest change that would have prevented it, applies it once agreed, and logs it in .crew/FEEDBACK.md.
argument-hint: "<what went wrong>"
---

# /crew-feedback

The maintainer hit a snag in how the crew works: $ARGUMENTS

If that's empty, ask them in one question what went wrong and what they expected.

## 1. Get the facts

Keep it short: you need the cause, not the whole history.

- `docs/CREW.md`: the part of the protocol involved.
- The agent definitions involved: `.claude/agents/nightman.md`, `minion.md`, `bitch.md`.
- The scripts and the hook involved: `tools/crew/*.sh`, `.claude/settings.json`.
- `.crew/STATE.md`, and the last entries of `.crew/DECISIONS.md` and `.crew/FEEDBACK.md`. A snag that comes back
  means the last fix didn't work.
- **If a particular session was at fault:** `claude agents --json`, and `claude logs <name>` for its recent output.
  `claude logs` may keep streaming: cap it with `timeout 10`.

## 2. Find the cause

Say in one or two sentences which of these it was:

- a rule that was **missing**;
- a rule that was **unclear** and read the wrong way;
- a rule that was **ignored**: prose isn't enough, so it needs a hook or a script;
- a **script or hook bug**;
- a **limit** of Claude Code itself: say so, and work around it.

## 3. Propose the smallest change

- **One change, or a few,** each with the file, what changes (before → after, in a few lines), and why that would
  have prevented this snag.
- **Hard rules:** prefer a deterministic check, in `tools/crew/guard.sh` or a script, over more words in an agent
  file.
- **Keep the files short:** replace a rule rather than adding a second one beside it.
- Then ask the maintainer with AskUserQuestion: **Apply**, **Adjust** (they say how) or **Skip**.

## 4. Apply

- **Edit the files.** These are the crew's own files, so the nightman may edit them too.
- **Test** any script or hook you changed, by running it on a sample input as its header shows.
- **Commit them on `main` in the main checkout,** naming the paths:
  `git commit -m "crew: <the change>" -- <paths>`, with the `Co-Authored-By` line from `CLAUDE.md`. Then
  `git push origin main`, then `git push gitea main` (the Gitea mirror carries `main` only). Process changes are small, so they don't go through a PR.
- **Running sessions keep the instructions they started with.**
  - If the change matters to them now, tell them by SendMessage, in two lines: the nightman, the minions it
    concerns.
  - New sessions get it from `main`. Minions get it at their next `git merge origin/main`.
- **Log it at the top of `.crew/FEEDBACK.md`:**

  ```
  ## F-NNN | <yyyy-mm-dd> | <the snag, in a line>
  What happened: …
  Cause: …
  Change: <files> — <what>
  ```

## 5. Report

Tell the maintainer in five lines at most: the cause, the change, where it's logged, and who was told.
