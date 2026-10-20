#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Formatter check and static analysers over Selecta's own C++ sources.
#
#   tools/lint.sh [--fix] [paths…]     default paths: src tests tools
#
# Runs, in order: clang-format (check, or rewrite with --fix), clang-tidy (.clang-tidy), clazy (Qt-specific checks)
# and cppcheck (returnByReference is off: Qt types are implicitly shared, so returning them by value is cheap and
# safer). Needs a configured build with compile_commands.json (SELECTA_BUILD, default build/dev) and the tools
# from the Brewfile (llvm, clazy, cppcheck). Exits non-zero on the first analyser that reports a problem.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${SELECTA_BUILD:-$root/build/dev}"
fix=0
if [[ "${1:-}" == "--fix" ]]; then fix=1; shift; fi
paths=("$@")
[[ ${#paths[@]} -eq 0 ]] && paths=(src tests tools)

llvm="$(brew --prefix llvm 2>/dev/null || true)"
clang_tidy="${CLANG_TIDY:-${llvm:+$llvm/bin/}clang-tidy}"
run_clang_tidy="${RUN_CLANG_TIDY:-${llvm:+$llvm/bin/}run-clang-tidy}"
# clazy-standalone is installed outside LLVM's prefix and can't find the compiler's builtin headers on its own.
resource_dir="$("${llvm:+$llvm/bin/}clang" -print-resource-dir)"
jobs="${LINT_JOBS:-$(getconf _NPROCESSORS_ONLN)}"

cd "$root"
mapfile -t files < <(git ls-files --cached --others --exclude-standard -- "${paths[@]}" | grep -E '\.(cpp|h|mm)$' || true)
mapfile -t sources < <(printf '%s\n' "${files[@]}" | grep -E '\.(cpp|mm)$' || true)
[[ ${#files[@]} -gt 0 ]] || { echo "no sources under: ${paths[*]}"; exit 0; }
[[ -f "$build/compile_commands.json" ]] || { echo "missing $build/compile_commands.json (configure first)" >&2; exit 2; }

echo "== clang-format (${#files[@]} files)"
if [[ $fix -eq 1 ]]; then
    clang-format -i "${files[@]}"
else
    clang-format --dry-run --Werror "${files[@]}"
fi

echo "== clang-tidy (${#sources[@]} translation units)"
tidy_args=(-quiet -j "$jobs" -p "$build" -clang-tidy-binary "$clang_tidy")
[[ $fix -eq 1 ]] && tidy_args+=(-fix)
"$run_clang_tidy" "${tidy_args[@]}" "${sources[@]/#/$root/}" > "$build/clang-tidy.log" 2>&1 || true
# Diagnostics are lines starting with a file path; progress and summary lines are not.
if grep -E '^/[^:]+:[0-9]+:[0-9]+: (warning|error):' "$build/clang-tidy.log"; then exit 1; fi

echo "== clazy"
clazy_out="$build/clazy.log"
# One translation unit per process, as run-clang-tidy does; LINT_JOBS of them at a time (default: one per core).
export build resource_dir
: > "$clazy_out"
[[ ${#sources[@]} -eq 0 ]] || printf '%s\0' "${sources[@]}" | xargs -0 -n 1 -P "$jobs" bash -c '
    clazy-standalone -p "$build/compile_commands.json" --header-filter=".*/(src|tests|tools)/.*" \
        --extra-arg="-resource-dir=$resource_dir" --extra-arg=-Wno-missing-designated-field-initializers \
        --checks=level0,level1,level2,no-qstring-allocations,no-non-pod-global-static,no-copyable-polymorphic,no-returning-void-expression \
        "$1" 2>&1 | grep --line-buffered -E "warning:|error:" || true' clazy >> "$clazy_out"
if [[ -s "$clazy_out" ]]; then cat "$clazy_out"; exit 1; fi

echo "== cppcheck"
cppcheck -j "$jobs" --project="$build/compile_commands.json" --library=qt --library=googletest --enable=warning,performance,portability --inline-suppr \
    --suppress=missingIncludeSystem --suppress=returnByReference --suppress='*:*/vcpkg_installed/*' --suppress='*:*/_deps/*' \
    --suppress='*:*_autogen/*' --error-exitcode=1 --quiet \
    $(for path in "${paths[@]}"; do printf -- "--file-filter=%s/%s/* " "$root" "$path"; done)

echo "lint: clean"
