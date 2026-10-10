# lila development sandbox

A container for working on lila with coding agents: it builds the firmware,
runs the host tests and formatting/static checks CI runs, and runs lila images
in the freeink-sdk simulator. `sandbox.sh` drives Apple's `container` CLI on
macOS; `podman.sh` is the Linux equivalent ([PODMAN.md](PODMAN.md)).

```sh
./container/sandbox.sh build-run
```

The checkout is mounted at `/workspace`. Everything else that should outlive the
container is mounted from `~/.lila-sandbox` on the host: `claude/`, `codex/`,
`pi/`, `local/` (`~/.local`, which also holds the simulator build and its SD
card), `platformio/` (`~/.platformio`: platforms, toolchains and the platform's
own Python env — about 11 GB for all five CI envs), and `cache/` (`~/.cache`,
where ESP-IDF's component manager clones what it needs when it rebuilds the
Arduino framework libraries). The rest of the home directory is discarded on
exit, so nothing is downloaded again on the next boot.

`build` stages a small context — this directory plus the repo's
`requirements.txt` files — so the Python packages are baked into the image.
Rebuild the image after changing a requirements file.

## What the image holds

| | Matches |
|---|---|
| Python 3.13 venv on `PATH` with pioarduino PlatformIO Core 6.1.19, the platform's build dependencies, and `requirements.txt` | `.github/workflows/ci.yml` |
| `clang-format` 21 (from PyPI) | `bin/clang-format-fix` |
| `libpcre3` for pioarduino's cppcheck (`pio check`); the image stays on bookworm for it | `ci.yml` cppcheck job |
| `cmake`, `ninja`, `g++` | `test/` (gtest), `test/tinta/run.sh`, the simulator |
| `zlib1g-dev` | simulator daemon and `freeink-emu` |
| `zip`, ImageMagick, Cairo | freeink-sdk host tests' fixtures, `scripts/convert_icon.py` |
| GitHub's SSH host keys | the `freeink-sdk` submodule's `git@github.com:` URL, over the forwarded agent |
| Claude Code, Codex, pi | `yolo` / `yolox` wrap the first two with permissions bypassed |

## Building lila

```sh
lila-setup                 # once per env; it persists in ~/.lila-sandbox/platformio
pio run -e default         # or x4pro, x4c, sticky, papermono, ...
./bin/clang-format-fix -g
pio check
cmake -S test -B build/test -G Ninja && cmake --build build/test && ctest --test-dir build/test
```

`lila-setup [env...]` initialises the submodule if needed, fetches each env's
PlatformIO packages, and pins the platform penv's core to 6.1.19 as CI does —
envs with `custom_sdkconfig` (`default`, `sticky`) otherwise fail mid-build. The
first `pio run` of an env builds the framework libraries (about 15 minutes);
after that a rebuild in a fresh container takes under a minute.

There is no USB passthrough, so `pio run -t upload` and the serial monitor run
on the host.

## Running lila in the simulator

The simulator is in `freeink-sdk/tools/simulator`; see
`freeink-sdk/docs/simulator.md`. Only the subtlepath fork of freeink-sdk has it,
which is why `.gitmodules` points there.

```sh
lila-sim build              # freeink-simd + freeink-emu, headless, into ~/.local/share/lila-sim
pio run -e default
lila-sim start              # .pio/build/default/firmware.bin as an X4
lila-sim capture screen.png
lila-sim press down confirm
lila-sim log --follow
lila-sim stop
```

`start` takes a PlatformIO env or a path to an image, with `--device NAME`
(default from the env: `x4pro` → X4PRO, `x4c` → X4CLASSIC, `sticky` → STICKY,
`papermono` → PAPERMONO, otherwise X4) and `--sd DIR` (default
`~/.local/share/lila-sim/sd`, also `LILA_SIM_SD`; put EPUBs there). The daemon
builds the card's FAT32 volume in RAM at full size, so `start` makes it 1024 MB
rather than the daemon's 8192 MB default; `LILA_SIM_SD_MB` changes it. It starts the
daemon paused, holds the power key, resumes, and releases the key after the
first panel refresh. Arguments after `--` go to the daemon, for example
`-- --seed 1`. Every other `lila-sim` command is passed to the SDK's
`freeink-sim` CLI, and `lila-sim emu-tool` runs `freeink-emu` (`info`, `boards`,
`boot`).

The daemon is built without SDL, so there is no window: use `capture`. It is
built outside the SDK tree so it never overwrites a macOS build of the same
checkout. Firmware bundles (`freeink-sim run`, `build-firmware.sh`) build `.so`
files into `freeink-sdk/tools/simulator/build/` as on any Linux host. The
browser simulator (`tools/simulator/web`) needs Emscripten and is not set up
here.
