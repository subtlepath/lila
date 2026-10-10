#!/usr/bin/env bash
# lila sandbox on macOS, using Apple's `container` CLI. podman.sh is the Linux equivalent.
set -euo pipefail

IMAGE="${LILA_IMAGE:-lila-sandbox}"
CONFIG_DIR="${LILA_CONFIG_DIR:-$HOME/.lila-sandbox}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"

MEM="${LILA_MEM:-32G}"
CPUS="${LILA_CPUS:-8}"

# Find the first running container whose image matches $IMAGE.
_find_container() {
  container list --format json \
    | python3 -c "
import sys, json
for c in json.load(sys.stdin):
    ref = c.get('configuration',{}).get('image',{}).get('reference','')
    status = c.get('status')
    # Older CLIs report a string; newer ones nest it as status.state.
    state = status.get('state') if isinstance(status, dict) else status
    if ref.startswith('${IMAGE}:') and state == 'running':
        print(c.get('id') or c['configuration']['id']); sys.exit(0)
sys.exit(1)
"
}

_env_args() {
  local v
  for v in ANTHROPIC_API_KEY OPENAI_API_KEY; do
    [[ -n "${!v:-}" ]] && printf '%s\n' -e "$v=${!v}"
  done
  printf '%s\n' -e CLAUDE_CODE_MAX_OUTPUT_TOKENS=128000
}

# The image needs the repo's requirements files, but a repo-root context would ship
# .pio/.cache (gigabytes) to the builder, so stage a small context instead. The
# requirements go in as one tarball: Apple's builder (container 1.4.1) drops files
# in context subdirectories, and the -r includes need their relative paths.
_stage_context() {
  local ctx="$1"
  cp "$HERE"/Containerfile "$HERE"/*.sh "$ctx"/
  git -C "$PROJECT_DIR" ls-files -z '*requirements.txt' \
    | COPYFILE_DISABLE=1 tar -C "$PROJECT_DIR" --null -T - -cf "$ctx/requirements.tar"
}

cmd_build() {
  echo "Building $IMAGE..."
  local ctx
  ctx="$(mktemp -d)"
  _stage_context "$ctx"
  container build --dns 8.8.8.8 -t "$IMAGE" -f "$ctx/Containerfile" "$ctx" || { rm -rf "$ctx"; return 1; }
  rm -rf "$ctx"
}

cmd_run() {
  mkdir -p "$CONFIG_DIR"/{claude,codex,local,pi,platformio,cache}

  local env_args=() line
  while IFS= read -r line; do env_args+=("$line"); done < <(_env_args)

  container run --rm -it \
    -v "$PROJECT_DIR:/workspace" \
    -v "$CONFIG_DIR/claude:/home/agent/.claude" \
    -v "$CONFIG_DIR/codex:/home/agent/.codex" \
    -v "$CONFIG_DIR/local:/home/agent/.local" \
    -v "$CONFIG_DIR/pi:/home/agent/.pi" \
    -v "$CONFIG_DIR/platformio:/home/agent/.platformio" \
    -v "$CONFIG_DIR/cache:/home/agent/.cache" \
    --ssh \
    -m "$MEM" -c "$CPUS" \
    ${env_args[@]+"${env_args[@]}"} \
    "$IMAGE"
}

cmd_attach() {
  local cid
  cid="$(_find_container)" || { echo "No running $IMAGE container found." >&2; exit 1; }
  container exec -it "$cid" bash
}

cmd_root_exec() {
  local cid
  cid="$(_find_container)" || { echo "No running $IMAGE container found." >&2; exit 1; }
  container exec -it --user root "$cid" "$@"
}

cmd_login() {
  mkdir -p "$CONFIG_DIR/claude" "$CONFIG_DIR/codex"

  local env_args=() line
  while IFS= read -r line; do env_args+=("$line"); done < <(_env_args)

  container run --rm -it \
    -v "$CONFIG_DIR/claude:/home/agent/.claude" \
    -v "$CONFIG_DIR/codex:/home/agent/.codex" \
    ${env_args[@]+"${env_args[@]}"} \
    "$IMAGE" \
    claude login
}

case "${1:-help}" in
  build)      cmd_build ;;
  run)        cmd_run ;;
  attach)     cmd_attach ;;
  build-run)  cmd_build && cmd_run ;;
  login)      cmd_login ;;
  root-exec)  shift; cmd_root_exec "$@" ;;
  *)
    cat <<'USAGE'
Usage: sandbox.sh {build|run|build-run|attach|login|root-exec}

  build            Build the container image
  run              Run an interactive shell with the project mounted
  build-run        Build then run
  attach           Open another shell in the running container
  login            Authenticate Claude Code via OAuth
  root-exec [cmd]  Run a command as root in the running container

Inside the container:
  lila-setup [env...]   PlatformIO packages for each env (once; persisted)
  pio run -e default    Build firmware
  lila-sim build        Build the freeink-sdk simulator
  lila-sim start        Run .pio/build/default/firmware.bin in it

Environment overrides:
  LILA_IMAGE       image tag            (default: lila-sandbox)
  LILA_CONFIG_DIR  agent state dir      (default: ~/.lila-sandbox)
  LILA_MEM         memory limit         (default: 32G)
  LILA_CPUS        cpu count            (default: 8)
USAGE
    exit 1
    ;;
esac
