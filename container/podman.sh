#!/usr/bin/env bash
# Podman equivalent of sandbox.sh, for running on a Linux host (arm64 or amd64).
# Run this ON the container host, from a checkout of the repo that lives there.
# The Apple `container` version (sandbox.sh) remains the macOS path.
set -euo pipefail

# Uses mapfile/readarray; macOS ships bash 3.2. This script is for the Linux host.
if (( BASH_VERSINFO[0] < 4 )); then
  echo "podman.sh needs bash 4+ (found $BASH_VERSION). Run it on the Linux host." >&2
  exit 1
fi

IMAGE="${LILA_IMAGE:-lila-sandbox}"
NAME="${LILA_NAME:-lila-sandbox}"
CONFIG_DIR="${LILA_CONFIG_DIR:-$HOME/.lila-sandbox}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"

MEM="${LILA_MEM:-32g}"
CPUS="${LILA_CPUS:-8}"

HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

# SELinux (Fedora/RHEL family) needs bind mounts relabelled or they read as empty.
# :z = shared label, safe when the host also touches these paths.
MNT=""
if command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled 2>/dev/null; then
  MNT=":z"
fi

_rootless() {
  [[ "$(podman info --format '{{.Host.Security.Rootless}}' 2>/dev/null)" == "true" ]]
}

# Rootless podman maps container uid 0 to a subuid, so bind-mounted files written
# by the agent user would land owned by a phantom uid. keep-id maps the invoking
# host user straight through, and the image builds `agent` at that same uid.
_userns_args() {
  if _rootless; then
    # keep-id also makes the container start as the host uid, so the entrypoint
    # would never be root: no chown, no npm -g, and gosu cannot switch users.
    # --user root puts us back at uid 0 inside the namespace, which still maps
    # to a subuid on the host, and gosu then drops to agent == the host user.
    printf '%s\n' --userns=keep-id --user root
  fi
}

# The image's ulimit bump to 65536 fails if the host hard limit is lower.
_nofile() {
  local hard
  hard="$(ulimit -Hn)"
  if [[ "$hard" == "unlimited" ]] || (( hard > 65536 )); then
    echo 65536
  else
    echo "$hard"
  fi
}

_ssh_args() {
  # No `podman run --ssh` (that flag is build-only), so forward the agent socket
  # by hand. Requires `ssh -A` into this host for the socket to be live.
  if [[ -n "${SSH_AUTH_SOCK:-}" && -S "${SSH_AUTH_SOCK}" ]]; then
    printf '%s\n' -v "${SSH_AUTH_SOCK}:/ssh-agent${MNT}" -e SSH_AUTH_SOCK=/ssh-agent
  fi
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
  echo "Building $IMAGE for uid=$HOST_UID gid=$HOST_GID..."
  local ctx
  ctx="$(mktemp -d)"
  _stage_context "$ctx"
  podman build \
    --build-arg "AGENT_UID=$HOST_UID" \
    --build-arg "AGENT_GID=$HOST_GID" \
    -t "$IMAGE" -f "$ctx/Containerfile" "$ctx" || { rm -rf "$ctx"; return 1; }
  rm -rf "$ctx"
}

cmd_run() {
  mkdir -p "$CONFIG_DIR"/{claude,codex,local,pi,platformio,cache}

  local userns ssh env nofile
  mapfile -t userns < <(_userns_args)
  mapfile -t ssh    < <(_ssh_args)
  mapfile -t env    < <(_env_args)
  nofile="$(_nofile)"

  # -c on podman is --cpu-shares (a relative weight), NOT a CPU count. --cpus is.
  podman run --rm -it \
    --name "$NAME" \
    -v "$PROJECT_DIR:/workspace${MNT}" \
    -v "$CONFIG_DIR/claude:/home/agent/.claude${MNT}" \
    -v "$CONFIG_DIR/codex:/home/agent/.codex${MNT}" \
    -v "$CONFIG_DIR/local:/home/agent/.local${MNT}" \
    -v "$CONFIG_DIR/pi:/home/agent/.pi${MNT}" \
    -v "$CONFIG_DIR/platformio:/home/agent/.platformio${MNT}" \
    -v "$CONFIG_DIR/cache:/home/agent/.cache${MNT}" \
    --ulimit "nofile=${nofile}:${nofile}" \
    -m "$MEM" --cpus "$CPUS" \
    ${userns[@]+"${userns[@]}"} \
    ${ssh[@]+"${ssh[@]}"} \
    ${env[@]+"${env[@]}"} \
    "$IMAGE"
}

_require_running() {
  podman ps --filter "name=^${NAME}$" --filter status=running --format '{{.ID}}' \
    | head -1 | grep . || {
      echo "No running container named '$NAME'." >&2
      exit 1
    }
}

cmd_attach() {
  _require_running >/dev/null
  podman exec -it "$NAME" bash
}

cmd_root_exec() {
  _require_running >/dev/null
  podman exec -it --user root "$NAME" "$@"
}

cmd_login() {
  mkdir -p "$CONFIG_DIR"/{claude,codex}

  local userns env
  mapfile -t userns < <(_userns_args)
  mapfile -t env    < <(_env_args)

  podman run --rm -it \
    -v "$CONFIG_DIR/claude:/home/agent/.claude${MNT}" \
    -v "$CONFIG_DIR/codex:/home/agent/.codex${MNT}" \
    ${userns[@]+"${userns[@]}"} \
    ${env[@]+"${env[@]}"} \
    "$IMAGE" \
    claude login
}

case "${1:-help}" in
  build)      cmd_build ;;
  run)        cmd_run ;;
  build-run)  cmd_build && cmd_run ;;
  attach)     cmd_attach ;;
  login)      cmd_login ;;
  root-exec)  shift; cmd_root_exec "$@" ;;
  *)
    cat <<'USAGE'
Usage: podman.sh {build|run|build-run|attach|login|root-exec}

  build            Build the image, with the agent user matching your host uid/gid
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
  LILA_NAME        container name       (default: lila-sandbox)
  LILA_CONFIG_DIR  agent state dir      (default: ~/.lila-sandbox)
  LILA_MEM         memory limit         (default: 32g)
  LILA_CPUS        cpu count            (default: 8)
USAGE
    exit 1
    ;;
esac
