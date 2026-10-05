#!/usr/bin/env python3
"""Convert BDF bitmap strikes into FreeInkUI BitmapFont data (PLAN.md section 5.3).

BDF is plain text and already 1-bit, so this is a lossless repack with no
FreeType dependency:

- Glyph rows are byte-aligned in BDF; DisplayTarget::drawGlyph reads bit
  `gy * width + gx` of one continuous MSB-first stream, so rows are repacked
  without padding. Blank borders are trimmed, which changes no pixel.
- xAdvance = DWIDTH, xOffset = BBX x, yOffset = -(BBX y + BBX height).
- ascent and yAdvance come from the converted subset, not FONT_ASCENT, so
  accented capitals never reach into the line above.
- The slots are the Tinta charset (tools/charset.py): U+0020..U+00FF, with the
  typographic punctuation drawn into C1 slots. Empty slots get zero-size glyphs.
- `--scale 2|3|4` enlarges a strike for display sizes: plainly, with Scale2x
  (`--smooth scale2x`, 2x only) or with hqx (`--smooth hqx`, hq2x/hq3x/hq4x).
  hqx blends; see scale_hqx() for how its output becomes 1-bit again.

Each strike becomes one .cpp defining `tinta::fonts::k<Name>`; one header,
Strikes.h, declares them all, so a strike is compiled once however many files
use it.

    # one strike
    python3 tools/bdf2freeink.py --bdf ../../x11/font-adobe-100dpi-1.0.4/timR18.bdf \\
        --name TimesRoman25 --out-dir build/fonts
    # everything in the manifest (what tools/gen-fonts.sh runs)
    python3 tools/bdf2freeink.py --manifest tools/fonts.manifest --out-dir src/fonts

Manifest BDF names resolve in lila's own strikes (BDF_DIRS: x11/ at the
repository root), so Tinta draws the same Times and Helvetica as the reader.
A manifest line with --px and --raster is an outline strike instead: the
font (a path relative to assets/fonts/) is rasterised by tools/otf2freeink.py, and this script re-runs itself in
build/ft-venv for the pinned FreeType. hqx comes from the pinned port in
build/hqx-venv (tools/hqx_tables.py). Each venv is installed on first use;
without it the strikes that need it fail to convert.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import shlex
import sys
import unicodedata
from dataclasses import dataclass, field

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
REPO = os.path.dirname(os.path.dirname(ROOT))
# Searched in order for a manifest BDF name; the two hold different sizes.
BDF_DIRS = [os.path.join(REPO, "x11", "font-crosspoint-100dpi"),
            os.path.join(REPO, "x11", "font-adobe-100dpi-1.0.4")]
OUTLINE_DIR = os.path.join(ROOT, "assets", "fonts")

GLYPH_STRUCT_BYTES = 8  # sizeof(FontGlyph): uint16 + five uint8, padded

# An hqx output pixel is ink when at least half of it is: coverage >= 128/255.
# Measured on Times Bold 17-34 px at hq2x/hq3x/hq4x, this keeps total ink
# within 0-2.3% of the source scaled by k*k and every glyph's box exactly k
# times the source box. Requiring more (>= 192) thins strokes by 1-4% and cuts
# box corners; requiring less (>= 64) thickens them by 3-8%.
HQX_INK_THRESHOLD = 128

SMOOTHING = ("none", "scale2x", "hqx")


class ConversionError(Exception):
    pass


# --- BDF ---------------------------------------------------------------------


@dataclass
class BdfGlyph:
    name: str
    encoding: int
    dwidth: int
    bbx: tuple[int, int, int, int]  # width, height, x offset, y offset (bottom, from baseline)
    rows: list[str]  # hex rows as written


@dataclass
class BdfFont:
    font: str = ""
    properties: dict[str, str | int] = field(default_factory=dict)
    glyphs: dict[int, BdfGlyph] = field(default_factory=dict)


def _property_value(raw: str) -> str | int:
    raw = raw.strip()
    if raw.startswith('"'):
        return raw[1:-1].replace('""', '"') if raw.endswith('"') else raw[1:]
    try:
        return int(raw)
    except ValueError:
        return raw


def parse_bdf(text: str, source: str = "<bdf>") -> BdfFont:
    font = BdfFont()
    lines = text.splitlines()
    i = 0
    glyph: dict | None = None

    def fail(msg: str):
        raise ConversionError(f"{source}:{i + 1}: {msg}")

    while i < len(lines):
        line = lines[i].strip()
        keyword, _, rest = line.partition(" ")
        if keyword == "FONT" and glyph is None:
            font.font = rest.strip()
        elif keyword == "STARTPROPERTIES":
            i += 1
            while i < len(lines) and not lines[i].startswith("ENDPROPERTIES"):
                key, _, value = lines[i].strip().partition(" ")
                font.properties[key] = _property_value(value)
                i += 1
        elif keyword == "STARTCHAR":
            glyph = {"name": rest.strip()}
        elif glyph is not None and keyword == "ENCODING":
            values = [int(v) for v in rest.split()]
            # "ENCODING -1 n" marks a glyph outside the standard encoding.
            glyph["encoding"] = values[1] if values[0] < 0 and len(values) > 1 else values[0]
        elif glyph is not None and keyword == "DWIDTH":
            glyph["dwidth"] = int(rest.split()[0])
        elif glyph is not None and keyword == "BBX":
            w, h, x, y = (int(v) for v in rest.split())
            glyph["bbx"] = (w, h, x, y)
        elif glyph is not None and keyword == "BITMAP":
            if "bbx" not in glyph:
                fail("BITMAP before BBX")
            height = glyph["bbx"][1]
            glyph["rows"] = [lines[i + 1 + r].strip() for r in range(height)]
            i += height
        elif keyword == "ENDCHAR":
            if glyph is None or "encoding" not in glyph or "bbx" not in glyph:
                fail("incomplete glyph")
            if "dwidth" not in glyph:
                fail(f"glyph {glyph['name']} has no DWIDTH")
            g = BdfGlyph(glyph["name"], glyph["encoding"], glyph["dwidth"], glyph["bbx"],
                         glyph.get("rows", []))
            if g.encoding >= 0:
                font.glyphs[g.encoding] = g
            glyph = None
        i += 1
    if not font.glyphs:
        raise ConversionError(f"{source}: no glyphs")
    return font


def read_bdf(path: str) -> BdfFont:
    with open(path, encoding="latin-1") as f:
        return parse_bdf(f.read(), path)


# --- glyph images ------------------------------------------------------------


@dataclass
class Glyph:
    """A glyph in FreeInkUI terms: pixels[y][x], offsets of the upper-left corner
    from the pen origin on the baseline (negative y is above)."""
    width: int
    height: int
    x_offset: int
    y_offset: int
    advance: int
    pixels: list[list[int]]

    @staticmethod
    def empty(advance: int = 0) -> "Glyph":
        return Glyph(0, 0, 0, 0, advance, [])


def glyph_from_bdf(g: BdfGlyph) -> Glyph:
    w, h, x, y = g.bbx
    pixels = []
    for row in g.rows:
        bits = len(row) * 4
        value = int(row, 16) if row else 0
        pixels.append([(value >> (bits - 1 - gx)) & 1 if gx < bits else 0 for gx in range(w)])
    return trim(Glyph(w, h, x, -(y + h), g.dwidth, pixels))


def trim(g: Glyph) -> Glyph:
    rows = [y for y in range(g.height) if any(g.pixels[y])]
    cols = [x for x in range(g.width) if any(g.pixels[y][x] for y in range(g.height))]
    if not rows:
        return Glyph.empty(g.advance)
    top, bottom, left, right = rows[0], rows[-1] + 1, cols[0], cols[-1] + 1
    return Glyph(right - left, bottom - top, g.x_offset + left, g.y_offset + top, g.advance,
                 [row[left:right] for row in g.pixels[top:bottom]])


def scale_plain(g: Glyph, k: int = 2) -> Glyph:
    pixels = []
    for row in g.pixels:
        wide = [p for p in row for _ in range(k)]
        pixels += [list(wide) for _ in range(k)]
    return Glyph(g.width * k, g.height * k, g.x_offset * k, g.y_offset * k, g.advance * k, pixels)


def scale2x(g: Glyph) -> Glyph:
    """Scale2x (AdvMAME2x): doubles each pixel, then cuts or fills each sub-pixel
    corner from its two edge neighbours, so diagonals stay diagonal instead of
    stepping in 2-pixel stairs. Never grows the bounding box: outside pixels have
    no ink neighbours on both sides."""
    w, h = g.width, g.height

    def at(x: int, y: int) -> int:
        return g.pixels[y][x] if 0 <= x < w and 0 <= y < h else 0

    pixels = [[0] * (w * 2) for _ in range(h * 2)]
    for y in range(h):
        for x in range(w):
            p = at(x, y)
            a, b, c, d = at(x, y - 1), at(x + 1, y), at(x - 1, y), at(x, y + 1)
            e0 = a if c == a and c != d and a != b else p
            e1 = b if a == b and a != c and b != d else p
            e2 = c if d == c and d != b and c != a else p
            e3 = d if b == d and b != a and d != c else p
            pixels[2 * y][2 * x], pixels[2 * y][2 * x + 1] = e0, e1
            pixels[2 * y + 1][2 * x], pixels[2 * y + 1][2 * x + 1] = e2, e3
    return trim(Glyph(w * 2, h * 2, g.x_offset * 2, g.y_offset * 2, g.advance * 2, pixels))


_hqx_tables: dict[int, list[list[int]]] | None = None


def hqx_tables() -> dict[int, list[list[int]]]:
    global _hqx_tables
    if _hqx_tables is None:
        import hqx_tables
        try:
            _hqx_tables = hqx_tables.load()
        except hqx_tables.HqxUnavailable as err:
            raise ConversionError(f"--smooth hqx needs the pinned hqx port: {err}") from None
    return _hqx_tables


def scale_hqx(g: Glyph, k: int, table: list[list[int]] | None = None) -> Glyph:
    """hq2x/hq3x/hq4x of a glyph rendered black on white, thresholded back to
    1-bit at HQX_INK_THRESHOLD. hqx reads each pixel's 3x3 neighbourhood, so a
    one-pixel white margin round the glyph is all the ink can reach; the
    neighbourhood indexes the table of the port's output for it."""
    from hqx_tables import NEIGHBOURS

    table = table if table is not None else hqx_tables()[k]
    w, h = g.width, g.height

    def at(x: int, y: int) -> int:
        return g.pixels[y][x] if 0 <= x < w and 0 <= y < h else 0

    pixels = [[0] * ((w + 2) * k) for _ in range((h + 2) * k)]
    for y in range(-1, h + 1):
        for x in range(-1, w + 1):
            mask = 0
            for bit, (dx, dy) in enumerate(NEIGHBOURS):
                if at(x + dx, y + dy):
                    mask |= 1 << bit
            if mask == 0:
                continue
            for j, coverage in enumerate(table[mask]):
                if coverage >= HQX_INK_THRESHOLD:
                    pixels[(y + 1) * k + j // k][(x + 1) * k + j % k] = 1
    return trim(Glyph((w + 2) * k, (h + 2) * k, (g.x_offset - 1) * k, (g.y_offset - 1) * k, g.advance * k,
                      pixels))


def enlarge(g: Glyph, k: int, smooth: str) -> Glyph:
    if k == 1:
        return g
    if smooth == "hqx":
        return scale_hqx(g, k)
    if smooth == "scale2x":
        return scale2x(g)
    return scale_plain(g, k)


def pack_bits(g: Glyph) -> bytes:
    out = bytearray()
    acc = nbits = 0
    for row in g.pixels:
        for p in row:
            acc = (acc << 1) | p
            nbits += 1
            if nbits == 8:
                out.append(acc)
                acc = nbits = 0
    if nbits:
        out.append(acc << (8 - nbits))
    return bytes(out)


# --- strikes -----------------------------------------------------------------


@dataclass
class StrikeGlyph:
    slot: int
    source: int | None
    offset: int
    glyph: Glyph


@dataclass
class Strike:
    first: int
    last: int
    glyphs: list[StrikeGlyph]
    bitmap: bytes
    ascent: int
    descent: int
    max_width: int
    max_height: int

    @property
    def y_advance(self) -> int:
        return self.ascent + self.descent

    def flash_bytes(self) -> int:
        return len(self.bitmap) + len(self.glyphs) * GLYPH_STRUCT_BYTES


def load_charset(path: str | None = None):
    path = path or os.path.join(TOOLS, "charset.py")
    spec = importlib.util.spec_from_file_location("tinta_charset", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _check_range(what: str, value: int, lo: int, hi: int):
    if not lo <= value <= hi:
        raise ConversionError(f"{what} = {value} does not fit its FontGlyph/BitmapFont field "
                              f"({lo}..{hi})")


def convert(bdf: BdfFont, charset, scale: int = 1, smooth: str = "none") -> Strike:
    if scale not in (1, 2, 3, 4):
        raise ConversionError("--scale must be 1, 2, 3 or 4")
    if smooth not in SMOOTHING:
        raise ConversionError(f"--smooth must be one of {', '.join(SMOOTHING)}")
    if smooth != "none" and scale == 1:
        raise ConversionError("--smooth needs --scale 2, 3 or 4")
    if smooth == "scale2x" and scale != 2:
        raise ConversionError("--smooth scale2x is 2x only")

    def source_glyph(source: int) -> Glyph:
        if source not in bdf.glyphs:
            raise ConversionError(f"{bdf.font}: no glyph for U+{source:04X}")
        return enlarge(glyph_from_bdf(bdf.glyphs[source]), scale, smooth)

    return assemble(source_glyph, charset)


def assemble(source_glyph, charset, only: str | None = None) -> Strike:
    """Packs one glyph per charset slot into a Strike, checking every field
    against FontGlyph and BitmapFont. `source_glyph(codepoint)` supplies the
    glyphs; slots whose source is not in `only` (when given) stay empty, as
    unmapped slots do. Line metrics come from the glyphs packed."""
    glyphs: list[StrikeGlyph] = []
    bitmap = bytearray()
    for slot, source in charset.slots():
        if source is None or (only is not None and chr(source) not in only):
            glyph = Glyph.empty()
        else:
            glyph = source_glyph(source)
        what = f"U+{source or slot:04X}"
        _check_range(f"{what} bitmap offset", len(bitmap), 0, 0xFFFF)
        _check_range(f"{what} width", glyph.width, 0, 255)
        _check_range(f"{what} height", glyph.height, 0, 255)
        _check_range(f"{what} xAdvance", glyph.advance, 0, 255)
        _check_range(f"{what} xOffset", glyph.x_offset, -128, 127)
        _check_range(f"{what} yOffset", glyph.y_offset, -128, 127)
        glyphs.append(StrikeGlyph(slot, source, len(bitmap), glyph))
        bitmap += pack_bits(glyph)

    inked = [g.glyph for g in glyphs if g.glyph.height > 0]
    ascent = max(-g.y_offset for g in inked)
    descent = max(g.y_offset + g.height for g in inked)
    strike = Strike(charset.FIRST, charset.LAST, glyphs, bytes(bitmap), ascent, max(descent, 0),
                    max(g.width for g in inked), max(g.height for g in inked))
    _check_range("ascent", strike.ascent, 0, 255)
    _check_range("yAdvance", strike.y_advance, 0, 255)
    return strike


# --- C++ output --------------------------------------------------------------


@dataclass
class Target:
    """Where generated code goes: the header the .cpp files include, as written
    in #include (its basename is the header's file name), and the namespace."""
    header: str = "fonts/Strikes.h"
    namespace: str = "tinta::fonts"
    manifest: str = "tools/fonts.manifest"


SMOOTH_NAMES = {"none": "plain", "scale2x": "Scale2x", "hqx": "hq{k}x"}


def _describe(bdf: BdfFont, scale: int, smooth: str) -> str:
    face = bdf.properties.get("FACE_NAME") or bdf.properties.get("FAMILY_NAME") or "?"
    px = bdf.properties.get("PIXEL_SIZE", "?")
    if scale == 1:
        return f"{face} {px} px"
    how = SMOOTH_NAMES[smooth].format(k=scale)
    return f"{face} {px} px scaled to {px * scale if isinstance(px, int) else '?'} px ({how})"


def _glyph_comment(sg: StrikeGlyph) -> str:
    if sg.source is None:
        return f"0x{sg.slot:02X} empty"
    name = unicodedata.name(chr(sg.source), "")
    if sg.source != sg.slot:
        return f"0x{sg.slot:02X} <- U+{sg.source:04X} {name}"
    return f"U+{sg.source:04X} {name}"


def emit_cpp(strike: Strike, symbol: str, bdf: BdfFont, source: str, scale: int,
             smooth: str, target: Target = Target()) -> str:
    out = []
    p = out.append
    p("// AUTO-GENERATED by tools/bdf2freeink.py — do not edit by hand.")
    p("//")
    p(f"// {_describe(bdf, scale, smooth)} from {source}")
    p(f"// ({bdf.font}).")
    p(f"// Tinta charset slots U+{strike.first:04X}..U+{strike.last:04X} (tools/charset.py), "
      "1 bpp; line metrics")
    p(f"// from the subset: ascent {strike.ascent}, descent {strike.descent}, "
      f"yAdvance {strike.y_advance}.")
    if smooth == "hqx":
        p(f"// hqx by the pinned port (tools/hqx_tables.py), ink where coverage >= "
          f"{HQX_INK_THRESHOLD}/255.")
    for key in ("COPYRIGHT", "NOTICE"):
        value = str(bdf.properties.get(key, "")).strip()
        if value:
            p(f"// {value}")
    p("")
    p(f'#include "{target.header}"')
    p("")
    p(f"namespace {target.namespace} {{")
    p("namespace {")
    p("")
    p("constexpr uint8_t kBitmap[] = {")
    for i in range(0, len(strike.bitmap), 16):
        p("    " + ", ".join(f"0x{b:02X}" for b in strike.bitmap[i:i + 16]) + ",")
    p("};")
    p("")
    p("constexpr freeink::ui::FontGlyph kGlyphs[] = {")
    for sg in strike.glyphs:
        g = sg.glyph
        p(f"    {{{sg.offset}, {g.width}, {g.height}, {g.advance}, {g.x_offset}, {g.y_offset}}},"
          f"  // {_glyph_comment(sg)}")
    p("};")
    p("")
    p("}  // namespace")
    p("")
    p(f"const freeink::ui::BitmapFont k{symbol} = {{kBitmap, kGlyphs, 0x{strike.first:04X}, "
      f"0x{strike.last:04X}, {strike.y_advance}, {strike.ascent}, {strike.max_width}, "
      f"{strike.max_height}, 1}};")
    p("")
    p(f"}}  // namespace {target.namespace}")
    return "\n".join(out) + "\n"


def emit_header(entries: list[tuple[str, str]], target: Target = Target()) -> str:
    out = [
        "#pragma once",
        "",
        f"// AUTO-GENERATED by tools/bdf2freeink.py from {target.manifest} — do not edit",
        "// by hand. One BitmapFont per strike, each defined in its own .cpp here.",
        "",
        "#include <FreeInkUIFont.h>",
        "",
        f"namespace {target.namespace} {{",
        "",
    ]
    for symbol, description in entries:
        out.append(f"extern const freeink::ui::BitmapFont k{symbol};  // {description}")
    out += ["", f"}}  // namespace {target.namespace}", ""]
    return "\n".join(out)


# --- manifest ----------------------------------------------------------------


@dataclass
class Entry:
    name: str
    bdf: str  # a BDF, or for an outline strike the font file
    scale: int = 1
    smooth: str = "none"
    px: int | None = None  # outline strikes: em size in pixels
    raster: str | None = None
    subset: str | None = None

    @property
    def outline(self) -> bool:
        return self.px is not None


def parse_manifest(path: str) -> list[Entry]:
    entries = []
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            words = shlex.split(line)
            if len(words) < 2:
                raise ConversionError(f"{path}:{number}: expected NAME BDF [options]")
            ap = argparse.ArgumentParser(prog=f"{path}:{number}", add_help=False)
            ap.add_argument("--scale", type=int, default=1)
            ap.add_argument("--smooth", default="none")
            ap.add_argument("--px", type=int)
            ap.add_argument("--raster")
            ap.add_argument("--subset")
            opts = ap.parse_args(words[2:])
            if not re.fullmatch(r"[A-Z][A-Za-z0-9]*", words[0]):
                raise ConversionError(f"{path}:{number}: bad strike name {words[0]!r}")
            if (opts.px is None) != (opts.raster is None):
                raise ConversionError(f"{path}:{number}: an outline strike needs both --px and --raster")
            if opts.px is not None and (opts.scale != 1 or opts.smooth != "none"):
                raise ConversionError(f"{path}:{number}: --scale/--smooth apply to BDF strikes only")
            entries.append(Entry(words[0], words[1], opts.scale, opts.smooth, opts.px, opts.raster, opts.subset))
    names = [e.name for e in entries]
    if len(set(names)) != len(names):
        raise ConversionError(f"{path}: duplicate strike names")
    return entries


def _relative(path: str) -> str:
    for base in (ROOT, REPO):
        rel = os.path.relpath(os.path.abspath(path), base)
        if not rel.startswith(".."):
            return rel
    return path


def find_bdf(name: str, dirs: list[str] | None = None) -> str:
    """The first of dirs (default BDF_DIRS) holding the BDF called name."""
    for d in dirs or BDF_DIRS:
        path = os.path.join(d, name)
        if os.path.isfile(path):
            return path
    raise ConversionError(f"{name}: not found in {', '.join(_relative(d) for d in dirs or BDF_DIRS)}")


def build(entries: list[Entry], bdf_dirs: list[str] | None, out_dir: str, charset, quiet: bool = False,
          target: Target = Target()) -> int:
    os.makedirs(out_dir, exist_ok=True)
    cache: dict[str, BdfFont] = {}
    described = []
    total = 0
    rows = []
    outlines: dict[str, object] = {}
    for e in entries:
        if e.outline:
            import otf2freeink
            import freetype

            path = os.path.join(OUTLINE_DIR, e.bdf)
            if path not in outlines:
                outlines[path] = otf2freeink.load_font(freetype, path)
            font = outlines[path]
            try:
                strike = otf2freeink.convert(freetype, font, e.px, e.raster, charset,
                                             otf2freeink.SUBSETS.get(e.subset))
            except ConversionError as err:
                raise ConversionError(f"{e.name} ({e.bdf}): {err}") from None
            bdf = otf2freeink.describe(font, e.px, e.raster, freetype)
            with open(os.path.join(out_dir, e.name + ".cpp"), "w", encoding="utf-8") as f:
                f.write(emit_cpp(strike, e.name, bdf, _relative(path), 1, "none", target))
            described.append((e.name, f"{bdf.properties['FACE_NAME']} {e.px} px (FreeType, {e.raster}), {e.bdf}"))
            total += strike.flash_bytes()
            rows.append((e.name, strike))
            continue
        path = find_bdf(e.bdf, bdf_dirs)
        if path not in cache:
            cache[path] = read_bdf(path)
        bdf = cache[path]
        try:
            strike = convert(bdf, charset, e.scale, e.smooth)
        except ConversionError as err:
            raise ConversionError(f"{e.name} ({e.bdf}): {err}") from None
        with open(os.path.join(out_dir, e.name + ".cpp"), "w", encoding="utf-8") as f:
            f.write(emit_cpp(strike, e.name, bdf, _relative(path), e.scale, e.smooth, target))
        described.append((e.name, f"{_describe(bdf, e.scale, e.smooth)}, {e.bdf}"))
        total += strike.flash_bytes()
        rows.append((e.name, strike))
    with open(os.path.join(out_dir, os.path.basename(target.header)), "w", encoding="utf-8") as f:
        f.write(emit_header(described, target))
    if not quiet:
        for name, s in rows:
            print(f"  {name:24} yAdvance {s.y_advance:3}  ascent {s.ascent:3}  "
                  f"bitmap {len(s.bitmap):6} B  flash {s.flash_bytes():6} B")
        print(f"{len(rows)} strikes, {total} B ({total / 1024:.1f} KB) of font data in {out_dir}")
    return total


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", help="convert every strike listed in this manifest")
    ap.add_argument("--bdf", help="convert one BDF file")
    ap.add_argument("--name", help="strike name for --bdf, e.g. TimesRoman25 -> kTimesRoman25")
    ap.add_argument("--scale", type=int, default=1, choices=(1, 2, 3, 4))
    ap.add_argument("--smooth", default="none", choices=SMOOTHING)
    ap.add_argument("--charset", help="charset module (default tools/charset.py)")
    ap.add_argument("--bdf-dir", action="append",
                    help="where manifest BDF names resolve; repeat to search several, in order "
                         "(default lila's x11/font-crosspoint-100dpi, then x11/font-adobe-100dpi-1.0.4)")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--header", default=Target.header,
                    help="header as the generated .cpp files #include it (default fonts/Strikes.h)")
    ap.add_argument("--namespace", default=Target.namespace,
                    help="C++ namespace of the strikes (default tinta::fonts)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    charset = load_charset(args.charset)
    try:
        if args.manifest:
            entries = parse_manifest(args.manifest)
            manifest = _relative(args.manifest)
            outline = [e.name for e in entries if e.outline]
            if outline:
                import otf2freeink
                otf2freeink.ensure_freetype(__file__, f"{manifest} ({', '.join(outline)})")
        elif args.bdf and args.name:
            entries = [Entry(args.name, os.path.abspath(args.bdf), args.scale, args.smooth)]
            manifest = "the command line"
        else:
            ap.error("give --manifest, or --bdf with --name")
        build(entries, args.bdf_dir, args.out_dir, charset, args.quiet,
              Target(args.header, args.namespace, manifest))
    except ConversionError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
