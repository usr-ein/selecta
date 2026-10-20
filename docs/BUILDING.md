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

Add `-DSELECTA_WITH_MEDIA=ON` to include the audio and conversion libraries.

## Quality checks

```sh
tools/lint.sh [paths…]                     # clang-format check, clang-tidy, clazy, cppcheck
tools/lint.sh --fix [paths…]               # apply formatting and automatic clang-tidy fixes
ctest --preset asan                        # after building the asan preset; likewise tsan
tests/fuzz/slsk/fuzz.sh [seconds] [target…] # libFuzzer on the protocol decoders (needs `brew install llvm`)
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
