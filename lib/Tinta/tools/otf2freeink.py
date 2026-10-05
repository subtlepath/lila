#!/usr/bin/env python3
"""Rasterise outline fonts (OpenType, TrueType, Type 1) into FreeInkUI
BitmapFont data — an experiment beside bdf2freeink.py, whose BDF strikes ship.

Same charset slots, field checks, subset-derived line metrics and C++ output
as bdf2freeink.py; only the glyphs come from FreeType instead of a BDF. The em
size is given in pixels (--px), as BDF PIXEL_SIZE is, so Times at 34 px here
and the Adobe 34 px strike have the same body.

Rasterisations (--raster), all 1-bit:
  hinted    FreeType monochrome with the font's own hints (TrueType bytecode,
            or the CFF / Type 1 hinter)
  autohint  FreeType monochrome with FreeType's auto-hinter
  coverage  unhinted outline at its exact area coverage, ink where at least
            half a pixel is covered: supersample-then-threshold taken to its
            limit. Advances are rounded to whole pixels.

FreeType comes from freetype-py in build/ft-venv (tools/ft-requirements.txt);
the script re-runs itself there, installing it on first use.

    python3 tools/otf2freeink.py --font texgyretermes-bold.otf --px 87 --raster coverage \\
        --name Termes87 --out-dir build/outline
    python3 tools/otf2freeink.py --manifest tools/host/outline.manifest --out-dir build/outline \\
        --header OutlineStrikes.h --namespace tinta::outline
    python3 tools/otf2freeink.py --sizes FONT RASTER 87 102 120   # bitmap bytes, limits ignored
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import bdf2freeink as b  # noqa: E402

# TINTA_FT_VENV_PATH moves it, e.g. to check what a machine without it sees.
VENV = os.environ.get("TINTA_FT_VENV_PATH") or os.path.join(ROOT, "build", "ft-venv")
REQUIREMENTS = os.path.join(TOOLS, "ft-requirements.txt")
FREETYPE_VERSION = (2, 13, 2)
RASTERS = ("hinted", "autohint", "coverage")
COVERAGE_THRESHOLD = 128  # of 255, as bdf2freeink.HQX_INK_THRESHOLD

# Enough for headwords: letters, Spanish accents and the punctuation a
# headword or short phrase carries. The 102 px and larger experiments use it.
HEADWORD_SUBSET = ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                   "áéíóúüñÁÉÍÓÚÜÑ"
                   " .,;:?!¿¡-'’—")
SUBSETS = {"headword": HEADWORD_SUBSET}


def venv_python(why: str = "this") -> str:
    """build/ft-venv's interpreter, creating the venv first if needed; exits
    with an error if that cannot work."""
    python = os.path.join(VENV, "bin", "python3")
    if os.path.exists(python) and subprocess.run([python, "-c", "import freetype"], capture_output=True).returncode == 0:
        return python  # ensure_freetype() still checks the FreeType version there
    try:
        if not os.path.exists(python):
            subprocess.run([sys.executable, "-m", "venv", VENV], check=True, capture_output=True)
        subprocess.run([python, "-m", "pip", "install", "--quiet", "--disable-pip-version-check", "--no-deps",
                        "-r", REQUIREMENTS], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as err:
        detail = getattr(err, "stderr", "") or str(err)
        sys.exit(f"error: {why} needs FreeType {'.'.join(map(str, FREETYPE_VERSION))} from freetype-py in "
                 f"build/ft-venv, and it cannot be installed (needs PyPI the first time):\n{detail.strip()}")
    return python


def ensure_freetype(script: str, why: str = "this"):
    """Imports freetype, re-running `script` (with the same arguments) inside
    build/ft-venv if needed; exits with an error if that cannot work."""
    try:
        import freetype
    except ImportError:
        freetype = None
    if freetype is not None:
        if freetype.version() != FREETYPE_VERSION:
            sys.exit(f"error: FreeType {freetype.version()} found, "
                     f"{'.'.join(map(str, FREETYPE_VERSION))} required")
        return freetype
    if os.environ.get("TINTA_FT_VENV") == "1":
        sys.exit(f"error: freetype-py is not importable in {VENV}, which {why} needs")
    python = venv_python(why)
    env = dict(os.environ, TINTA_FT_VENV="1")
    os.execve(python, [python, os.path.abspath(script)] + sys.argv[1:], env)


@dataclass
class OutlineFont:
    path: str
    face: object  # freetype.Face
    family: str
    style: str
    notice: str


def _notice(path: str, face) -> str:
    """The font's copyright line: sfnt name 0, or a Type 1 header comment."""
    try:
        for i in range(face.sfnt_name_count):
            name = face.get_sfnt_name(i)
            if name.name_id == 0 and name.platform_id == 3:
                return name.string.decode("utf-16-be").strip()
    except Exception:  # Type 1 fonts have no sfnt names
        pass
    with open(path, "rb") as f:
        head = f.read(4096).decode("latin-1")
    for line in head.splitlines():
        if "Copyright" in line:
            return line.lstrip("% ").strip()
    return ""


def load_font(freetype, path: str) -> OutlineFont:
    face = freetype.Face(path)
    return OutlineFont(path, face, face.family_name.decode("latin-1"), face.style_name.decode("latin-1"),
                       _notice(path, face))


def rasterise(freetype, font: OutlineFont, cp: int, px: int, raster: str) -> b.Glyph:
    face = font.face
    face.set_pixel_sizes(0, px)
    index = face.get_char_index(cp)
    if index == 0:
        raise b.ConversionError(f"{os.path.basename(font.path)}: no glyph for U+{cp:04X}")
    if raster == "coverage":
        face.load_glyph(index, freetype.FT_LOAD_NO_HINTING)
        face.glyph.render(freetype.FT_RENDER_MODE_NORMAL)
        advance = round(face.glyph.linearHoriAdvance / 65536)
    else:
        flags = freetype.FT_LOAD_TARGET_MONO
        if raster == "autohint":
            flags |= freetype.FT_LOAD_FORCE_AUTOHINT
        face.load_glyph(index, flags)
        face.glyph.render(freetype.FT_RENDER_MODE_MONO)
        advance = round(face.glyph.advance.x / 64)
    bitmap = face.glyph.bitmap
    w, h, pitch, buffer = bitmap.width, bitmap.rows, bitmap.pitch, bitmap.buffer
    if raster == "coverage":
        pixels = [[1 if buffer[y * pitch + x] >= COVERAGE_THRESHOLD else 0 for x in range(w)] for y in range(h)]
    else:
        pixels = [[(buffer[y * pitch + (x >> 3)] >> (7 - (x & 7))) & 1 for x in range(w)] for y in range(h)]
    return b.trim(b.Glyph(w, h, face.glyph.bitmap_left, -face.glyph.bitmap_top, advance, pixels))


def describe(font: OutlineFont, px: int, raster: str, freetype) -> b.BdfFont:
    """A stand-in BdfFont carrying what bdf2freeink.emit_cpp() prints."""
    shown = {"hinted": "monochrome, font hints", "autohint": "monochrome, auto-hinter",
             "coverage": f"unhinted, coverage >= {COVERAGE_THRESHOLD}/255"}[raster]
    version = ".".join(map(str, freetype.version()))
    fake = b.BdfFont()
    fake.font = f"{os.path.basename(font.path)}, FreeType {version}, {shown}"
    fake.properties = {"FACE_NAME": f"{font.family} {font.style}", "PIXEL_SIZE": px, "COPYRIGHT": font.notice}
    return fake


def convert(freetype, font: OutlineFont, px: int, raster: str, charset, only: str | None = None) -> b.Strike:
    if raster not in RASTERS:
        raise b.ConversionError(f"--raster must be one of {', '.join(RASTERS)}")
    return b.assemble(lambda cp: rasterise(freetype, font, cp, px, raster), charset, only)


def bitmap_bytes(freetype, font: OutlineFont, px: int, raster: str, charset,
                 only: str | None = None) -> tuple[int, bool]:
    """(bytes the strike's bitmap would take, whether every slot's start offset
    fits FontGlyph's 16 bits), without stopping at the limit."""
    total = 0
    fits = True
    for _, source in charset.slots():
        fits = fits and total <= 0xFFFF
        if source is not None and (only is None or chr(source) in only):
            total += len(b.pack_bits(rasterise(freetype, font, source, px, raster)))
    return total, fits


@dataclass
class Entry:
    name: str
    font: str
    px: int
    raster: str
    subset: str | None


def parse_manifest(path: str) -> list[Entry]:
    """Lines `font ALIAS PATH` name a font; `NAME ALIAS-or-PATH --px N --raster R [--subset S]`
    make a strike."""
    aliases: dict[str, str] = {}
    entries = []
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            words = shlex.split(line)
            if words[0] == "font" and len(words) == 3:
                aliases[words[1]] = words[2]
                continue
            ap = argparse.ArgumentParser(prog=f"{path}:{number}", add_help=False)
            ap.add_argument("--px", type=int, required=True)
            ap.add_argument("--raster", choices=RASTERS, required=True)
            ap.add_argument("--subset", choices=sorted(SUBSETS))
            opts = ap.parse_args(words[2:])
            entries.append(Entry(words[0], aliases.get(words[1], words[1]), opts.px, opts.raster, opts.subset))
    return entries


def build(freetype, entries: list[Entry], out_dir: str, charset, target: b.Target, quiet: bool) -> None:
    os.makedirs(out_dir, exist_ok=True)
    fonts: dict[str, OutlineFont] = {}
    described = []
    for e in entries:
        if e.font not in fonts:
            fonts[e.font] = load_font(freetype, e.font)
        font = fonts[e.font]
        try:
            strike = convert(freetype, font, e.px, e.raster, charset, SUBSETS.get(e.subset))
        except b.ConversionError as err:
            raise b.ConversionError(f"{e.name}: {err}") from None
        fake = describe(font, e.px, e.raster, freetype)
        with open(os.path.join(out_dir, e.name + ".cpp"), "w", encoding="utf-8") as f:
            f.write(b.emit_cpp(strike, e.name, fake, font.path, 1, "none", target))
        subset = f", {e.subset} subset" if e.subset else ""
        described.append((e.name, f"{fake.properties['FACE_NAME']} {e.px} px, {e.raster}{subset}"))
        if not quiet:
            print(f"  {e.name:24} yAdvance {strike.y_advance:3}  ascent {strike.ascent:3}  "
                  f"bitmap {len(strike.bitmap):6} B  flash {strike.flash_bytes():6} B")
    with open(os.path.join(out_dir, os.path.basename(target.header)), "w", encoding="utf-8") as f:
        f.write(b.emit_header(described, target))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest")
    ap.add_argument("--font")
    ap.add_argument("--px", type=int)
    ap.add_argument("--raster", choices=RASTERS, default="coverage")
    ap.add_argument("--subset", choices=sorted(SUBSETS))
    ap.add_argument("--name")
    ap.add_argument("--sizes", nargs="+", metavar="ARG",
                    help="FONT RASTER PX...: print bitmap bytes, full charset and headword subset")
    ap.add_argument("--out-dir")
    ap.add_argument("--header", default=b.Target.header)
    ap.add_argument("--namespace", default=b.Target.namespace)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    freetype = ensure_freetype(__file__)
    charset = b.load_charset()
    try:
        if args.sizes:
            font = load_font(freetype, args.sizes[0])
            for px in map(int, args.sizes[2:]):
                full, full_fits = bitmap_bytes(freetype, font, px, args.sizes[1], charset)
                head, head_fits = bitmap_bytes(freetype, font, px, args.sizes[1], charset, HEADWORD_SUBSET)
                print(f"{px:4} px  full charset {full:6} B {'fits' if full_fits else 'TOO BIG'}  "
                      f"headword subset {head:6} B {'fits' if head_fits else 'TOO BIG'}")
            return 0
        if not args.out_dir:
            ap.error("--out-dir is required")
        if args.manifest:
            entries = parse_manifest(args.manifest)
            target = b.Target(args.header, args.namespace, os.path.relpath(args.manifest, ROOT))
        elif args.font and args.px and args.name:
            entries = [Entry(args.name, args.font, args.px, args.raster, args.subset)]
            target = b.Target(args.header, args.namespace, "the command line")
        else:
            ap.error("give --manifest, --sizes, or --font with --px and --name")
        build(freetype, entries, args.out_dir, charset, target, args.quiet)
    except b.ConversionError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
