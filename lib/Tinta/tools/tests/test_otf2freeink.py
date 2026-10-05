"""Tests for tools/otf2freeink.py, which makes the shipped headword strikes.

    python3 -m unittest discover -s tools/tests

They need the pinned FreeType from build/ft-venv. Run by another interpreter,
this file re-runs itself there (installing the venv on first use) and fails,
rather than skips, if that cannot work.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

TESTS = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(TESTS)
ROOT = os.path.dirname(TOOLS)
TERMES = os.path.join(ROOT, "assets", "fonts", "outline", "texgyretermes-bold.otf")
sys.path.insert(0, TOOLS)

import bdf2freeink as b  # noqa: E402
import charset  # noqa: E402
import otf2freeink as o  # noqa: E402

try:
    import freetype
except ImportError:
    freetype = None


class Manifest(unittest.TestCase):
    """Outline entries in a bdf2freeink manifest (no FreeType needed)."""

    def parse(self, text: str) -> list[b.Entry]:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "fonts.manifest")
            with open(path, "w") as f:
                f.write(text)
            return b.parse_manifest(path)

    def test_outline_entries(self):
        entries = self.parse("TimesBold34 timB24.bdf\n"
                             "TermesBold87 outline/texgyretermes-bold.otf --px 87 --raster hinted\n")
        self.assertFalse(entries[0].outline)
        self.assertTrue(entries[1].outline)
        self.assertEqual((entries[1].px, entries[1].raster), (87, "hinted"))

    def test_bad_outline_entries(self):
        with self.assertRaises(b.ConversionError):
            self.parse("X outline/a.otf --px 87\n")
        with self.assertRaises(b.ConversionError):
            self.parse("X outline/a.otf --px 87 --raster hinted --scale 2\n")

    def test_shipped_manifest_ladder_is_termes(self):
        entries = {e.name: e for e in b.parse_manifest(os.path.join(TOOLS, "fonts.manifest"))}
        for px in (50, 58, 68, 75, 87):
            e = entries[f"TermesBold{px}"]
            self.assertEqual((e.bdf, e.px, e.raster), ("outline/texgyretermes-bold.otf", px, "hinted"))
        self.assertFalse(any(e.smooth == "hqx" for e in entries.values()))


if freetype is None:

    class InFreeTypeVenv(unittest.TestCase):
        def test_outline_suite_in_ft_venv(self):
            python = o.venv_python("tools/tests/test_otf2freeink.py")
            result = subprocess.run([python, "-m", "unittest", "test_otf2freeink"], cwd=TESTS,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

else:

    def termes():
        return o.load_font(freetype, TERMES)

    def ink(g: b.Glyph) -> set[tuple[int, int]]:
        return {(g.x_offset + x, g.y_offset + y) for y, row in enumerate(g.pixels) for x, p in enumerate(row) if p}

    class Rasterise(unittest.TestCase):
        def test_freetype_is_the_pinned_version(self):
            self.assertEqual(freetype.version(), o.FREETYPE_VERSION)

        def test_glyphs_match_freetype(self):
            # Read FreeType's own monochrome rendering directly and compare.
            face = freetype.Face(TERMES)
            face.set_pixel_sizes(0, 50)
            font = termes()
            for ch in "Ñ¿g—A":
                face.load_char(ch, freetype.FT_LOAD_TARGET_MONO)
                face.glyph.render(freetype.FT_RENDER_MODE_MONO)
                bm = face.glyph.bitmap
                want = {(face.glyph.bitmap_left + x, -face.glyph.bitmap_top + y)
                        for y in range(bm.rows) for x in range(bm.width)
                        if bm.buffer[y * bm.pitch + (x >> 3)] >> (7 - (x & 7)) & 1}
                got = o.rasterise(freetype, font, ord(ch), 50, "hinted")
                self.assertEqual(ink(got), want, ch)
                self.assertEqual(got.advance, round(face.glyph.advance.x / 64), ch)
                if got.height:  # trimmed to the ink
                    self.assertTrue(any(got.pixels[0]) and any(got.pixels[-1]), ch)

        def test_the_three_rasterisations_differ_but_all_convert(self):
            font = termes()
            strikes = {r: o.convert(freetype, font, 34, r, charset) for r in o.RASTERS}
            self.assertNotEqual(strikes["hinted"].bitmap, strikes["autohint"].bitmap)
            self.assertNotEqual(strikes["hinted"].bitmap, strikes["coverage"].bitmap)
            for strike in strikes.values():
                self.assertEqual(len(strike.glyphs), charset.LAST - charset.FIRST + 1)

    class Strike(unittest.TestCase):
        @classmethod
        def setUpClass(cls):
            cls.font = termes()
            cls.strike = o.convert(freetype, cls.font, 87, "hinted", charset)

        def glyph(self, slot: int) -> b.Glyph:
            return self.strike.glyphs[slot - self.strike.first].glyph

        def test_charset_slots_and_c1_remaps(self):
            self.assertEqual((self.strike.first, self.strike.last), (0x20, 0xFF))
            for cp, slot in list(charset.REMAP.items()) + [(ord("A"), ord("A")), (0xD1, 0xD1)]:
                want = o.rasterise(freetype, self.font, cp, 87, "hinted")
                got = self.glyph(slot)
                self.assertEqual((ink(got), got.advance), (ink(want), want.advance), f"slot 0x{slot:02X}")
            for slot in (0x7F, 0x81, 0x8D, 0x90, 0x9D):
                g = self.glyph(slot)
                self.assertEqual((g.width, g.height, g.advance), (0, 0, 0), f"slot 0x{slot:02X}")

        def test_line_metrics_come_from_the_subset(self):
            inked = [sg.glyph for sg in self.strike.glyphs if sg.glyph.height]
            self.assertEqual(self.strike.ascent, max(-g.y_offset for g in inked))
            self.assertEqual(self.strike.descent, max(g.y_offset + g.height for g in inked))
            self.assertEqual(self.strike.y_advance, self.strike.ascent + self.strike.descent)
            # Accented capitals set the ascent, above the plain capitals.
            tallest_accent = max(-self.glyph(ord(c)).y_offset for c in "ÁÉÍÓÚÑ")
            self.assertGreaterEqual(self.strike.ascent, tallest_accent)
            self.assertGreater(tallest_accent, -self.glyph(ord("H")).y_offset)

        def test_deterministic(self):
            again = o.convert(freetype, self.font, 87, "hinted", charset)
            self.assertEqual(again.bitmap, self.strike.bitmap)
            fake = o.describe(self.font, 87, "hinted", freetype)
            self.assertEqual(b.emit_cpp(again, "X", fake, "x.otf", 1, "none"),
                             b.emit_cpp(self.strike, "X", fake, "x.otf", 1, "none"))

        def test_committed_strike_is_what_the_manifest_makes(self):
            fake = o.describe(self.font, 87, "hinted", freetype)
            want = b.emit_cpp(self.strike, "TermesBold87", fake, "assets/fonts/outline/texgyretermes-bold.otf", 1,
                              "none")
            with open(os.path.join(ROOT, "src", "fonts", "TermesBold87.cpp"), encoding="utf-8") as f:
                self.assertEqual(f.read(), want)

    class Limits(unittest.TestCase):
        def test_102_px_full_charset_overflows_the_bitmap_offsets(self):
            with self.assertRaisesRegex(b.ConversionError, "bitmap offset"):
                o.convert(freetype, termes(), 102, "hinted", charset)

        def test_headword_subset_fits_at_102_px(self):
            strike = o.convert(freetype, termes(), 102, "hinted", charset, o.HEADWORD_SUBSET)
            glyphs = {sg.slot: sg.glyph for sg in strike.glyphs}
            self.assertGreater(glyphs[ord("ñ")].height, 0)
            self.assertEqual(glyphs[0xA9].height, 0)  # © is not a headword character
            self.assertLessEqual(max(sg.offset for sg in strike.glyphs), 0xFFFF)

        def test_missing_font_file_fails(self):
            with self.assertRaises(Exception):
                o.load_font(freetype, os.path.join(TESTS, "no-such-font.otf"))


if __name__ == "__main__":
    unittest.main()
