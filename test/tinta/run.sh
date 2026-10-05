#!/bin/sh
# Host unit tests for lib/Tinta/src/core (the Spanish course app's engine).
#
#   sh test/tinta/run.sh            # every test
#   sh test/tinta/run.sh pack_test  # one test
#
# Each *_test.cpp names the core modules it links on a line like
#
#   // modules: text lang
#
# and is built with lib/Tinta/src/core/<module>/*.cpp for those modules only.
# Tests run from test/tinta, so fixture paths are relative to it. The course
# packs in fixtures/ are compiled by ../spanish's tools/packc; fixtures/full.pack
# (the whole course, about 3 MB) is not committed, and the tests that need it
# are skipped without it:
#
#   (cd ../spanish && python3 tools/packc --out <lila>/test/tinta/fixtures/full.pack)
#
# TINTA_PACK_SOURCE=file reads every pack through the SD card's block cache
# and string copies instead of memory.

set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$ROOT/test/tinta"
CXX=${CXX:-c++}
OUT="$HERE/build"
mkdir -p "$OUT/usage-sample/tinta"

FLAGS="-std=c++17 -O1 -g -Wall -Wextra -fno-exceptions"
INCLUDES="-I$ROOT/lib/Tinta/src -I$HERE -I$ROOT/freeink-sdk/libs/ui/FreeInkUI/include"

status=0
ran=0
for test in "$HERE"/*_test.cpp; do
  [ -f "$test" ] || continue
  name=$(basename "$test" .cpp)
  if [ -n "$1" ] && [ "$1" != "$name" ]; then continue; fi

  modules=$(sed -n 's|^// modules:||p' "$test" | head -1)
  sources=""
  for module in $modules; do
    for src in "$ROOT/lib/Tinta/src/core/$module"/*.cpp; do
      [ -f "$src" ] && sources="$sources $src"
    done
  done

  ran=$((ran + 1))
  # shellcheck disable=SC2086
  if ! $CXX $FLAGS $INCLUDES $sources "$test" -o "$OUT/$name"; then
    echo "BUILD FAIL $name"
    status=1
    continue
  fi
  if (cd "$HERE" && "$OUT/$name"); then
    echo "ok   $name"
  else
    echo "FAIL $name"
    status=1
  fi
done

[ "$ran" -gt 0 ] || echo "no tests matched"
exit $status
