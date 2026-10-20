# Using the crew

You file pitches. The **nightman** asks you questions, then runs **minions**, each building in its own worktree and
opening a PR. A **reviewer** checks each PR, and the nightman merges it. Details: `docs/CREW.md`.

## Start

```sh
tools/crew/start-nightman.sh      # once
claude attach nightman            # talk to it
claude agents                     # see every crew session
```

## File a pitch

Tell the nightman what you want, or copy `.crew/pitches/TEMPLATE.md` to `.crew/pitches/P-NNN-slug.md`, fill it in,
and tell the nightman `new pitch P-NNN`.

It asks its questions first. Nothing starts until you've answered.

## Day and night

Tell the nightman `day mode` or `night mode`.

- **Day:** minions' questions come to you.
- **Night:** the nightman decides for them and logs it. Next morning, `day mode` gets you a digest, and you mark
  each decision keep or reverse in `.crew/DECISIONS.md`.

## Where to look

| What | Where |
|---|---|
| Progress | `.crew/STATE.md`, or ask the nightman |
| PRs | `gh pr list --label crew` |
| Decisions to check | `.crew/DECISIONS.md` |
| New pitches it proposes | `.crew/pitches/`, `Status: proposed`: tell it yes or no |

## When the process goes wrong

Run `/crew-feedback <what went wrong>`, in any session. It proposes a fix to the rules and applies it once you agree.

## Stop

Tell the nightman `stop the crew`, or run `claude stop <session>` yourself.
