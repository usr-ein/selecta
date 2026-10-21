# Building Selecta

Selecta links every dependency statically (ADR-004). Dependencies come from vcpkg; Homebrew only provides tools.

## One-time setup (macOS 14.4+, Apple Silicon)

```sh
xcode-select --install                     # Apple clang and the macOS SDK
brew bundle                                # CMake, Ninja, LLVM, clazy, cppcheck, clang-format, ccache (Brewfile)
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg"            # add this to your shell profile
```

The first configure builds Qt, OpenSSL and, with media enabled, FFmpeg, LAME, soxr and TagLib from source into the
vcpkg binary cache (about 10 minutes). Later configures restore them in seconds.

## Build, test, run

```sh
cmake --workflow --preset dev              # configure + build + test (build/dev)
open build/dev/src/app/Selecta.app
```

| Preset | What for |
|---|---|
| `dev` | Debug build with tests and developer tools |
| `asan` | AddressSanitizer + UBSan |
| `lint` | clang-tidy on every compiled file |
| `release` | Optimised, no tests, no developer tools |

The audio and conversion libraries (FFmpeg, LAME, soxr, TagLib) are built by default; `-DSELECTA_WITH_MEDIA=OFF`
leaves them out for a quicker build of the rest.

## Quality checks

```sh
tools/lint.sh [paths…]                     # clang-format check, clang-tidy, clazy, cppcheck
tools/lint.sh --fix [paths…]               # apply formatting and automatic clang-tidy fixes
ctest --preset asan                        # after building the asan preset
tests/fuzz/slsk/fuzz.sh [seconds] [target…] # libFuzzer on the protocol decoders (needs `brew install llvm`)
```

## Continuous integration

`.github/workflows/ci.yml` runs on pull requests, on pushes to `main` and to `ci/**` branches, every night at 03:17 UTC
and on request (`gh workflow run CI --ref <branch>`). A push that only touches `docs/`, `references/`, `mock/` or
Markdown files doesn't start it. A newer run for the same branch and event cancels the one in progress, on `main` too:
the newer commit covers the older ones. The nightly run is never cancelled by a push.

A pull request runs on Linux runners only, and every job gates it. It should finish in about five minutes (P-028). The
jobs that take longer, or need macOS or Windows, run after the merge on `main`, or at night:

| Job | Runner | Runs on | What it checks |
|---|---|---|---|
| Linux · build, tests | `ubuntu-26.04` | everything | The `linux` preset with Clang and warnings as errors, then the tests in a network namespace that has only loopback: on a pull request, only those whose code or inputs changed; elsewhere the whole suite |
| Linux · lint 1, 2, 3 | `ubuntu-26.04` | everything | `tools/lint.sh --changed` on the files the pull request (or the push) changes, split in up to three shares of about fifteen files; the whole tree at night and on request |
| macOS · build, tests, static linking | `macos-26` | `main`, nightly | The `dev` preset with warnings as errors, the whole suite, and that the app and `selecta-cli` load only system libraries |
| macOS · lint (.mm) | `macos-26` | pushes to `main` | `tools/lint.sh` on the Objective-C++ files (`git ls-files '*.mm'`), which Linux can't compile |
| Windows · build, tests | `windows-2025` | `main`, nightly | The `windows` preset with MSVC, without the media libraries (so no engine or app), warnings not fatal |
| Linux · asan | `ubuntu-26.04` | nightly, on request | The `linux-asan` preset: the whole suite under AddressSanitizer + UBSan |

Before its pull request, a minion builds and runs the targeted tests on the Mac (docs/CREW.md): that covers the
macOS-only code a pull request's CI no longer sees (Objective-C++, Keychain, the Trash, the bundle, Apple Clang's
warnings). `main`'s run builds and tests all of it after the merge, and a red `main` gets a fix-forward minion at once.

Lint runs on Linux (maintainer, 2026-10-21), except the `.mm` files: Linux can't compile Objective-C++, so macOS lints
them on pushes to `main`. The `Q_OS_MACOS` branches of C++ files stay out of Linux's sight: the minion's own
`tools/lint.sh` on the Mac is what checks them. cppcheck doesn't read Objective-C++, so it skips `.mm` files.

Each job runs the same presets as a local build, so `cmake --workflow --preset dev`, `tools/lint.sh` and the `asan`
preset reproduce it on the Mac. Linux builds Selecta with Clang (its dependencies keep the system's GCC); upstream Clang
also flags designated initializers that leave fields to their defaults, which Apple Clang doesn't, so
`cmake/SelectaWarnings.cmake` turns that one off, and every other warning is an error on Linux too. The Linux test
step runs four tests per core: most of them wait on timers and sockets, and the suite takes less than half the time of
one per core.

**Which tests a pull request runs.** `tools/ci/affected-tests.sh` gives each test a fingerprint: its definition in
ctest, the bytes of its executable, the runtime inputs its compile definitions name (`selecta-cli` for the share and
slsk tests, the data files of `tests/meta/fixtures` and of the tests' source folders), and the script itself. A build
is byte-reproducible, so an unchanged executable means unchanged code. `main` saves the fingerprints of the tests that
passed; a pull request runs only the tests whose fingerprint no passing run on `main` or on that pull request has. A
change to `src/app` runs the UI tests only; a change to `base` runs them all. When in doubt it runs the test: a missing
fingerprint file, an input it can't hash, or a test it can't judge (a command outside the checkout).

**What a pull request's lint leaves to `main`.** `tools/lint.sh --changed <ref>` checks the files changed since `<ref>`
(committed, uncommitted and new): clang-format on all of them; the analysers on the changed sources, and on each changed
header through one source that includes it. A change can still upset a file it doesn't touch, and a change to
`.clang-tidy` or `.clang-format` alone lints nothing on the pull request: `main`'s full lint catches both. The Linux
lint's tools are the Mac's, from Homebrew (llvm, clazy, cppcheck), so both runs agree.

```sh
tools/lint.sh --changed origin/main        # what a pull request's lint checks, on the Mac
tools/lint.sh --list --changed origin/main # only the files and translation units it would check
```

**Network safety.** No job talks to the Soulseek network or to the metadata services. Every job builds with the
live-server switch off and checks it after configuring. The tests use loopback fakes, and on Linux they can't reach
anything else. CI has no secrets.

**Caches.** Each job restores vcpkg's binary archives for its triplet and a compiler cache (ccache), both kept in
`build/cache` (actions/cache refuses paths with `..`).
- The archive key is a hash of `vcpkg.json`, `cmake/vcpkg-triplets` and `cmake/vcpkg-ports`. A change to any of them,
  or a new compiler on the runner image, rebuilds the dependencies once: about 25 minutes on macOS and Linux and 35 on
  Windows, mostly Qt. A pull request that changes them takes that long once.
- A build saves only the compiler-cache entries it used, so each saved cache holds one build. `main` saves its own; a
  pull request starts from `main`'s newest and adds its own previous one, so its second push recompiles only what it
  changed since.
- How long a pull request's build takes depends on how many files its changes reach: about two minutes for changes
  across `src/app`, six for a header that nearly every file includes (`base`, `governor`), and the whole build (about
  ten) when nothing is cached, as on the first run after a toolchain change.

**Windows.** vcpkg builds every dependency with MSVC (triplet `x64-windows-selecta`: static libraries on the dynamic
CRT, about 35 minutes cold). Without the media libraries the job builds base, governor, storage, slsk, meta, match,
library, `selecta-cli` and the devnet tool, and runs their tests. What is POSIX-only by design stays behind platform
checks:
- meta's `ToolProcess` leads a process group on Unix only; on Windows a kill ends the tool alone;
- the tool tests (`ToolProcessTest`, `YtDlpRunnerTest`) script their tools in `/bin/sh`, so they build on Unix only;
- with the media libraries on, the directory walkers in collection and share (`openat`, `fdopendir`) don't build yet.

**Reading a failure.** Builds keep going after an error (`ninja -k 0`), so one run lists every compile error.

```sh
gh run list --branch <branch> --limit 5
gh run view <run-id>                       # jobs and steps
gh run view <run-id> --log-failed          # logs of the failed steps, once the whole run has finished
gh run download <run-id> -n linux-test-log # LastTest.log of a failed test step: also macos-, windows-, asan-test-log
gh run download <run-id> -n lint-logs-1    # clang-tidy.log and clazy.log of lint share 1 (macos-lint-logs: .mm)
```

## Inspecting a running Selecta

Debug builds have a local debug socket. These commands never touch your real library:

```sh
tools/dev/run-headless.sh                  # throwaway data folder, native rendering, no window on screen
tools/dev/app.sh state
tools/dev/app.sh navigate view=history
tools/dev/app.sh screenshot path=/tmp/history.png
tools/dev/app.sh tree name=sidebar depth=3
tools/dev/app.sh log lines=50
tools/dev/app.sh quit
```

## Network safety

Debug builds, tests and tools connect only to local test servers (`devenv/`, loopback and private addresses). The live
Soulseek network needs `-DSELECTA_ENABLE_LIVE_SERVER=ON` and the user's explicit choice at run time; tests refuse to
build with that option. See `CLAUDE.md`.
