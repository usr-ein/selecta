# Testing a beta build on the live network

For the maintainer's own sessions (CLAUDE.md: live-network sessions are run by hand, never automated). The beta build
is optimised, allows the live network once you switch it on in Settings, and records what the developer needs to fix
what you run into. Nothing it records leaves the Mac.

## Build and open

```sh
export VCPKG_ROOT="$HOME/vcpkg"
cmake --preset beta && cmake --build --preset beta
open build/beta/src/app/Selecta.app
```

It uses your normal data (`~/Library/Application Support/Selecta`), logs (`~/Library/Logs/Selecta`) and caches, the
Keychain for passwords and the Spotify sign-in, the real Trash, and UPnP/NAT-PMP on your router. Your collection folders
are only ever read.

## First run: the pre-flight (PLAN §5.5)

1. **Settings → Soulseek:** tick **Use the live Soulseek network**. It fills in `server.slsknet.org`, port 2242. Then
   enter your username and password, and press **Save**. A dedicated account is the safest.
2. **Settings → Library:** set the Inbox and your collection folders. Check the scan in **Collection**.
3. **Settings → Spotify** (optional): your own Client ID and secret, then **Sign In in the Browser**. Then the yt-dlp
   path, if you want YouTube previews.
4. **Activity → Log:** the governor's lines are there. Every search, request and refusal has a reason.
5. One search by hand, then one download. Watch **Activity** for denials or bans before doing more.

## While you use it

- **Something wrong? Press ⌘⇧N (Help → Note a Problem…).** Say what happened and what you expected. The note is saved
  with a picture of the window as it was before the note opened, what Selecta was doing, and the end of its log.
- The log keeps:
  - a trail of what you did: the controls you used, never what you typed;
  - any moment Selecta stopped answering for more than a quarter of a second. A long freeze also gets a stack sample
    (`stalls/`).
- After a crash, macOS keeps a report. Just reopen Selecta and carry on.
- **Help → Show Logs in Finder** opens the folder.

## When you're done (or at any time)

Tell the developer. They gather everything with:

```sh
tools/dev/collect-feedback.sh [since yyyy-mm-dd]      # into local/feedback/<time>/
```

That copies the logs, the notes with their screenshots, the stall samples, the crash reports, the activity log and
`settings.ini`. Passwords and tokens stay in the Keychain.

**Don't start the beta with `--debug-server`** during a live session: a tool could then drive it.
