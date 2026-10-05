#!/bin/sh
# Fail if src/fonts/ differs from what tools/gen-fonts.sh would generate now
# (PLAN.md section 5.3: generated fonts are committed, CI checks them).
#
#   sh tools/check-fonts.sh
#
# The headword strikes are rasterised from an outline font with the pinned
# FreeType in build/ft-venv (tools/ft-requirements.txt), installed on first
# use. Without it the check fails; it never compares other rasterisations.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/tinta-fonts.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

if ! python3 "$ROOT/tools/bdf2freeink.py" --manifest "$ROOT/tools/fonts.manifest" --out-dir "$TMP/fonts" --quiet; then
  echo "FAIL could not regenerate the fonts (see the error above); src/fonts was not checked"
  exit 1
fi

if diff -r "$TMP/fonts" "$ROOT/src/fonts" >"$TMP/diff" 2>&1; then
  echo "ok   src/fonts matches tools/fonts.manifest"
else
  head -40 "$TMP/diff"
  echo "FAIL src/fonts is stale: run sh tools/gen-fonts.sh"
  exit 1
fi
