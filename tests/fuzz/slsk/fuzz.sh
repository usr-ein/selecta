#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the slsk fuzz configuration with Homebrew LLVM and runs each libFuzzer target.
#
#   tests/fuzz/slsk/fuzz.sh [seconds-per-target] [target…]     targets: server peer_init peer distrib zlib
#
# Defaults: 60 s per target, every target. Needs VCPKG_ROOT and `brew install llvm`. The build lives in
# build/fuzz-slsk (SELECTA_FUZZ_BUILD overrides it); the corpus grows in build/fuzz-slsk/fuzz-corpus/<target>/ and
# crashing inputs land in build/fuzz-slsk/fuzz-artifacts/<target>/. Exits non-zero if any target finds a crash.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
build="${SELECTA_FUZZ_BUILD:-$root/build/fuzz-slsk}"
seconds="${1:-60}"
[[ $# -gt 0 ]] && shift
targets=("$@")
[[ ${#targets[@]} -eq 0 ]] && targets=(server peer_init peer distrib zlib)
: "${VCPKG_ROOT:?set VCPKG_ROOT to your vcpkg clone}"
llvm="$(brew --prefix llvm)"
[[ -x "$llvm/bin/clang++" ]] || { echo "Homebrew LLVM not found: brew install llvm" >&2; exit 2; }

if [[ ! -f "$build/build.ninja" ]]; then
    # Same dependencies as the dev preset, compiled with LLVM clang plus AddressSanitizer and UBSan. Warnings are not
    # errors here: this build gates nothing, and a newer clang may warn where Apple clang doesn't.
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET=arm64-osx-selecta -DVCPKG_HOST_TRIPLET=arm64-osx-selecta \
        -DVCPKG_OVERLAY_TRIPLETS="$root/cmake/vcpkg-triplets" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4 -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER="$llvm/bin/clang" -DCMAKE_CXX_COMPILER="$llvm/bin/clang++" \
        -DSELECTA_SANITIZE="address;undefined" -DSELECTA_SLSK_FUZZ=ON -DSELECTA_WARNINGS_AS_ERRORS=OFF \
        -DSELECTA_DEVTOOLS=OFF
fi
cmake --build "$build" --target slsk_fuzz_seeds "${targets[@]/#/slsk_fuzz_}"

corpus="$build/fuzz-corpus"
artifacts="$build/fuzz-artifacts"
"$build/tests/fuzz/slsk/slsk_fuzz_seeds" "$corpus"

status=0
for target in "${targets[@]}"; do
    mkdir -p "$corpus/$target" "$artifacts/$target"
    echo "== slsk_fuzz_$target: ${seconds}s"
    if ! "$build/tests/fuzz/slsk/slsk_fuzz_$target" -max_total_time="$seconds" -rss_limit_mb=2048 -timeout=10 \
        -print_final_stats=1 -artifact_prefix="$artifacts/$target/" "$corpus/$target"; then
        echo "slsk_fuzz_$target found a problem; inputs in $artifacts/$target/" >&2
        status=1
    fi
done
exit $status
