#!/bin/sh
# Regenerate src/fonts/ from tools/fonts.manifest and lila's x11/ strikes.
#
#   sh tools/gen-fonts.sh
#
# Generates into a scratch directory first, so a failed conversion leaves the
# committed fonts untouched, then replaces every generated file (stale strikes
# dropped from the manifest disappear too). The headword strikes need the
# pinned FreeType, installed into build/ft-venv on first use.

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/tinta-fonts.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

python3 "$ROOT/tools/bdf2freeink.py" --manifest "$ROOT/tools/fonts.manifest" --out-dir "$TMP"

mkdir -p "$ROOT/src/fonts"
rm -f "$ROOT/src/fonts"/*.cpp "$ROOT/src/fonts/Strikes.h"
cp "$TMP"/* "$ROOT/src/fonts/"
