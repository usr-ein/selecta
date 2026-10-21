#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of tools/lint.sh's file selection: --changed, --shard and --list, in a throwaway git repository with a
# hand-written compile_commands.json. The analysers never run (--list stops before them).
#
#   tests/tools/lint-test.sh            needs bash 4 or later, git, and the lint tools (lint.sh looks them up)
set -euo pipefail
lint_sh="$(cd "$(dirname "$0")/../.." && pwd)/tools/lint.sh"
repo="$(mktemp -d)"
trap 'rm -rf "$repo"' EXIT
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

lint() { (cd "$repo" && SELECTA_BUILD="$repo/build" tools/lint.sh "$@"); }
listed() { local kind="$1"; shift; lint --list "$@" | sed -n "s/^$kind //p"; }
files() { listed file "$@"; }
units() { listed unit "$@"; }

write() { mkdir -p "$repo/$(dirname "$1")"; printf '%s\n' "${@:2}" > "$repo/$1"; }

mkdir -p "$repo/tools" "$repo/build"
cp "$lint_sh" "$repo/tools/lint.sh"
git -C "$repo" init -q
write src/a/Foo.h '#pragma once'
write src/a/Foo.cpp '#include "a/Foo.h"'
write src/a/Bar.h '#pragma once'
write src/b/Use.cpp '#include "a/Bar.h"'
write src/a/Near.cpp '#include "a/Bar.h"'
write src/a/Deep.h '#pragma once'
write src/a/Mid.h '#include "a/Deep.h"'
write src/b/Top.cpp '#include "a/Mid.h"'
write src/a/Orphan.h '#pragma once'
write src/a/Similar.h '#pragma once'
write src/a/MySimilar.h '#pragma once'
write src/b/Other.cpp '#include "a/MySimilar.h"'
write src/a/Foo2.h '#pragma once'
write src/b/Foo2.h '#pragma once'
write src/b/Bare.cpp '#include "Foo2.h"'
write src/a/Mac.h '#pragma once'
write src/a/Mac.mm '#include "a/Mac.h"'
write src/a/Gone.cpp '// removed later'
write tests/a/FooTest.cpp '#include "a/Foo.h"' '// a test, three times the weight per byte'
write src/a/notes.txt 'not C++'
compiled=(src/a/Foo.cpp src/b/Use.cpp src/a/Near.cpp src/b/Top.cpp src/b/Other.cpp src/b/Bare.cpp src/a/Gone.cpp
    src/a/New.cpp tests/a/FooTest.cpp)
{
    echo '['
    for unit in "${compiled[@]}"; do
        printf '{\n  "directory": "%s/build",\n  "command": "c++ -c %s/%s",\n  "file": "%s/%s"\n},\n' \
            "$repo" "$repo" "$unit" "$repo" "$unit"
    done
    echo ']'
} > "$repo/build/compile_commands.json"
echo 'build/' > "$repo/.gitignore"
git -C "$repo" add -A
git -C "$repo" -c user.name=t -c user.email=t@example.invalid commit -q -m base
git -C "$repo" branch base

check "nothing changed" "no sources changed since base under: src tests tools" "$(lint --list --changed base)"

# A commit and some uncommitted work since base.
write src/a/Foo.h '#pragma once' '// changed'
write src/a/Bar.h '#pragma once' '// changed'
write src/a/Deep.h '#pragma once' '// changed'
write src/a/Orphan.h '#pragma once' '// changed'
write src/a/Similar.h '#pragma once' '// changed'
write src/a/Foo2.h '#pragma once' '// changed'
write src/a/Mac.mm '#include "a/Mac.h"' '// changed'
write src/a/notes.txt 'changed'
git -C "$repo" rm -q src/a/Gone.cpp
git -C "$repo" -c user.name=t -c user.email=t@example.invalid commit -q -am change
write tests/a/FooTest.cpp '#include "a/Foo.h"' '// uncommitted'
write src/a/New.cpp '// new, untracked'

check "changed files: committed, uncommitted and new; not deleted, not other kinds" \
"src/a/Bar.h
src/a/Deep.h
src/a/Foo.h
src/a/Foo2.h
src/a/Mac.mm
src/a/New.cpp
src/a/Orphan.h
src/a/Similar.h
tests/a/FooTest.cpp" "$(files --changed base)"

# Foo.h: its own .cpp. Bar.h: the includer in its own folder, not the one in src/b. Deep.h: through Mid.h. Orphan.h,
# Similar.h (MySimilar.h's includer doesn't count) and Foo2.h (src/b's bare "Foo2.h" is src/b/Foo2.h) have no unit.
# Mac.mm isn't in this build.
check "translation units of the changed files" \
"src/a/Foo.cpp
src/a/Near.cpp
src/a/New.cpp
src/b/Top.cpp
tests/a/FooTest.cpp" "$(units --changed base)"

check "a path narrows --changed" "src/a/Foo.cpp" "$(units --changed base src/a/Foo.h)"
check "a ref is a merge base: HEAD's own changes since it" "$(units --changed base)" "$(units --changed base~0)"

all="$(units --changed base)"
shards=""
for k in 1 2 3; do
    shard="$(units --changed base --shard "$k/3")"
    [[ -z "$shard" ]] || shards+="$shard"$'\n'
    if [[ $k -gt 1 ]]; then
        check "share $k/3 lists no file for clang-format" "" "$(files --changed base --shard "$k/3")"
    fi
done
check "three shares check every unit once" "$all" "$(printf '%s' "$shards" | sort)"
check "the heaviest unit (a test) has a share of its own" "tests/a/FooTest.cpp" "$(units --changed base --shard 1/3)"
check "one share is the whole" "$all" "$(units --changed base --shard 1/1)"

check "a folder without --changed: every source in it" \
"src/b/Bare.cpp
src/b/Foo2.h
src/b/Other.cpp
src/b/Top.cpp
src/b/Use.cpp" "$(files src/b)"

status() { set +e; lint "$@" > /dev/null 2>&1; echo $?; set -e; }
check "--changed without a ref" 2 "$(status --list --changed)"
check "--changed with an unknown ref" 2 "$(status --list --changed no-such-ref)"
check "--shard 0/2" 2 "$(status --list --shard 0/2)"
check "--shard 3/2" 2 "$(status --list --shard 3/2)"
check "--shard x" 2 "$(status --list --shard x)"

if [[ $failures -gt 0 ]]; then
    echo "$failures failed"
    exit 1
fi
echo "lint-test: all passed"
