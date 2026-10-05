"""Tests for hqx scaling (tools/hqx_tables.py, bdf2freeink --smooth hqx). No
shipped strike uses it any more; tools/host/render-specimen.sh's comparison
pages do.

    python3 -m unittest discover -s tools/tests

These need the pinned hqx port in build/hqx-venv, installed on first use; they
fail rather than skip without it. The ffmpeg cross-check skips when ffmpeg is
not installed.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import bdf2freeink as b  # noqa: E402
import charset  # noqa: E402
import hqx_tables  # noqa: E402


def bdf(name: str) -> b.BdfFont:
    return b.read_bdf(b.find_bdf(name))


def rows(g: b.Glyph) -> list[str]:
    return ["".join("#" if p else "." for p in row) for row in g.pixels]


def glyph(art: list[str], advance: int = 0) -> b.Glyph:
    pixels = [[1 if c == "#" else 0 for c in row] for row in art]
    return b.Glyph(len(art[0]), len(art), 0, -len(art), advance or len(art[0]) + 1, pixels)


class Tables(unittest.TestCase):
    def test_shape_and_trivial_neighbourhoods(self):
        tables = b.hqx_tables()
        for k in (2, 3, 4):
            self.assertEqual(len(tables[k]), 512)
            self.assertTrue(all(len(row) == k * k for row in tables[k]))
            self.assertEqual(tables[k][0], [0] * (k * k))
            self.assertEqual(tables[k][511], [255] * (k * k))
            self.assertTrue(all(0 <= v <= 255 for row in tables[k] for v in row))

    def test_symmetric_under_rotation_and_mirroring(self):
        # hqx treats all eight orientations alike, so a wrong bit or output
        # order would show up here.
        def turn(mask: int, f) -> int:
            out = 0
            for i, (dx, dy) in enumerate(hqx_tables.NEIGHBOURS):
                if mask >> i & 1:
                    out |= 1 << hqx_tables.NEIGHBOURS.index(f(dx, dy))
            return out

        tables = b.hqx_tables()
        for k in (2, 3, 4):
            for f in (lambda x, y: (-y, x), lambda x, y: (-x, y)):
                for mask in range(512):
                    src, dst = tables[k][mask], tables[k][turn(mask, f)]
                    for j in range(k * k):
                        nx, ny = f(2 * (j % k) - (k - 1), 2 * (j // k) - (k - 1))
                        self.assertEqual(src[j], dst[(ny + k - 1) // 2 * k + (nx + k - 1) // 2])

    def test_loading_twice_gives_the_same_tables(self):
        self.assertEqual(hqx_tables.load(), hqx_tables.load())


class Shapes(unittest.TestCase):
    def test_diagonal_steps_one_pixel_per_row(self):
        diagonal = glyph(["#...", ".#..", "..#.", "...#"])
        self.assertEqual(rows(b.scale_hqx(diagonal, 2)), [
            "##......",
            "###.....",
            ".###....",
            "..###...",
            "...###..",
            "....###.",
            ".....###",
            "......##",
        ])
        for k in (2, 3, 4):
            out = rows(b.scale_hqx(diagonal, k))
            starts = [row.index("#") for row in out]
            steps = [c - a for a, c in zip(starts, starts[1:])]
            # The edge moves at most one pixel per row, almost every row; plain
            # scaling holds it for k rows and then jumps k pixels.
            self.assertTrue(set(steps) <= {0, 1}, f"x{k}: {starts}")
            self.assertGreaterEqual(steps.count(1), 4 * k - 4, f"x{k}: {starts}")
            self.assertTrue(all("#" * k in row for row in out[1:-1]), f"x{k}")  # unbroken, k wide
            plain = [row.index("#") for row in rows(b.scale_plain(diagonal, k))]
            self.assertIn(k, [c - a for a, c in zip(plain, plain[1:])])

    def test_disc_is_rounded_and_symmetric(self):
        disc = glyph(["...#...", ".#####.", ".#####.", "#######", ".#####.", ".#####.", "...#..."])
        out = rows(b.scale_hqx(disc, 2))
        self.assertEqual(out, [
            "......##......",
            ".....####.....",
            "..##########..",
            "..##########..",
            "..##########..",
            ".############.",
            "##############",
            "##############",
            ".############.",
            "..##########..",
            "..##########..",
            "..##########..",
            ".....####.....",
            "......##......",
        ])
        self.assertEqual(out, out[::-1])
        self.assertEqual(out, [row[::-1] for row in out])

    def test_curves_step_finer_than_plain_scaling(self):
        font = bdf("timB18.bdf")
        for ch in "OQC":
            g = b.glyph_from_bdf(font.glyphs[ord(ch)])
            for k in (2, 3):
                def left_edges(scaled):
                    return [row.index(1) for row in scaled.pixels if 1 in row]

                plain, smooth = left_edges(b.scale_plain(g, k)), left_edges(b.scale_hqx(g, k))
                self.assertGreater(len(set(smooth)), len(set(plain)), f"{ch} x{k}")
                jump = lambda e: max(abs(a - c) for a, c in zip(e, e[1:]))  # noqa: E731
                self.assertLess(jump(smooth), jump(plain), f"{ch} x{k}")


class Strikes(unittest.TestCase):
    CASES = [("timB18.bdf", 2), ("timB18.bdf", 3), ("timB21.bdf", 3), ("timB24.bdf", 2), ("timB14.bdf", 4)]

    def test_metrics_scale_exactly(self):
        for name, k in self.CASES:
            with self.subTest(font=name, scale=k):
                one = b.convert(bdf(name), charset)
                big = b.convert(bdf(name), charset, k, "hqx")
                self.assertEqual((big.ascent, big.descent), (one.ascent * k, one.descent * k))
                self.assertEqual((big.max_width, big.max_height), (one.max_width * k, one.max_height * k))
                for small, large in zip(one.glyphs, big.glyphs):
                    s, g = small.glyph, large.glyph
                    self.assertEqual((g.width, g.height, g.x_offset, g.y_offset, g.advance),
                                     (s.width * k, s.height * k, s.x_offset * k, s.y_offset * k, s.advance * k),
                                     f"U+{small.source or small.slot:04X}")

    def test_ink_stays_close_to_the_source(self):
        for name, k in self.CASES:
            one = b.convert(bdf(name), charset)
            big = b.convert(bdf(name), charset, k, "hqx")
            ink = lambda s: sum(sum(map(sum, g.glyph.pixels)) for g in s.glyphs)  # noqa: E731
            ratio = ink(big) / (ink(one) * k * k)
            self.assertTrue(0.97 <= ratio <= 1.03, f"{name} x{k}: ink ratio {ratio:.3f}")

    def test_102_px_overflows_the_bitmap_offsets(self):
        with self.assertRaisesRegex(b.ConversionError, "bitmap offset"):
            b.convert(bdf("timB24.bdf"), charset, 3, "hqx")

    def test_options(self):
        with self.assertRaises(b.ConversionError):
            b.convert(bdf("timB18.bdf"), charset, 1, "hqx")
        with self.assertRaises(b.ConversionError):
            b.convert(bdf("timB18.bdf"), charset, 3, "scale2x")

    def test_deterministic(self):
        first = b.convert(bdf("timB21.bdf"), charset, 3, "hqx")
        second = b.convert(bdf("timB21.bdf"), charset, 3, "hqx")
        self.assertEqual(first.bitmap, second.bitmap)
        font = bdf("timB21.bdf")
        self.assertEqual(b.emit_cpp(first, "X", font, "x.bdf", 3, "hqx"),
                         b.emit_cpp(second, "X", font, "x.bdf", 3, "hqx"))


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg not installed")
class MatchesFfmpeg(unittest.TestCase):
    """ffmpeg's hqx filter is an independent C implementation of the same
    algorithm; the port's output must match it value for value."""

    def test_glyph_atlas(self):
        from PIL import Image

        font = bdf("timB24.bdf")
        glyphs = [b.glyph_from_bdf(font.glyphs[ord(ch)]) for ch in "Ñ¿gQa—"]
        width = sum(g.width + 3 for g in glyphs) + 2
        height = max(g.height for g in glyphs) + 4
        ink = [[0] * width for _ in range(height)]
        x = 2
        for g in glyphs:
            for y, row in enumerate(g.pixels):
                for gx, p in enumerate(row):
                    ink[2 + y][x + gx] = p
            x += g.width + 3
        image = Image.new("RGB", (width, height), (255, 255, 255))
        for y in range(height):
            for x in range(width):
                if ink[y][x]:
                    image.putpixel((x, y), (0, 0, 0))
        tables = b.hqx_tables()
        with tempfile.TemporaryDirectory() as tmp:
            source = os.path.join(tmp, "atlas.png")
            image.save(source)
            for k in (2, 3, 4):
                out = os.path.join(tmp, f"hq{k}x.png")
                subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", source, "-vf", f"hqx=n={k}",
                                "-pix_fmt", "rgb24", out], check=True)
                theirs = Image.open(out).convert("RGB").load()
                for y in range(height):
                    for x in range(width):
                        mask = 0
                        for i, (dx, dy) in enumerate(hqx_tables.NEIGHBOURS):
                            nx, ny = x + dx, y + dy
                            if 0 <= nx < width and 0 <= ny < height and ink[ny][nx]:
                                mask |= 1 << i
                        for j, ours in enumerate(tables[k][mask]):
                            red = theirs[x * k + j % k, y * k + j // k][0]
                            self.assertEqual(ours, 255 - red, f"hq{k}x at ({x}, {y}) block pixel {j}")


if __name__ == "__main__":
    unittest.main()
