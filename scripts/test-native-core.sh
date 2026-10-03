#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

CC="${CC:-cc}"
CXX="${CXX:-c++}"
VERSION="$(cat "$ROOT/vendor/quickjs/VERSION")"

CFLAGS=(
  -std=gnu11
  -O2
  -g
  -fwrapv
  -D_GNU_SOURCE
  "-DCONFIG_VERSION=\"$VERSION\""
)
CXXFLAGS=(
  -std=c++17
  -O2
  -g
  -Wall
  -Wextra
  -Wpedantic
  -I"$ROOT/cpp"
)

QUICKJS_SOURCES=(
  quickjs.c
  dtoa.c
  libregexp.c
  libunicode.c
  cutils.c
)

OBJECTS=()
for source in "${QUICKJS_SOURCES[@]}"; do
  object="$BUILD/${source%.c}.o"
  "$CC" "${CFLAGS[@]}" -c "$ROOT/vendor/quickjs/$source" -o "$object"
  OBJECTS+=("$object")
done

"$CXX" "${CXXFLAGS[@]}"   -c "$ROOT/cpp/QuickJSRuntime.cpp"   -o "$BUILD/QuickJSRuntime.o"

"$CXX" "${CXXFLAGS[@]}"   -c "$ROOT/cpp/tests/quickjs_runtime_test.cpp"   -o "$BUILD/quickjs_runtime_test.o"

"$CXX"   "$BUILD/QuickJSRuntime.o"   "$BUILD/quickjs_runtime_test.o"   "${OBJECTS[@]}"   -lm -ldl -lpthread   -o "$BUILD/quickjs_runtime_test"

"$BUILD/quickjs_runtime_test"
