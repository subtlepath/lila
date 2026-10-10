#!/bin/bash
set -e

export CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=true
# Runs as root: ~ would be /root, which agent cannot enter once gosu passes PATH on.
export PATH=/home/agent/.local/bin:$PATH
# Raise file descriptor limits (container default of 1024 is too low)
ulimit -n 65536 2>/dev/null || ulimit -n $(ulimit -Hn) 2>/dev/null || true
mkdir -p /home/agent/.claude
chown -R agent: /home/agent/.claude 2>/dev/null || true
chown -R agent: /home/agent/.codex 2>/dev/null || true
# Toolchains, packages and download caches live on mounts so they survive restarts.
chown agent: /home/agent/.platformio /home/agent/.local /home/agent/.cache 2>/dev/null || true

# Persist ~/.claude.json inside the mounted volume via symlink
if [ -f /home/agent/.claude/claude.json ] && [ ! -L /home/agent/.claude.json ]; then
  ln -sf /home/agent/.claude/claude.json /home/agent/.claude.json
elif [ -f /home/agent/.claude.json ] && [ ! -L /home/agent/.claude.json ]; then
  # First run with an existing file: move it into the volume
  mv /home/agent/.claude.json /home/agent/.claude/claude.json
  ln -sf /home/agent/.claude/claude.json /home/agent/.claude.json
elif [ ! -e /home/agent/.claude.json ]; then
  # No file yet — create empty one in volume and symlink
  touch /home/agent/.claude/claude.json
  ln -sf /home/agent/.claude/claude.json /home/agent/.claude.json
fi
chown agent: /home/agent/.claude/claude.json /home/agent/.claude.json 2>/dev/null || true


# Under rootless podman the container may already start as the agent user, in
# which case there is no root to install as and nobody to drop from.
if [ "$(id -u)" -eq 0 ]; then
  # Fetch latest versions of coding agent tools
  echo "Updating coding agent tools..."
  npm install -g @mariozechner/pi-coding-agent@latest @openai/codex@latest 2>&1 | tail -1
  echo "Done."

  if [ $# -eq 0 ]; then
    exec gosu agent bash
  else
    exec gosu agent "$@"
  fi
else
  if [ $# -eq 0 ]; then
    exec bash
  else
    exec "$@"
  fi
fi
