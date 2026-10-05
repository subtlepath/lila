#!/usr/bin/env python3
"""hqx lookup tables for 1-bit glyphs.

hq2x/hq3x/hq4x decide each output block from the source pixel's 3x3
neighbourhood. A 1-bit glyph has only 512 such neighbourhoods, so scaling one
is a table lookup: this module runs the pinned hqx port (PyPI hqx 1.0, a
pure-Python port of Maxim Stepin's hqx, LGPL-2.1) once per neighbourhood,
black on white, and records how much ink each output pixel gets.

The port is installed into build/hqx-venv from tools/hqx-requirements.txt
(exact version and wheel hash). Only its algorithm modules are loaded; its
package __init__ needs a Pillow that no longer exists.

    python3 tools/hqx_tables.py --setup   # create build/hqx-venv
    python3 tools/hqx_tables.py           # print the tables as JSON
"""

from __future__ import annotations

import json
import os
import subprocess
import sys

HQX_VERSION = "1.0"

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
# TINTA_HQX_VENV moves it, e.g. to check what a machine without it sees.
VENV = os.environ.get("TINTA_HQX_VENV") or os.path.join(ROOT, "build", "hqx-venv")
REQUIREMENTS = os.path.join(TOOLS, "hqx-requirements.txt")

# Neighbourhood bit i is context cell i, in the port's order.
NEIGHBOURS = [(-1, -1), (0, -1), (1, -1), (-1, 0), (0, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]

BLACK = 0x000000
WHITE = 0xFFFFFF


class HqxUnavailable(RuntimeError):
    pass


def venv_python() -> str:
    return os.path.join(VENV, "bin", "python3")


def _shown(path: str) -> str:
    rel = os.path.relpath(path, ROOT)
    return path if rel.startswith("..") else rel


def setup() -> None:
    """Create build/hqx-venv with the pinned port, or raise HqxUnavailable."""
    try:
        if not os.path.exists(venv_python()):
            subprocess.run([sys.executable, "-m", "venv", VENV], check=True, capture_output=True, text=True)
        subprocess.run([venv_python(), "-m", "pip", "install", "--quiet", "--disable-pip-version-check",
                        "--no-deps", "--require-hashes", "-r", REQUIREMENTS],
                       check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as err:
        detail = getattr(err, "stderr", "") or str(err)
        raise HqxUnavailable(
            f"cannot install hqx=={HQX_VERSION} into {_shown(VENV)} "
            f"(needs network access to PyPI the first time):\n{detail.strip()}") from None


def _load_port():
    """The port's hq2x/hq3x/hq4x modules, without running hqx/__init__.py."""
    import importlib.util
    import types
    from importlib.metadata import PackageNotFoundError, version

    try:
        found = version("hqx")
    except PackageNotFoundError:
        raise HqxUnavailable("hqx is not installed in this interpreter") from None
    if found != HQX_VERSION:
        raise HqxUnavailable(f"hqx {found} is installed, {HQX_VERSION} is pinned")
    spec = importlib.util.find_spec("hqx")
    package = types.ModuleType("hqx")
    package.__path__ = list(spec.submodule_search_locations)
    sys.modules["hqx"] = package
    import hqx.algor_hq2x
    import hqx.algor_hq3x
    import hqx.algor_hq4x
    return {2: hqx.algor_hq2x.hq2x_pixel, 3: hqx.algor_hq3x.hq3x_pixel, 4: hqx.algor_hq4x.hq4x_pixel}


def compute() -> dict[int, list[list[int]]]:
    """{scale: [512 x [k*k ink levels, 0 = paper .. 255 = full ink]]}, output pixels row-major."""
    port = _load_port()
    tables = {}
    for scale, pixel in port.items():
        rows = []
        for mask in range(512):
            context = [BLACK if mask >> i & 1 else WHITE for i in range(9)]
            # Inputs are grey, so every channel of every blend is the same.
            rows.append([255 - (rgb & 0xFF) for rgb in pixel(context)])
        tables[scale] = rows
    return tables


def load() -> dict[int, list[list[int]]]:
    """The tables, computed inside build/hqx-venv (created on first use)."""
    if not os.path.exists(venv_python()):
        setup()
    result = subprocess.run([venv_python(), os.path.abspath(__file__)], capture_output=True, text=True)
    if result.returncode != 0:
        # A stale or broken venv: reinstall once, then give up loudly.
        setup()
        result = subprocess.run([venv_python(), os.path.abspath(__file__)], capture_output=True, text=True)
        if result.returncode != 0:
            raise HqxUnavailable(f"hqx tables failed in {_shown(VENV)}:\n"
                                 f"{result.stderr.strip()}")
    return {int(k): v for k, v in json.loads(result.stdout).items()}


def main() -> int:
    if sys.argv[1:] == ["--setup"]:
        try:
            setup()
        except HqxUnavailable as err:
            print(f"error: {err}", file=sys.stderr)
            return 1
        print(f"hqx {HQX_VERSION} ready in {_shown(VENV)}")
        return 0
    try:
        tables = compute()
    except HqxUnavailable as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    json.dump(tables, sys.stdout, separators=(",", ":"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
