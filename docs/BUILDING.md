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
| `tsan` | ThreadSanitizer |
| `lint` | clang-tidy on every compiled file |
| `release` | Optimised, no tests, no developer tools |

The audio and conversion libraries (FFmpeg, LAME, soxr, TagLib) are built by default; `-DSELECTA_WITH_MEDIA=OFF`
leaves them out for a quicker build of the rest.

## Quality checks

```sh
tools/lint.sh [paths…]                     # clang-format check, clang-tidy, clazy, cppcheck
tools/lint.sh --fix [paths…]               # apply formatting and automatic clang-tidy fixes
ctest --preset asan                        # after building the asan preset; likewise tsan
tests/fuzz/slsk/fuzz.sh [seconds] [target…] # libFuzzer on the protocol decoders (needs `brew install llvm`)
```

## Continuous integration

`.github/workflows/ci.yml` runs on pushes to `main` and to `ci/**` branches, on pull requests, every night at 03:17 UTC
and on request (`gh workflow run CI --ref <branch>`). A push that only touches `docs/`, `references/`, `mock/` or
Markdown files doesn't start it. A newer push to the same branch cancels the run in progress, except on `main`.

| Job | Runner | What it checks |
|---|---|---|
| macOS · build, tests, static linking | `macos-26` | The `dev` preset with warnings as errors, the whole suite, and that the app and `selecta-cli` load only system libraries |
| macOS · lint | `macos-26` | `tools/lint.sh` over `src`, `tests` and `tools`. It needs the generated sources only, not a build. About 50 minutes, most of it clang-tidy |
| macOS · asan | `macos-26` | The whole suite under AddressSanitizer + UBSan. Not on pull requests |
| Linux · build, tests | `ubuntu-26.04` | The `linux` preset with GCC and warnings as errors, then the suite in a network namespace that has only loopback |
| Windows · build, tests | `windows-2025` | The `windows` preset with MSVC, without the media libraries (so no engine or app), warnings not fatal |

Each job runs the same presets as a local build, so `cmake --workflow --preset dev`, `tools/lint.sh` and the `asan`
preset reproduce it. GCC leaves `-Wshadow` and `-Wmissing-field-initializers` to Clang
(`cmake/SelectaWarnings.cmake`), and every other warning is an error on Linux too.

**Network safety.** No job talks to the Soulseek network or to the metadata services. Every job builds with the
live-server switch off and checks it after configuring. The tests use loopback fakes, and on Linux they can't reach
anything else. CI has no secrets.

**Caches.** Each job restores vcpkg's binary archives for its triplet and its own ccache folder, both kept in
`build/cache` (actions/cache refuses paths with `..`). The archive key is a hash of `vcpkg.json`, `cmake/vcpkg-triplets`
and `cmake/vcpkg-ports`. A change to any of them, or a new compiler on the runner image, rebuilds the dependencies once:
about 25 minutes on macOS and Linux and 35 on Windows, mostly Qt. The macOS, Linux and Windows build jobs save the new
archives. Lint and the sanitizer job only restore them.

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
gh run download <run-id> -n macos-test-log # LastTest.log of a failed test step: also linux-, windows-, asan-test-log
gh run download <run-id> -n lint-logs      # clang-tidy.log and clazy.log
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
