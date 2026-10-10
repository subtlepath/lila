#!/usr/bin/env bash
# Builds and runs the freeink-sdk simulator (freeink-sdk/tools/simulator) on lila
# firmware images, headless, inside the sandbox. See freeink-sdk/docs/simulator.md.
#
# The daemon is built into ~/.local/share/lila-sim (a persistent mount) rather
# than the SDK's tools/simulator/build/, so it never overwrites a macOS build
# made from the same bind-mounted checkout.
set -euo pipefail

PROJECT_DIR="${LILA_PROJECT_DIR:-/workspace}"
SIM_DIR="$PROJECT_DIR/freeink-sdk/tools/simulator"
STATE_DIR="${LILA_SIM_HOME:-$HOME/.local/share/lila-sim}"
BIN_DIR="$STATE_DIR/bin-$(uname -m)"
SD_DIR="${LILA_SIM_SD:-$STATE_DIR/sd}"
# The daemon builds the card's FAT32 volume in RAM at full capacity; its 8192 MB
# default exceeds the container's memory limit.
SD_MB="${LILA_SIM_SD_MB:-1024}"
RUN_DIR="$HOME/.freeink-sim"
PID_FILE="$RUN_DIR/daemon.pid"
LOG_FILE="$RUN_DIR/daemon.log"
SOCKET="$RUN_DIR/sim.sock"
ROM_DIR="$SIM_DIR/emu/rom"

_require_sdk() {
  if [[ ! -f "$SIM_DIR/build/build-daemon.sh" ]]; then
    echo "lila-sim: $SIM_DIR not found." >&2
    echo "The simulator is only in the subtlepath fork (git@github.com:subtlepath/freeink-sdk.git)." >&2
    exit 1
  fi
}

_require_daemon() {
  if [[ ! -x "$BIN_DIR/freeink-simd" ]]; then
    echo "lila-sim: daemon not built. Run: lila-sim build" >&2
    exit 1
  fi
}

_cli() {
  python3 "$SIM_DIR/cli/freeink-sim" --socket "$SOCKET" "$@"
}

_running() {
  [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null
}

# PlatformIO env -> FreeInk board, from the FREEINK_DEVICE_* flags in platformio.ini.
# The default envs build for both X3 and X4 and detect at runtime; X4 is the target.
_device_for_env() {
  case "$1" in
    x4pro*) echo X4PRO ;;
    x4c*) echo X4CLASSIC ;;
    sticky*) echo STICKY ;;
    papermono*) echo PAPERMONO ;;
    *) echo X4 ;;
  esac
}

cmd_build() {
  _require_sdk
  mkdir -p "$BIN_DIR"
  FSIM_NO_SDL=1 FSIM_OUT="$BIN_DIR/freeink-simd" FSIM_EMU_OUT="$BIN_DIR/freeink-emu" \
    sh "$SIM_DIR/build/build-daemon.sh"
}

cmd_start() {
  _require_sdk
  _require_daemon
  if _running; then
    echo "lila-sim: daemon already running (pid $(cat "$PID_FILE")). lila-sim stop first." >&2
    exit 1
  fi

  local target="default" device="" extra=()
  while (($#)); do
    case "$1" in
      --device) device="$2"; shift 2 ;;
      --sd) SD_DIR="$2"; shift 2 ;;
      --) shift; extra=("$@"); break ;;
      *) target="$1"; shift ;;
    esac
  done

  local image
  if [[ -f "$target" ]]; then
    image="$(cd "$(dirname "$target")" && pwd)/$(basename "$target")"
    [[ -n "$device" ]] || device="X4"
  else
    image="$PROJECT_DIR/.pio/build/$target/firmware.bin"
    [[ -n "$device" ]] || device="$(_device_for_env "$target")"
  fi
  if [[ ! -f "$image" ]]; then
    echo "lila-sim: no image at $image. Build it first: pio run -e $target" >&2
    exit 1
  fi

  mkdir -p "$RUN_DIR" "$SD_DIR"
  rm -f "$SOCKET"
  # Start paused so the power key is already down when the firmware boots;
  # these devices go straight back to sleep if it is not held.
  nohup "$BIN_DIR/freeink-simd" "$image" --device "$device" --sd "$SD_DIR" --sd-capacity "$SD_MB" \
    --rom-dir "$ROM_DIR" --headless --paused ${extra[@]+"${extra[@]}"} >"$LOG_FILE" 2>&1 &
  echo $! >"$PID_FILE"

  local i
  for ((i = 0; i < 100; i++)); do
    [[ -S "$SOCKET" ]] && break
    _running || { cat "$LOG_FILE" >&2; echo "lila-sim: daemon exited" >&2; exit 1; }
    sleep 0.1
  done
  [[ -S "$SOCKET" ]] || { echo "lila-sim: no socket at $SOCKET after 10s; see $LOG_FILE" >&2; exit 1; }

  _cli --quiet hold power
  _cli --quiet resume
  echo "Booting $(basename "$image") as $device (card: $SD_DIR)..."
  if _cli --quiet --timeout 130 wait-refresh --timeout 120000; then
    _cli --quiet release power
    echo "Running. Try: lila-sim capture screen.png   lila-sim press confirm   lila-sim log"
  else
    echo "lila-sim: no panel refresh within 120s; power is still held. Check: lila-sim log, lila-sim emu" >&2
    exit 1
  fi
}

cmd_stop() {
  if _running; then
    kill "$(cat "$PID_FILE")"
    echo "Stopped."
  else
    echo "Not running."
  fi
  rm -f "$PID_FILE"
}

# freeink-emu reads options only after `boot <image>`; it looks for ROM maps
# relative to the working directory otherwise.
cmd_emu_tool() {
  _require_sdk
  _require_daemon
  if [[ "${1:-}" == boot && $# -ge 2 ]]; then
    exec "$BIN_DIR/freeink-emu" boot "$2" --rom-dir "$ROM_DIR" "${@:3}"
  fi
  exec "$BIN_DIR/freeink-emu" "$@"
}

case "${1:-help}" in
  build) cmd_build ;;
  start) shift; cmd_start "$@" ;;
  stop) cmd_stop ;;
  emu-tool) shift; cmd_emu_tool "$@" ;;
  help | -h | --help)
    cat <<USAGE
Usage: lila-sim {build|start|stop|emu-tool|<freeink-sim command>}

  build                          Build freeink-simd and freeink-emu (headless) into
                                 $BIN_DIR
  start [ENV|IMAGE] [--device NAME] [--sd DIR] [-- DAEMON_ARGS]
                                 Run a lila image in the background, power key held
                                 until the first refresh. ENV is a PlatformIO env
                                 (default: default -> .pio/build/default/firmware.bin)
  stop                           Stop the daemon
  emu-tool ARGS                  freeink-emu (info, boards, boot ...) with the SDK's ROM maps
  anything else                  passed to freeink-sim: status, press, hold, release,
                                 capture, log, expect, emu, sd, nvs, battery, ...

Card: $SD_DIR, $SD_MB MB (LILA_SIM_SD, LILA_SIM_SD_MB). Daemon log: $LOG_FILE
USAGE
    ;;
  *) _require_sdk; _cli "$@" ;;
esac
