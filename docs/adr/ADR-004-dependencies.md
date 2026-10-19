# ADR-004: Dependencies and toolchain

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** `references/qt-app-architecture.md` §1, §5–6, §9–10; RESEARCH-SYNTHESIS C9, C17

## Decision

- **Qt 6.12.x.** Bump minors about every 6 months, because open-source LTS patches stop around 2027-04.
- **CMake ≥ 3.28, Ninja and `CMakePresets.json`** with presets `dev`, `asan`, `tsan`, `release`, `fuzz` and `ci-*`.
  Settings: `qt_standard_project_setup(REQUIRES 6.12)`, `QT_ENABLE_STRICT_MODE_UP_TO=0x060C00`, `QT_NO_KEYWORDS`.
  C++20, or C++23 if the CI matrix builds it.
- **Local development:**
  - Homebrew with a `Brewfile` for Qt, FFmpeg, LAME, libsoxr, TagLib and LLVM (for fuzzing).
  - Personal builds bundle Homebrew dylibs, so they need **macOS 27** and declare it in `LSMinimumSystemVersion`.
- **CI and release:**
  - Official Qt via aqtinstall.
  - An audio-only FFmpeg, LAME and libsoxr from a pinned vcpkg baseline, targeting macOS 14.4.
  - Small CMake dependencies via FetchContent: SQLite, QtKeychain, rapidfuzz-cpp, GoogleTest, RapidCheck,
    miniupnpc, libnatpmp.
- **CI matrix (GitHub Actions):**
  - `macos-26`: build, unit tests, UI smoke tests, ASan.
  - `ubuntu-24.04`: unit tests, Docker integration tests, fuzzing, clang-tidy.
  - `windows-2025`: compile and unit tests.
- **Signing (D33):**
  - Development builds use a stable local identity, never the Developer ID.
  - Release builds use the maintainer's Developer ID, the hardened runtime and notarisation.

## Consequences

- Fast local iteration on the maintainer's Mac. Wider macOS support waits for release builds.
