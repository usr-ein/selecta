# devenv: a real Soulseek server on this machine

Selecta never talks to the live Soulseek network during development (CLAUDE.md). Two local stand-ins replace it:

- **`selecta-devnet`** (`tools/devnet/`): an in-process fake server and fake peers on 127.0.0.1, with a generated
  corpus. The everyday choice for manual runs of the app.
- **Soulfind** (this folder): the open-source Soulseek server (GPL-3.0-or-later, `soulfind-dev/soulfind`), the one
  other client projects test against. It runs the real server code, so it catches what the fakes model wrongly. It
  has no distributed network (those paths are tested with the fakes).

## Safety

- Clients connect to Soulfind at **127.0.0.1** only. Nothing here knows the live server's address.
- **Soulfind listens on every interface**: it has no option to bind one address. On a network you don't trust, turn
  on the macOS firewall's "Block all incoming connections" (System Settings > Network > Firewall > Options) while it
  runs; loopback connections still work.
- Soulfind tells clients on 127.0.0.1 each other's **LAN address** (it can't know they share the machine). Selecta
  reaches a peer at one of this machine's own addresses over loopback, so its clients can listen on 127.0.0.1 alone
  (the devenv tests do). Other clients (reference peers) need to listen on every interface, and the macOS firewall
  may drop connections to programs it doesn't know.
- The scripts download from GitHub only (`fetch-soulfind.sh`), and check the pinned SHA-256 before keeping anything.

## Quick start

```sh
devenv/fetch-soulfind.sh                         # once: the pinned build, into devenv/.cache/ (git-ignored)
devenv/run-soulfind.sh --all-interfaces 2242     # a server on port 2242, throwaway database removed on Ctrl-C
```

`run-soulfind.sh` refuses to start without `--all-interfaces`: the flag says you know Soulfind will be reachable from
your network (see Safety).

On macOS Soulfind needs Homebrew's SQLite (`brew install sqlite`); with the system one it stops at start-up with
"SQLite error 7". `run-soulfind.sh` and the tests pick Homebrew's up on their own.

Then point Selecta (Settings > server), `selecta-cli` or a reference peer at `127.0.0.1:2242`. Accounts don't need
creating: Soulfind registers a name with its password at the first login, and refuses other passwords after that.
`soulsetup -d <database>` (next to `soulfind` in the cache) makes admins and server-side search filters.

## Driving it from the command line

`selecta-cli` has slsk commands that only ever talk to a local server (they refuse public addresses and never resolve
host names), with permits from the real governor:

```sh
selecta-cli slsk-share --server 127.0.0.1:2242 --user bob --dir ~/devnet-corpus/cbr_carl   # answer and upload
selecta-cli slsk-search --server 127.0.0.1:2242 --user alice --wait 5 saffron tide         # one line per result
selecta-cli slsk-browse --server 127.0.0.1:2242 --user alice bob
selecta-cli slsk-download --server 127.0.0.1:2242 --user alice bob 'cbr_carl\…\01-….mp3' --to track.mp3
selecta-cli slsk-login --server 127.0.0.1:2242 --user alice --stay 60
```

`slsk-share --advertise-port <a closed port>` plays a peer that can't be reached directly, so downloads from it go
through the server's indirect connection. They work against `selecta-devnet` the same way.

## The devenv tests

`tests/slsk/devenv/` holds integration tests with the CTest label `devenv`. They are opt-in twice: built only with
the CMake option `SELECTA_SOULFIND_TESTS=ON` (default off, so the default suite only uses `selecta-devnet` and the
fixtures, which bind 127.0.0.1), and run only with `SELECTA_DEVENV=1`. Nothing downloads Soulfind for them:

```sh
devenv/fetch-soulfind.sh
cmake --preset dev -DSELECTA_SOULFIND_TESTS=ON && cmake --build --preset dev
SELECTA_DEVENV=1 ctest --preset dev -L devenv
```

Each test starts its own Soulfind on a free port with a database in a temporary folder and stops it afterwards
(ADR-013). They cover login, relogin and a wrong password, two client stacks finding and downloading a file over the
direct path and over the indirect one (the uploader advertises a closed port, so the downloader's direct connection
fails and the uploader dials back through the server), and the same with two `selecta-cli` processes (`slsk-share`
against `slsk-search` and `slsk-download`). `SELECTA_SOULFIND=/path/to/soulfind` uses another
build. A machine with no network at all may fail them: Soulfind looks up the LAN address through the routing table.

## Updating Soulfind

Upstream publishes rolling builds under the tag `latest`; `soulfind.pin` records the commit, each platform's GitHub
asset id, and its SHA-256. When upstream replaces the build, the old asset id stops resolving and
`fetch-soulfind.sh` fails instead of fetching something else. To move the pin:

1. Read the release: `curl -s https://api.github.com/repos/soulfind-dev/soulfind/releases/tags/latest`. Note
   `target_commitish`, `published_at`, and for each `soulfind-<platform>.zip` asset its `id` and `digest`.
2. Look at upstream's changes since the pinned commit.
3. Edit `soulfind.pin`, run `devenv/fetch-soulfind.sh` (it checks the new digest), then the devenv tests.

## Reference peers (optional)

Other clients on the same server show what Selecta does differently. They aren't needed by any test. **Both default
to the live network: write their configuration before their first launch.**

- **Nicotine+** (`pip install nicotine-plus` in a virtual environment), headless. Its configuration file
  (`nicotine --headless --config <dir>/config --user-data <dir>`) needs, at least:

  ```ini
  [server]
  server = ('127.0.0.1', 2242)
  login = 'np-peer'
  passw = 'test'
  portrange = (2240, 2240)
  upnp = False
  auto_connect_startup = True
  [transfers]
  shared = [('Synthetic', '/absolute/path/to/a/synthetic/corpus')]
  ```

- **slskd** (a native release), driven through its web UI and API on 127.0.0.1:5030:

  ```sh
  SLSKD_SLSK_ADDRESS=127.0.0.1 SLSKD_SLSK_PORT=2242 SLSKD_SLSK_USERNAME=slskd-peer SLSKD_SLSK_PASSWORD=test \
  SLSKD_SLSK_LISTEN_PORT=50300 SLSKD_SHARED_DIR=/absolute/path/to/a/synthetic/corpus SLSKD_NO_AUTH=true \
  SLSKD_NO_VERSION_CHECK=true SLSKD_HTTP_PORT=5030 ./slskd
  ```

Share synthetic files only (`selecta-devnet --generate <dir>` writes some), never real music.
