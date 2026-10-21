#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Which tests a build has to run: those whose code or inputs changed since a run in which they passed (P-028).
#
#   tools/ci/affected-tests.sh fingerprint <build-dir> [--passed]   one line per test: name, fingerprint, definition
#   tools/ci/affected-tests.sh select <known> <current>              the names of <current>'s lines that <known> lacks
#
# A test's fingerprint is made of what decides its result:
#   - its definition in ctest, as JSON: command line, environment and other properties;
#   - the executable it runs. A build is byte-reproducible (same sources, compiler, flags and paths give the same
#     bytes), so an unchanged executable means unchanged code, libraries and generated sources. A build that isn't
#     only makes every fingerprint differ, and every test run;
#   - the runtime inputs its executable's compile definitions name with an absolute path: another executable
#     (SELECTA_CLI_PATH) or file, and the data files of a source folder (SELECTA_META_FIXTURES, QT_TESTCASE_SOURCEDIR):
#     every file in it but C++ sources and CMakeLists.txt, which reach the executables through the build. Folders of
#     the build tree are left out: they hold the build itself, and no test reads data the build writes there;
#   - the files its arguments name with an absolute path (an argument that is one, or --option=/path), the same way;
#   - this script.
# When in doubt a test counts as changed and runs: an executable no compile command builds (a script, a target with
# another OUTPUT_NAME), an executable or an input outside the checkout, or one it can't read, gets a fingerprint no
# other run has. A <known> file that is missing or empty selects every test.
#
# With --passed, the tests that failed in the build folder's last ctest run (Testing/Temporary/LastTestsFailed.log)
# are left out, so that they run again next time. Needs bash 4, jq, ctest and sha256sum (or shasum).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"

if command -v sha256sum > /dev/null; then sha=(sha256sum); else sha=(shasum -a 256); fi
hash_text() { "${sha[@]}" | cut -d' ' -f1; }
hash_file() { "${sha[@]}" < "$1" | cut -d' ' -f1; }
# A fingerprint no other run produces: the test always runs.
unknown() { printf 'unknown-%s-%s-%s' "$$" "$RANDOM$RANDOM" "$(date +%s%N)"; }

# hash_folder <folder>: the data files under a source folder, by path and content.
hash_folder() {
    (cd "$1" && find . -type f ! -name '*.cpp' ! -name '*.h' ! -name '*.mm' ! -name CMakeLists.txt -print0 |
        LC_ALL=C sort -z | xargs -0 "${sha[@]}" /dev/null | hash_text)
}

# hash_input <absolute path>: one runtime input.
hash_input() {
    local path="$1"
    if [[ "$path" == "$build"/* || "$path" == "$build" ]]; then
        if [[ -f "$path" ]]; then
            if [[ -r "$path" ]]; then hash_file "$path"; else unknown; fi
        elif [[ -e "$path" ]]; then
            echo "build-folder"
        else
            echo "absent"
        fi
    elif [[ "$path" == "$root"/* ]]; then
        if [[ -f "$path" ]]; then
            if [[ -r "$path" ]]; then hash_file "$path"; else unknown; fi
        elif [[ -d "$path" ]]; then
            hash_folder "$path" || unknown
        elif [[ -e "$path" ]]; then
            unknown
        else
            echo "absent"
        fi
    else
        unknown
    fi
}

# hash_executable <path>: sets executable_hashes[<path>] from the executable and the runtime inputs its compile
# definitions name.
declare -A executable_hashes=()
hash_executable() {
    local executable="$1" name commands inputs="" path
    if [[ "$executable" != "$root"/* || ! -f "$executable" || ! -r "$executable" ]]; then
        executable_hashes["x$executable"]="$(unknown)"
        return
    fi
    name="${executable##*/}"
    commands="$(jq -r --arg target "CMakeFiles/$name.dir/" \
        '.[] | select((.output // "") | contains($target)) | .command' "$build/compile_commands.json")"
    # Its runtime inputs are known only from its compile commands.
    if [[ -z "$commands" ]]; then
        executable_hashes["x$executable"]="$(unknown)"
        return
    fi
    while IFS= read -r path; do
        inputs+="$path $(hash_input "$path")"$'\n'
    done < <(grep -oE -- '-D[A-Za-z_][A-Za-z0-9_]*=\\?"/[^"\\]*\\?"' <<< "$commands" |
        sed -E 's/^[^=]*=\\?"(.*)$/\1/; s/\\?"$//' | LC_ALL=C sort -u)
    executable_hashes["x$executable"]="$(printf '%s\n%s' "$(hash_file "$executable")" "$inputs" | hash_text)"
}

# hash_arguments <paths…>: the files a test's arguments name, once each.
declare -A argument_hashes=()
hash_arguments() {
    local path all=""
    for path in "$@"; do
        [[ -n "${argument_hashes[x$path]:-}" ]] || argument_hashes["x$path"]="$(hash_input "$path")"
        all+="$path ${argument_hashes[x$path]}"$'\n'
    done
    [[ -z "$all" ]] || hash_text <<< "$all"
}

fingerprint() {
    local passed=0 self tests line name command arguments definition paths
    build="$(cd "$1" && pwd)"
    [[ "${2:-}" != --passed ]] || passed=1
    [[ -f "$build/compile_commands.json" ]] || { echo "affected-tests: no $build/compile_commands.json" >&2; exit 2; }
    self="$(hash_file "$0")"
    # Discovering the GUI tests starts a QApplication: Qt's minimal platform needs no display.
    tests="$(cd "$build" && QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-minimal}" ctest --show-only=json-v1)"
    while IFS= read -r command; do
        hash_executable "$command"
    done < <(jq -r '.tests[] | .command[0] // ""' <<< "$tests" | LC_ALL=C sort -u)
    declare -A failed=()
    if [[ $passed -eq 1 && -f "$build/Testing/Temporary/LastTestsFailed.log" ]]; then
        while IFS= read -r line; do
            failed["${line#*:}"]=1
        done < "$build/Testing/Temporary/LastTestsFailed.log"
    fi
    # Fields split on the unit separator: a tab is blank to read, which would merge an empty command away. Arguments
    # split on the record separator.
    while IFS=$'\x1f' read -r name command arguments definition; do
        [[ -z "${failed[$name]:-}" ]] || continue
        IFS=$'\x1e' read -r -a paths <<< "$arguments"
        printf '%s\t%s:%s:%s\t%s\n' "$name" "$self" "${executable_hashes[x$command]}" \
            "$(hash_arguments "${paths[@]}")" "$definition"
    done < <(jq -r '.tests[] | [.name, (.command[0] // ""),
            ((.command // [])[1:] | map(capture("^(?:[^=/]*=)?(?<path>/.*)$").path) | join("\u001e")),
            tojson] | join("\u001f")' <<< "$tests") | LC_ALL=C sort
}

select_tests() {
    local known="$1" current="$2"
    if [[ ! -s "$known" ]]; then
        cut -f1 "$current"
        return
    fi
    LC_ALL=C sort -u "$known" | LC_ALL=C comm -13 - <(LC_ALL=C sort -u "$current") | cut -f1
}

case "${1:-}" in
    fingerprint)
        [[ $# -ge 2 && -d "$2" ]] || { echo "usage: $0 fingerprint <build-dir> [--passed]" >&2; exit 2; }
        fingerprint "$2" "${3:-}" ;;
    select)
        [[ $# -eq 3 && -r "$3" && ! -d "$3" ]] || { echo "usage: $0 select <known> <current>" >&2; exit 2; }
        select_tests "$2" "$3" ;;
    *)
        echo "usage: $0 fingerprint <build-dir> [--passed] | select <known> <current>" >&2
        exit 2 ;;
esac
