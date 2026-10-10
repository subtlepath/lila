# Running the sandbox on a remote Linux host with podman

`sandbox.sh` targets Apple's `container` CLI on macOS. `podman.sh` is the
equivalent for a Linux host. Both build from the same `Containerfile`; see
[README.md](README.md) for what the image holds and how to build lila in it.

Run `podman.sh` **on the container host**, from a checkout that lives there:

```sh
ssh -A you@host          # -A so the forwarded ssh agent reaches the container
git clone --recurse-submodules git@github.com:subtlepath/lila.git ~/lila
cd ~/lila
./container/podman.sh build-run
```

## Moving the agent state across

`~/.lila-sandbox` holds the Claude and Codex logins, session history, the
PlatformIO toolchains under `platformio/`, and download caches under `cache/`. The logins and history are
architecture-independent, so they copy straight over. The toolchains are not:
skip `platformio/` when the hosts differ in architecture and let `lila-setup`
fetch it again on the host.

**Stop the container on the Mac first.** The codex sqlite databases keep live
`-wal`/`-shm` files; copying them mid-write moves a torn database.

```sh
rsync -av --delete --exclude platformio/ ~/.lila-sandbox/ you@host:~/.lila-sandbox/
```

This copies `claude/.credentials.json` and `codex/auth.json` — real OAuth
credentials — onto the remote host in plaintext. If that host is shared or
less trusted than your laptop, skip those two files and run
`./container/podman.sh login` on the host instead.

After copying, fix ownership for the remote user's uid:

```sh
ssh you@host 'chown -R $(id -u):$(id -g) ~/.lila-sandbox'
```

## Differences from the Apple `container` version

| | `sandbox.sh` (Apple) | `podman.sh` |
|---|---|---|
| CPU limit | `-c 8` | `--cpus 8` — podman's `-c` is `--cpu-shares`, a relative weight, not a count |
| SSH agent | `--ssh` | bind-mounts `$SSH_AUTH_SOCK`; `podman run` has no `--ssh` |
| User mapping | fixed uid 501 / gid 20 | image built at the host's uid/gid, plus `--userns=keep-id --user root` under rootless podman |
| Base image user | no conflict at uid 501 | `node:lts` holds uid 1000; the build evicts it when it collides with your host uid |
| SELinux | n/a | bind mounts get `:z` when SELinux is enforcing |
| fd limit | image `ulimit` only | `--ulimit nofile=…`, clamped to the host hard limit |

## Notes

- One container per name. Set `LILA_NAME` to run a second alongside the first.
- Under rootless podman, `--userns=keep-id` is what keeps files written in
  `/workspace` owned by you on the host. Without it they land on a subuid.
- `--user root` is required alongside `keep-id`. keep-id otherwise starts the
  container as the host uid, so the entrypoint is never root: its `chown` and
  `npm install -g` fail, and `gosu` cannot switch users
  (`failed switching to "agent": operation not permitted`). With `--user root`
  the entrypoint runs as uid 0 inside the namespace — still a subuid on the
  host — and gosu drops to `agent`, which keep-id maps back to you.
- Overrides: `LILA_IMAGE`, `LILA_NAME`, `LILA_CONFIG_DIR`, `LILA_MEM`,
  `LILA_CPUS`.
