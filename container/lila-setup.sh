#!/usr/bin/env bash
# One-time setup for building lila inside the sandbox. Everything it writes is
# under ~/.platformio (a persistent mount), so it is not needed on each boot;
# rerun it for an env not set up yet, or after deleting ~/.lila-sandbox/platformio.
#
#   lila-setup              # default env
#   lila-setup x4pro x4c    # one or more PlatformIO envs
#
# Fetches the PlatformIO packages for each env and pins the platform penv's core
# to the outer one, as .github/workflows/ci.yml does: envs with custom_sdkconfig
# run a nested `pio run` from that penv, and an unpinned core there breaks the
# build (pioarduino/platform-espressif32#529).
set -euo pipefail

PROJECT_DIR="${LILA_PROJECT_DIR:-/workspace}"
PENV_PY="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/penv/bin/python"

if [[ ! -f "$PROJECT_DIR/platformio.ini" ]]; then
  echo "lila-setup: no platformio.ini in $PROJECT_DIR" >&2
  exit 1
fi

if [[ ! -f "$PROJECT_DIR/freeink-sdk/README.md" ]]; then
  echo "Initialising the freeink-sdk submodule..."
  git -C "$PROJECT_DIR" submodule update --init --recursive
fi

(( $# )) || set -- default
for env in "$@"; do
  pio pkg install -d "$PROJECT_DIR" -e "$env"
done

if [[ -x "$PENV_PY" ]]; then
  uv pip install --python "$PENV_PY" "pioarduino==${PIOARDUINO_VERSION}"
fi

echo "Ready: pio run -d $PROJECT_DIR -e ${1}"
