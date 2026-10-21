#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of tools/ci/affected-tests.sh, in a throwaway checkout: a hand-written CTestTestfile.cmake, executables that
# are plain files, and a compile_commands.json that names runtime inputs. Nothing is built or run.
#
#   tests/tools/affected-tests-test.sh            needs bash 4 or later, jq and ctest
set -euo pipefail
script="$(cd "$(dirname "$0")/../.." && pwd)/tools/ci/affected-tests.sh"
root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT
build="$root/build"
failures=0

check() { # check <name> <expected> <actual>
    if [[ "$2" == "$3" ]]; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        diff <(printf '%s\n' "$2") <(printf '%s\n' "$3") | sed 's/^/     /' || true
        failures=$((failures + 1))
    fi
}

write() { mkdir -p "$(dirname "$1")"; printf '%s\n' "${@:2}" > "$1"; }
fingerprint() { "$root/tools/ci/affected-tests.sh" fingerprint "$build" "$@"; }
selected() { "$root/tools/ci/affected-tests.sh" select "$1" <(fingerprint) | LC_ALL=C sort | paste -s -d' ' -; }

mkdir -p "$root/tools/ci" "$build/tests"
cp "$script" "$root/tools/ci/affected-tests.sh"
# Three test executables: core_tests reads fixtures, cli_tests runs the cli, plain_tests nothing else.
write "$build/tests/core_tests" core-v1
write "$build/tests/cli_tests" cli-tests-v1
write "$build/tests/plain_tests" plain-v1
write "$build/tools/cli" cli-v1
write "$root/tests/fixtures/answer.json" '{"answer": 1}'
write "$root/tests/fixtures/FixtureTest.cpp" '// compiled into an executable, not read at run time'
write "$root/tests/check.sh" '#!/bin/sh' 'exit 0'
write "$root/tests/data/input.txt" 'input-v1'
write "$build/CTestTestfile.cmake" \
    "add_test([=[Core.One]=] \"$build/tests/core_tests\" --gtest_filter=Core.One)" \
    "add_test([=[Core.Two]=] \"$build/tests/core_tests\" --gtest_filter=Core.Two)" \
    "add_test([=[Cli.Runs]=] \"$build/tests/cli_tests\" --gtest_filter=Cli.Runs)" \
    "add_test([=[Plain.Works]=] \"$build/tests/plain_tests\" --gtest_filter=Plain.Works)" \
    "set_tests_properties([=[Plain.Works]=] PROPERTIES ENVIRONMENT \"MODE=a\")" \
    "add_test([=[Outside.Runs]=] \"/bin/sh\" -c true)" \
    "add_test([=[Script.Checks]=] \"$root/tests/check.sh\")" \
    "add_test([=[Plain.Reads]=] \"$build/tests/plain_tests\" --input=$root/tests/data/input.txt --gtest_filter=Plain.Reads)"
# As CMake writes them: the defines' quotes escaped for the shell (-DNAME=\"value\").
command() { # command <target> <defines…>
    jq -n --arg directory "$build" --arg command "c++ ${*:2} -o x.o -c x.cpp" --arg file "$root/x.cpp" \
        --arg output "tests/CMakeFiles/$1.dir/x.cpp.o" \
        '{directory: $directory, command: $command, file: $file, output: $output}'
}
jq -s . <(command core_tests "-DFIXTURES=\\\"$root/tests/fixtures\\\" -DQT_TESTCASE_BUILDDIR=\\\"$build/tests\\\" -DKIND=1") \
    <(command cli_tests "-DCLI_PATH=\\\"$build/tools/cli\\\"") <(command plain_tests "-DNOTHING=1") \
    > "$build/compile_commands.json"

fingerprint > "$root/known.txt"
check "every test has a fingerprint" "Cli.Runs Core.One Core.Two Outside.Runs Plain.Reads Plain.Works Script.Checks" \
    "$(cut -f1 "$root/known.txt" | paste -s -d' ' -)"
check "fingerprints are stable" "$(grep -vE '^(Outside|Script)' "$root/known.txt")" \
    "$(fingerprint | grep -vE '^(Outside|Script)')"
check "nothing changed: only what can't be judged runs" "Outside.Runs Script.Checks" "$(selected "$root/known.txt")"
check "no known fingerprints: everything runs" "Cli.Runs Core.One Core.Two Outside.Runs Plain.Reads Plain.Works Script.Checks" \
    "$(selected /dev/null)"
check "an empty file of known fingerprints: everything runs" "Cli.Runs Core.One Core.Two Outside.Runs Plain.Reads Plain.Works Script.Checks" \
    "$(selected <(true))"

write "$build/tests/core_tests" core-v2
check "a changed executable runs its tests" "Core.One Core.Two Outside.Runs Script.Checks" \
    "$(selected "$root/known.txt")"
write "$build/tests/core_tests" core-v1

write "$build/tools/cli" cli-v2
check "a changed executable a test runs (SELECTA_CLI_PATH) runs that test" "Cli.Runs Outside.Runs Script.Checks" \
    "$(selected "$root/known.txt")"
write "$build/tools/cli" cli-v1

write "$root/tests/fixtures/answer.json" '{"answer": 2}'
check "a changed data file in a named folder runs its tests" "Core.One Core.Two Outside.Runs Script.Checks" \
    "$(selected "$root/known.txt")"
write "$root/tests/fixtures/answer.json" '{"answer": 1}'

write "$root/tests/fixtures/FixtureTest.cpp" '// changed: reaches the tests through their executables only'
check "C++ sources in a named folder count only through the build" "Outside.Runs Script.Checks" "$(selected "$root/known.txt")"

write "$build/tests/stray.o" 'build output'
check "folders of the build tree don't count" "Outside.Runs Script.Checks" "$(selected "$root/known.txt")"

sed -i.bak 's/MODE=a/MODE=b/' "$build/CTestTestfile.cmake"
check "a changed test definition runs that test" "Outside.Runs Plain.Works Script.Checks" \
    "$(selected "$root/known.txt")"
mv "$build/CTestTestfile.cmake.bak" "$build/CTestTestfile.cmake"

chmod 000 "$build/tests/plain_tests"
if [[ ! -r "$build/tests/plain_tests" ]]; then
    check "an executable it can't read runs its tests" "Outside.Runs Plain.Reads Plain.Works Script.Checks" \
        "$(selected "$root/known.txt")"
fi
chmod 644 "$build/tests/plain_tests"

rm "$root/tests/fixtures/answer.json"
check "a removed input runs its tests" "Core.One Core.Two Outside.Runs Script.Checks" "$(selected "$root/known.txt")"
write "$root/tests/fixtures/answer.json" '{"answer": 1}'

write "$root/tests/data/input.txt" 'input-v2'
check "a changed file an argument names runs that test" "Outside.Runs Plain.Reads Script.Checks" \
    "$(selected "$root/known.txt")"
write "$root/tests/data/input.txt" 'input-v1'

check "a script no compile command builds always runs" "Script.Checks" \
    "$(selected "$root/known.txt" | tr ' ' '\n' | grep -x Script.Checks)"

printf '# changed\n' >> "$root/tools/ci/affected-tests.sh"
check "a changed script runs everything" "Cli.Runs Core.One Core.Two Outside.Runs Plain.Reads Plain.Works Script.Checks" \
    "$(selected "$root/known.txt")"
cp "$script" "$root/tools/ci/affected-tests.sh"

mkdir -p "$build/Testing/Temporary"
printf '2:Core.Two\n4:Outside.Runs\n' > "$build/Testing/Temporary/LastTestsFailed.log"
check "--passed leaves the failed tests out" "Cli.Runs Core.One Plain.Reads Plain.Works Script.Checks" \
    "$(fingerprint --passed | cut -f1 | paste -s -d' ' -)"
fingerprint --passed > "$root/passed.txt"
rm "$build/Testing/Temporary/LastTestsFailed.log"
check "a test that failed last time runs again" "Core.Two Outside.Runs Script.Checks" "$(selected "$root/passed.txt")"

status() { set +e; "$root/tools/ci/affected-tests.sh" "$@" > /dev/null 2>&1; echo $?; set -e; }
check "no command" 2 "$(status)"
check "fingerprint of a folder that isn't there" 2 "$(status fingerprint "$root/nowhere")"
check "fingerprint without compile_commands.json" 2 "$(status fingerprint "$root/tests")"
check "select without the current fingerprints" 2 "$(status select "$root/known.txt" "$root/nowhere.txt")"

if [[ $failures -gt 0 ]]; then
    echo "$failures failed"
    exit 1
fi
echo "affected-tests-test: all passed"
