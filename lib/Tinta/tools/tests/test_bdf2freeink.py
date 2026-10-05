"""Tests for tools/bdf2freeink.py.

    python3 -m unittest discover -s tools/tests

Glyph checks read the expected pixels straight from the vendored BDF text with
a separate minimal reader, so a parser bug cannot hide behind itself.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import bdf2freeink as b  # noqa: E402
import charset  # noqa: E402


def bdf_char(path: str, encoding: int):
    """(DWIDTH, BBX, pixel rows as strings of '#'/'.') for one ENCODING, read naively."""
    with open(path, encoding="latin-1") as f:
        lines = f.read().splitlines()
    for i, line in enumerate(lines):
        if line == f"ENCODING {encoding}":
            j = i
            while not lines[j].startswith("DWIDTH"):
                j += 1
            dwidth = int(lines[j].split()[1])
            while not lines[j].startswith("BBX"):
                j += 1
            w, h, x, y = (int(v) for v in lines[j].split()[1:])
            while lines[j] != "BITMAP":
                j += 1
            rows = []
            for r in range(h):
                hexrow = lines[j + 1 + r]
                bits = bin(int(hexrow, 16))[2:].zfill(len(hexrow) * 4)
                rows.append("".join("#" if c == "1" else "." for c in bits[:w]))
            return dwidth, (w, h, x, y), rows
    raise KeyError(encoding)


def unpack(strike: b.Strike, slot: int):
    """The converted glyph for `slot` as (FontGlyph fields, pixel rows), decoded the
    way DisplayTarget::drawGlyph reads it."""
    sg = strike.glyphs[slot - strike.first]
    g = sg.glyph
    rows = []
    for gy in range(g.height):
        row = ""
        for gx in range(g.width):
            bit = gy * g.width + gx
            byte = strike.bitmap[sg.offset + (bit >> 3)]
            row += "#" if (byte >> (7 - (bit & 7))) & 1 else "."
        rows.append(row)
    return (sg.offset, g.width, g.height, g.advance, g.x_offset, g.y_offset), rows


def convert_file(name: str, **kwargs) -> b.Strike:
    return b.convert(b.read_bdf(b.find_bdf(name)), charset, **kwargs)


class GlyphsMatchBdf(unittest.TestCase):
    CASES = [
        ("timR18.bdf", "Ñ"), ("timR18.bdf", "¿"), ("timR18.bdf", "—"), ("timR18.bdf", "g"),
        ("timB24.bdf", "Ñ"), ("timI16.bdf", "¡"), ("helvR14.bdf", "€"), ("helvO14.bdf", "ü"),
        ("helvB10.bdf", "“"), ("timR21.bdf", "Á"),
    ]

    def test_bitmaps_and_metrics(self):
        for name, ch in self.CASES:
            with self.subTest(font=name, char=ch):
                strike = convert_file(name)
                cp = ord(ch)
                slot = charset.REMAP.get(cp, cp)
                dwidth, (w, h, x, y), rows = bdf_char(b.find_bdf(name), cp)
                (_, gw, gh, adv, xoff, yoff), got = unpack(strike, slot)
                self.assertEqual(adv, dwidth)
                # These glyphs have tight boxes in the BDF, so trimming is a no-op.
                self.assertEqual((gw, gh), (w, h))
                self.assertEqual(xoff, x)
                self.assertEqual(yoff, -(y + h))
                self.assertEqual(got, rows)

    def test_every_glyph_draws_the_same_pixels(self):
        # Trimmed blank borders must not move a single pixel.
        for name in ("timR21.bdf", "timB21.bdf", "helvR18.bdf"):
            strike = convert_file(name)
            for sg in strike.glyphs:
                if sg.source is None:
                    continue
                dwidth, (w, h, x, y), rows = bdf_char(b.find_bdf(name), sg.source)
                want = {(x + gx, -(y + h) + gy) for gy, r in enumerate(rows)
                        for gx, c in enumerate(r) if c == "#"}
                fields, got_rows = unpack(strike, sg.slot)
                gx0, gy0 = fields[4], fields[5]
                got = {(gx0 + gx, gy0 + gy) for gy, r in enumerate(got_rows)
                       for gx, c in enumerate(r) if c == "#"}
                self.assertEqual(got, want, f"{name} U+{sg.source:04X}")
                self.assertEqual(fields[3], dwidth)


class Charset(unittest.TestCase):
    def setUp(self):
        self.strike = convert_file("timR18.bdf")

    def test_range_and_slot_count(self):
        self.assertEqual((self.strike.first, self.strike.last), (0x20, 0xFF))
        self.assertEqual(len(self.strike.glyphs), 0xFF - 0x20 + 1)

    def test_remapped_slots_hold_their_source_glyph(self):
        for cp, slot in charset.REMAP.items():
            dwidth, _, rows = bdf_char(b.find_bdf("timR18.bdf"), cp)
            fields, got = unpack(self.strike, slot)
            self.assertEqual(got, rows, f"slot 0x{slot:02X}")
            self.assertEqual(fields[3], dwidth)

    def test_empty_slots_are_zero_size(self):
        for slot in [0x7F, 0x81, 0x8D, 0x90, 0x98, 0x9F]:
            fields, rows = unpack(self.strike, slot)
            self.assertEqual(fields[1:], (0, 0, 0, 0, 0))
            self.assertEqual(rows, [])

    def test_ascent_clears_accented_capitals(self):
        bdf = b.read_bdf(b.find_bdf("timR18.bdf"))
        self.assertGreater(self.strike.ascent, bdf.properties["FONT_ASCENT"])
        for ch in "ÁÉÍÓÚÑÜ":
            g = self.strike.glyphs[ord(ch) - 0x20].glyph
            self.assertLessEqual(-g.y_offset, self.strike.ascent, ch)
        for sg in self.strike.glyphs:
            g = sg.glyph
            if g.height:
                self.assertGreaterEqual(g.y_offset, -self.strike.ascent)
                self.assertLessEqual(g.y_offset + g.height, self.strike.descent)
        self.assertEqual(self.strike.y_advance, self.strike.ascent + self.strike.descent)


class Doubling(unittest.TestCase):
    def test_plain_doubles_every_pixel_and_metric(self):
        one = convert_file("timB18.bdf")
        two = convert_file("timB18.bdf", scale=2)
        self.assertEqual(two.ascent, one.ascent * 2)
        self.assertEqual(two.descent, one.descent * 2)
        for ch in "Ñ¿g—":
            slot = charset.REMAP.get(ord(ch), ord(ch))
            f1, r1 = unpack(one, slot)
            f2, r2 = unpack(two, slot)
            self.assertEqual(f2[1:], tuple(v * 2 for v in f1[1:]))
            doubled = [r for row in r1 for r in ("".join(c * 2 for c in row),) * 2]
            self.assertEqual(r2, doubled)

    def test_scale2x_rounds_a_diagonal(self):
        g = b.Glyph(2, 2, 0, -2, 3, [[1, 0], [0, 1]])
        out = b.scale2x(g)
        self.assertEqual(out.pixels, [
            [1, 1, 0, 0],
            [1, 1, 1, 0],
            [0, 1, 1, 1],
            [0, 0, 1, 1],
        ])
        self.assertEqual((out.x_offset, out.y_offset, out.advance), (0, -4, 6))

    def test_scale2x_keeps_squares_and_lines(self):
        g = b.Glyph(3, 1, 1, -1, 4, [[1, 1, 1]])
        self.assertEqual(b.scale2x(g).pixels, b.scale_plain(g).pixels)
        lone = b.Glyph(1, 1, 0, -1, 2, [[1]])
        self.assertEqual(b.scale2x(lone).pixels, [[1, 1], [1, 1]])

    def test_scale2x_stays_inside_the_doubled_box(self):
        one = convert_file("timB18.bdf")
        smooth = convert_file("timB18.bdf", scale=2, smooth="scale2x")
        for sg1, sg2 in zip(one.glyphs, smooth.glyphs):
            g1, g2 = sg1.glyph, sg2.glyph
            if not g1.height:
                continue
            self.assertGreaterEqual(g2.x_offset, g1.x_offset * 2)
            self.assertGreaterEqual(g2.y_offset, g1.y_offset * 2)
            self.assertLessEqual(g2.x_offset + g2.width, (g1.x_offset + g1.width) * 2)
            self.assertLessEqual(g2.y_offset + g2.height, (g1.y_offset + g1.height) * 2)
            self.assertEqual(g2.advance, g1.advance * 2)

    def test_smooth_needs_scale(self):
        with self.assertRaises(b.ConversionError):
            convert_file("timB18.bdf", smooth="scale2x")


def synthetic_bdf(glyphs: list[tuple[int, int, int, int]]) -> str:
    """BDF text with one solid glyph per (encoding, width, height, dwidth)."""
    out = ["STARTFONT 2.1", "FONT -Test-Synthetic", "STARTPROPERTIES 2", "FONT_ASCENT 10",
           "FONT_DESCENT 2", "ENDPROPERTIES", f"CHARS {len(glyphs)}"]
    for enc, w, h, dw in glyphs:
        hexdigits = (w + 7) // 8 * 2
        row = f"{(1 << w) - 1 << (hexdigits * 4 - w):0{hexdigits}X}"
        out += [f"STARTCHAR c{enc}", f"ENCODING {enc}", "SWIDTH 500 0", f"DWIDTH {dw} 0",
                f"BBX {w} {h} 0 0", "BITMAP"] + [row] * h + ["ENDCHAR"]
    out.append("ENDFONT")
    return "\n".join(out)


def all_slots(w: int, h: int, dw: int, big: dict[int, tuple[int, int, int]] | None = None):
    big = big or {}
    cps = [src for _, src in charset.slots() if src is not None]
    return [(cp, *big.get(cp, (w, h, dw))) for cp in cps]


class Limits(unittest.TestCase):
    def test_bitmap_offset_overflow_fails(self):
        # About 200 glyphs of 60x60 px = 450 bytes each: offsets pass 65535.
        bdf = b.parse_bdf(synthetic_bdf(all_slots(60, 60, 62)))
        with self.assertRaisesRegex(b.ConversionError, "bitmap offset"):
            b.convert(bdf, charset)

    def test_metric_overflow_fails(self):
        bdf = b.parse_bdf(synthetic_bdf(all_slots(4, 4, 5, {ord("M"): (4, 4, 300)})))
        with self.assertRaisesRegex(b.ConversionError, "xAdvance"):
            b.convert(bdf, charset)
        bdf = b.parse_bdf(synthetic_bdf(all_slots(4, 70, 5)))
        with self.assertRaisesRegex(b.ConversionError, "yOffset"):
            b.convert(bdf, charset, scale=2)
        bdf = b.parse_bdf(synthetic_bdf(all_slots(4, 130, 5)))
        with self.assertRaisesRegex(b.ConversionError, "height"):
            b.convert(bdf, charset, scale=2)

    def test_missing_glyph_fails(self):
        glyphs = [g for g in all_slots(4, 4, 5) if g[0] != 0x2014]
        bdf = b.parse_bdf(synthetic_bdf(glyphs))
        with self.assertRaisesRegex(b.ConversionError, "U\\+2014"):
            b.convert(bdf, charset)

    def test_small_synthetic_strike_converts(self):
        strike = b.convert(b.parse_bdf(synthetic_bdf(all_slots(3, 5, 4))), charset)
        self.assertEqual((strike.ascent, strike.descent, strike.y_advance), (5, 0, 5))
        fields, rows = unpack(strike, ord("A"))
        self.assertEqual(rows, ["###"] * 5)
        self.assertEqual(fields[3:], (4, 0, -5))


class Output(unittest.TestCase):
    def test_manifest_build_writes_one_cpp_per_strike_and_a_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = os.path.join(tmp, "fonts.manifest")
            with open(manifest, "w") as f:
                f.write("# test\nTimesRoman25 timR18.bdf\nTimesBold50Smooth timB18.bdf "
                        "--scale 2 --smooth scale2x\n")
            out = os.path.join(tmp, "out")
            result = subprocess.run(
                [sys.executable, os.path.join(TOOLS, "bdf2freeink.py"), "--manifest", manifest,
                 "--out-dir", out, "--quiet"],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(sorted(os.listdir(out)),
                             ["Strikes.h", "TimesBold50Smooth.cpp", "TimesRoman25.cpp"])
            with open(os.path.join(out, "Strikes.h")) as f:
                header = f.read()
            self.assertIn("extern const freeink::ui::BitmapFont kTimesRoman25;", header)
            self.assertIn("namespace tinta::fonts", header)
            self.assertNotIn("kBitmap", header)
            with open(os.path.join(out, "TimesRoman25.cpp")) as f:
                cpp = f.read()
            self.assertIn("const freeink::ui::BitmapFont kTimesRoman25 = {kBitmap, kGlyphs, "
                          "0x0020, 0x00FF, 29, 23,", cpp)
            # A comment ending in a backslash would swallow the next line.
            self.assertFalse(any(line.rstrip().endswith("\\") for line in cpp.splitlines()))

    def test_bad_manifest_reports_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = os.path.join(tmp, "fonts.manifest")
            with open(manifest, "w") as f:
                f.write("TimesRoman25 timR18.bdf\nTimesRoman25 timR16.bdf\n")
            with self.assertRaises(b.ConversionError):
                b.parse_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
