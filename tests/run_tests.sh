#!/bin/bash
# Builds and runs every tests/test_*.cpp against the project's sources (minus
# main.cpp, which defines its own main()). No test framework dependency --
# see tests/test_util.h. Must pass offline, no network, no LLM (PROMPT.md).
#
# Library sources are compiled to .o once and relinked per test binary,
# rather than recompiled from scratch for every test file -- this is the
# difference between O(n) and O(n*m) compiles as both grow.
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

OBJ_DIR="build/tests/obj"
BUILD_DIR="build/tests"
mkdir -p "$OBJ_DIR" "$BUILD_DIR"

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++11 -O0 -g -Wall -Wextra -Iinclude -Ithird_party -Itests -pthread"

overall=0

LIB_OBJS=()
for src in src/*.cpp; do
    [ "$(basename "$src")" = "main.cpp" ] && continue
    obj="$OBJ_DIR/$(basename "${src%.cpp}").o"
    if [ ! -e "$obj" ] || [ "$src" -nt "$obj" ]; then
        if ! $CXX $CXXFLAGS -c "$src" -o "$obj" 2>"$BUILD_DIR/$(basename "$src").build.log"; then
            echo "BUILD FAIL: $src"
            cat "$BUILD_DIR/$(basename "$src").build.log"
            overall=1
        fi
    fi
    LIB_OBJS+=("$obj")
done
[ "$overall" -eq 0 ] || exit 1

for t in tests/test_*.cpp; do
    name="$(basename "$t" .cpp)"
    bin="$BUILD_DIR/$name"
    if ! $CXX $CXXFLAGS "$t" "${LIB_OBJS[@]}" -o "$bin" 2>"$BUILD_DIR/$name.build.log"; then
        echo "BUILD FAIL: $name"
        cat "$BUILD_DIR/$name.build.log"
        overall=1
        continue
    fi
    if "$bin" > "$BUILD_DIR/$name.out" 2>&1; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        cat "$BUILD_DIR/$name.out"
        overall=1
    fi
done

for it in tests/integration_*.sh; do
    [ -e "$it" ] || continue
    if ! "$it"; then
        overall=1
    fi
done

exit $overall
