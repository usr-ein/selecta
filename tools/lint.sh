#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Formatter check and static analysers over Selecta's own C++ sources.
#
#   tools/lint.sh [--fix] [paths…]                    default paths: src tests tools
#   tools/lint.sh [--fix] --changed <ref> [paths…]    only what changed since <ref> (pull requests in CI)
#   tools/lint.sh … --shard <k>/<n> …                  the k-th of n shares of the translation units (parallel CI jobs)
#   tools/lint.sh … --list …                           print the files and translation units it would check, and stop
#
# Runs, in order: clang-format (check, or rewrite with --fix), clang-tidy (.clang-tidy), clazy (Qt-specific checks)
# and cppcheck (returnByReference is off: Qt types are implicitly shared, so returning them by value is cheap and
# safer). Needs a configured build with compile_commands.json (SELECTA_BUILD, default build/dev) and the tools
# from the Brewfile (llvm, clazy, cppcheck). Exits non-zero on the first analyser that reports a problem.
#
# --changed takes the files changed since the merge base of <ref> and HEAD: committed, uncommitted and new ones.
# clang-format checks them all. The analysers check the changed sources, and each changed header through one source
# that includes it: its own .cpp when this build compiles one, otherwise the nearest source that includes it, directly or
# through other headers. A change can still upset a file it doesn't touch: the full lint on main catches that.
#
# --shard splits the translation units into n shares of about the same cost, by size (a test costs about three times
# as much per byte: gtest's macros); clang-format runs in the first share only. The n shares check what one run would.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${SELECTA_BUILD:-$root/build/dev}"
fix=0
changed=""
shard_index=1
shard_count=1
list=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fix) fix=1; shift ;;
        --list) list=1; shift ;;
        --changed)
            [[ $# -ge 2 && -n "$2" ]] || { echo "lint: --changed needs a git ref" >&2; exit 2; }
            changed="$2"; shift 2 ;;
        --shard)
            if [[ $# -lt 2 || ! "$2" =~ ^([1-9][0-9]*)/([1-9][0-9]*)$ ]] || ((BASH_REMATCH[1] > BASH_REMATCH[2])); then
                echo "lint: --shard needs <k>/<n>, 1 <= k <= n" >&2
                exit 2
            fi
            shard_index="${BASH_REMATCH[1]}"; shard_count="${BASH_REMATCH[2]}"; shift 2 ;;
        *) break ;;
    esac
done
paths=("$@")
[[ ${#paths[@]} -eq 0 ]] && paths=(src tests tools)

llvm="$(brew --prefix llvm 2>/dev/null || true)"
clang_tidy="${CLANG_TIDY:-${llvm:+$llvm/bin/}clang-tidy}"
run_clang_tidy="${RUN_CLANG_TIDY:-${llvm:+$llvm/bin/}run-clang-tidy}"
# --fix applies clang-tidy's fixes with clang-apply-replacements, which brew's llvm keeps off PATH: take the one
# beside the clang-tidy in use.
tidy_path="$(command -v "$clang_tidy" || true)"
apply_replacements="${CLANG_APPLY_REPLACEMENTS:-$(dirname "${tidy_path:-$clang_tidy}")/clang-apply-replacements}"
# clazy-standalone is installed outside LLVM's prefix and can't find the compiler's builtin headers on its own.
resource_dir="$("${llvm:+$llvm/bin/}clang" -print-resource-dir)"
jobs="${LINT_JOBS:-$(getconf _NPROCESSORS_ONLN)}"

cd "$root"
if [[ -n "$changed" ]]; then
    base="$(git merge-base "$changed" HEAD)" || { echo "lint: no merge base of $changed and HEAD" >&2; exit 2; }
    # Deleted files are left out; a renamed file is listed under its new name.
    mapfile -t files < <({ git diff --name-only --diff-filter=d "$base" -- "${paths[@]}"
        git ls-files --others --exclude-standard -- "${paths[@]}"; } | grep -E '\.(cpp|h|mm)$' | sort -u || true)
    [[ ${#files[@]} -gt 0 ]] || { echo "no sources changed since $changed under: ${paths[*]}"; exit 0; }
else
    mapfile -t files < <(git ls-files --cached --others --exclude-standard -- "${paths[@]}" | grep -E '\.(cpp|h|mm)$' || true)
    [[ ${#files[@]} -gt 0 ]] || { echo "no sources under: ${paths[*]}"; exit 0; }
fi
mapfile -t sources < <(printf '%s\n' "${files[@]}" | grep -E '\.(cpp|mm)$' || true)
[[ -f "$build/compile_commands.json" ]] || { echo "missing $build/compile_commands.json (configure first)" >&2; exit 2; }
# The analysers check what this build compiles. A file of a target it doesn't configure (an optional test suite) has
# no compile command, and the analysers would guess its flags.
declare -A compiled=()
while IFS= read -r unit; do
    compiled["$unit"]=1
done < <(sed -n 's/^ *"file": *"\(.*\)",\{0,1\}$/\1/p' "$build/compile_commands.json")
units=()
for source in "${sources[@]}"; do
    if [[ -n "${compiled[$root/$source]:-}" ]]; then
        units+=("$source")
    fi
done
not_built=""
[[ ${#units[@]} -eq ${#sources[@]} ]] || not_built=", $((${#sources[@]} - ${#units[@]})) not in this build"

# nearest_first <file>: sorts the paths read from stdin by how much of <file>'s folder they share, the most first.
nearest_first() {
    awk -v file="$1" -F/ 'BEGIN { n = split(file, target, "/") }
        { shared = 0; for (i = 1; i < NF && i < n && $i == target[i]; i++) shared++; printf "%d\t%s\n", shared, $0 }' |
        sort -t $'\t' -k1,1nr -k2,2 | cut -f2-
}

# unit_for_header <header>: the compiled source that checks a changed header (see --changed above), or nothing. An
# include names the header by a path its own path ends with ("base/Clock.h"), or by its bare name from the same folder.
unit_for_header() {
    local stem="${1%.h}" ext level=("$1") next file name includer include
    local -A seen=(["$1"]=1)
    for ext in cpp mm; do
        if [[ -n "${compiled[$root/$stem.$ext]:-}" ]]; then echo "$stem.$ext"; return; fi
    done
    for _ in 1 2 3; do
        next=()
        for file in "${level[@]}"; do
            name="${file##*/}"
            while IFS= read -r includer; do
                [[ -z "${seen[$includer]:-}" ]] || continue
                while IFS= read -r include; do
                    if [[ ( "$include" == */* && "$file" == */"$include" ) ||
                          ( "$include" == "$name" && "${includer%/*}" == "${file%/*}" ) ]]; then
                        seen["$includer"]=1
                        if [[ "$includer" == *.h ]]; then
                            next+=("$includer")
                        elif [[ -n "${compiled[$root/$includer]:-}" ]]; then
                            echo "$includer"
                            return
                        fi
                        break
                    fi
                done < <(sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' "$includer" | grep -F "$name")
            done < <(git grep --untracked -l -F "$name" -- src tests tools | grep -E '\.(cpp|h|mm)$' | nearest_first "$file")
        done
        [[ ${#next[@]} -gt 0 ]] || return 0
        level=("${next[@]}")
    done
}

if [[ -n "$changed" ]]; then
    headers_unchecked=()
    for file in "${files[@]}"; do
        [[ "$file" == *.h ]] || continue
        unit="$(unit_for_header "$file")"
        if [[ -n "$unit" ]]; then units+=("$unit"); else headers_unchecked+=("$file"); fi
    done
    mapfile -t units < <(printf '%s\n' "${units[@]}" | sed '/^$/d' | sort -u)
    echo "== changed since $changed: ${#files[@]} files; the analysers check ${#units[@]} translation units${not_built}"
    printf '   %s\n' "${units[@]}"
    [[ ${#headers_unchecked[@]} -eq 0 ]] || printf '   (no compiled source includes %s: clang-format only)\n' "${headers_unchecked[@]}"
    not_built=""
fi

if [[ $shard_count -gt 1 ]]; then
    # Longest first, each to the share with the least so far: every share computes the same split.
    mapfile -t units < <(for unit in "${units[@]}"; do
        weight=1
        [[ "$unit" != tests/* ]] || weight=3
        printf '%s\t%s\n' "$(($(wc -c < "$unit") * weight))" "$unit"
    done | sort -t $'\t' -k1,1nr -k2,2 | awk -F'\t' -v k="$shard_index" -v n="$shard_count" '
        { best = 1; for (i = 2; i <= n; i++) if (load[i] < load[best]) best = i; load[best] += $1; if (best == k) print $2 }')
    echo "== share $shard_index of $shard_count: ${#units[@]} translation units"
    printf '   %s\n' "${units[@]}"
fi

if [[ $list -eq 1 ]]; then
    [[ $shard_index -ne 1 ]] || printf 'file %s\n' "${files[@]}"
    [[ ${#units[@]} -eq 0 ]] || printf 'unit %s\n' "${units[@]}"
    exit 0
fi

if [[ $shard_index -eq 1 ]]; then
    echo "== clang-format (${#files[@]} files)"
    if [[ $fix -eq 1 ]]; then
        clang-format -i "${files[@]}"
    else
        clang-format --dry-run --Werror "${files[@]}"
    fi
fi

echo "== clang-tidy (${#units[@]} translation units$not_built)"
tidy_args=(-quiet -j "$jobs" -p "$build" -clang-tidy-binary "$clang_tidy")
tidy_log="$build/clang-tidy.log"
: > "$tidy_log"
if [[ $fix -eq 1 ]]; then
    if ! apply_binary="$(command -v "$apply_replacements")"; then
        echo "lint: --fix needs clang-apply-replacements, not found at $apply_replacements (set CLANG_APPLY_REPLACEMENTS)" | tee "$tidy_log" >&2
        exit 1
    fi
    tidy_args+=(-fix -clang-apply-replacements-binary "$apply_binary")
fi
tidy_status=0
[[ ${#units[@]} -eq 0 ]] || "$run_clang_tidy" "${tidy_args[@]}" "${units[@]/#/$root/}" > "$tidy_log" 2>&1 || tidy_status=$?
# A failure of the fix step itself (a missing tool, fixes that didn't apply) is an error line of run-clang-tidy's or
# clang-apply-replacements' own, not a diagnostic: it doesn't start with a file path. clang-tidy's "Error while
# processing" follows a diagnostic, reported below.
if [[ $fix -eq 1 ]] && grep -E '^(error:|Error )' "$tidy_log" | grep -qv '^Error while processing '; then
    echo "lint: clang-tidy --fix failed, fixes not applied; see $tidy_log" >&2
    exit 1
fi
# Diagnostics are lines starting with a file path; progress and summary lines are not.
if grep -E '^/[^:]+:[0-9]+:[0-9]+: (warning|error):' "$tidy_log"; then exit 1; fi
if [[ $fix -eq 1 && $tidy_status -ne 0 ]]; then
    echo "lint: clang-tidy --fix exited with status $tidy_status; see $tidy_log" >&2
    exit 1
fi

echo "== clazy"
clazy_out="$build/clazy.log"
# One translation unit per process, as run-clang-tidy does; LINT_JOBS of them at a time (default: one per core).
export build resource_dir
: > "$clazy_out"
[[ ${#units[@]} -eq 0 ]] || printf '%s\0' "${units[@]}" | xargs -0 -n 1 -P "$jobs" bash -c '
    clazy-standalone -p "$build/compile_commands.json" --header-filter=".*/(src|tests|tools)/.*" \
        --extra-arg="-resource-dir=$resource_dir" --extra-arg=-Wno-missing-designated-field-initializers \
        --checks=level0,level1,level2,no-qstring-allocations,no-non-pod-global-static,no-copyable-polymorphic,no-returning-void-expression \
        "$1" 2>&1 | grep --line-buffered -E "warning:|error:" || true' clazy >> "$clazy_out"
if [[ -s "$clazy_out" ]]; then cat "$clazy_out"; exit 1; fi

echo "== cppcheck"
# A folder takes every translation unit under it; a file, --changed or --shard, only the units themselves.
filters=()
if [[ -n "$changed" || $shard_count -gt 1 ]]; then
    for unit in "${units[@]}"; do filters+=("--file-filter=$root/$unit"); done
else
    for path in "${paths[@]}"; do
        if [[ -f "$path" ]]; then filters+=("--file-filter=$root/$path"); else filters+=("--file-filter=$root/${path%/}/*"); fi
    done
fi
[[ ${#units[@]} -eq 0 ]] || cppcheck -j "$jobs" --project="$build/compile_commands.json" --library=qt --library=googletest --enable=warning,performance,portability --inline-suppr \
    --suppress=missingIncludeSystem --suppress=returnByReference --suppress='*:*/vcpkg_installed/*' --suppress='*:*/_deps/*' \
    --suppress='*:*_autogen/*' --error-exitcode=1 --quiet "${filters[@]}"

echo "lint: clean"
