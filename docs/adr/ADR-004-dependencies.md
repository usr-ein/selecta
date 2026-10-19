# ADR-004: Dependencies and toolchain

- **Status:** Accepted (revised 2026-10-20: fully static linking)
- **Date:** 2026-10-19, revised 2026-10-20
- **Inputs:** `references/qt-app-architecture.md` §1, §5–6, §9–10; RESEARCH-SYNTHESIS C9, C17; the maintainer's
  direction "linking should be full static. No dylib, or just the core macOS ones."

## Decision

- **Every dependency links statically, from vcpkg, in development and in release.**
  - The overlay triplet `cmake/vcpkg-triplets/arm64-osx-selecta.cmake` sets static libraries, release-only builds of
    dependencies and a macOS 14.4 deployment target.
  - The manifest is `vcpkg.json`, pinned by `builtin-baseline`. Optional features: `tests` (GoogleTest, Google
    Benchmark) and `media` (FFmpeg avcodec/avformat/swresample, libmp3lame, libsoxr, TagLib).
  - The vcpkg binary cache makes rebuilds fast.
  - The app may load only macOS system frameworks and `/usr/lib` system libraries (libc++, libSystem, libz, libobjc).
    Check with `otool -L`.
- **Qt is static (qtbase 6.11.2 from the vcpkg baseline).**
  - Built without Qt Quick, SQL drivers, ICU or D-Bus.
  - Plugins are linked in: `cocoa` and the `macos` style for the app; `minimal` for headless UI tests (the static
    build has no `offscreen`).
  - Move to 6.12.x when vcpkg's baseline does, then bump minors about every 6 months, because open-source LTS
    patches stop around 2027-04.
- **SQLite and GoogleTest:**
  - SQLite: Selecta's own pinned amalgamation, compiled with its own options (ADR-005).
  - GoogleTest: from vcpkg; a pinned FetchContent fallback exists for builds without vcpkg.
- **Credentials:** the macOS Security framework (`SecItem*`) directly, instead of QtKeychain, whose vcpkg port pulls
  in all of qttools.
- **Homebrew is for tools only,** listed in `Brewfile`: CMake, Ninja, clang-format, LLVM (clang-tidy, libFuzzer),
  clazy, cppcheck, ccache.
- **CMake ≥ 3.28, Ninja and `CMakePresets.json`.**
  - Presets: `dev`, `asan`, `tsan`, `lint`, `release` and `ci`, all through the vcpkg toolchain.
  - Settings: `qt_standard_project_setup(REQUIRES 6.11)`, `QT_NO_KEYWORDS`, `QT_NO_CAST_FROM_ASCII`.
  - C++20.
- **CI matrix (GitHub Actions)** runs on macOS: the static vcpkg build (cached), unit and UI tests, and the lint
  pipeline. Linux and Windows jobs come later, when someone needs them (the app targets macOS first).
- **Signing (D33):**
  - Development builds use a stable local identity, never the Developer ID.
  - Release builds use the maintainer's Developer ID, the hardened runtime and notarisation.

## Consequences

- One `.app` with no third-party dylibs to bundle, sign or update, and the same binaries in development and release.
- The first build compiles Qt and FFmpeg (about 10 minutes on Apple Silicon); later builds restore them from the
  binary cache.
- macOS 14.4 or newer.
