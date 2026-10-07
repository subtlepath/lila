"""Render the lila logos: logo.svg into Logo120.png and the Logo120.h boot logo,
logo_sleep.svg into the LogoSleep120.h sleep logo.

Usage: python3 scripts/convert_logo.py

Needs Pillow plus cairosvg or Inkscape on PATH. The header is drawn with
GfxRenderer::drawImage, which blits panel-native rows, so the bitmap is stored
rotated 90 degrees counter-clockwise, MSB-first, 1 = white.
"""

import io
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

SIZE = 120
THRESHOLD = 128
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGES = os.path.join(ROOT, "src", "images")


def render_svg(svg_path):
    try:
        import cairosvg

        return Image.open(io.BytesIO(cairosvg.svg2png(url=svg_path, output_width=SIZE, output_height=SIZE)))
    except ImportError:
        pass
    if not shutil.which("inkscape"):
        sys.exit("Need cairosvg (pip install cairosvg) or Inkscape on PATH")
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "logo.png")
        subprocess.run(
            ["inkscape", svg_path, "--export-type=png", f"--export-width={SIZE}", f"--export-height={SIZE}",
             f"--export-filename={out}"],
            check=True, capture_output=True)
        return Image.open(out).copy()


def write_header(img, name):
    flat = Image.new("RGBA", img.size, (255, 255, 255, 255))
    flat.paste(img, mask=img.split()[3])
    gray = flat.convert("L").rotate(90, expand=True)

    packed = []
    for y in range(SIZE):
        for x in range(0, SIZE, 8):
            byte = 0
            for b in range(8):
                if gray.getpixel((x + b, y)) >= THRESHOLD:
                    byte |= 1 << (7 - b)
            packed.append(byte)

    lines = ["#pragma once", "#include <cstdint>", "", f"// Image dimensions: {SIZE}x{SIZE}",
             f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(packed), 16):
        lines.append("    " + ", ".join(f"0x{v:02x}" for v in packed[i:i + 16]) + ",")
    lines.append("};")
    with open(os.path.join(IMAGES, f"{name}.h"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Wrote {name}.h ({len(packed)} bytes)")


def main():
    img = render_svg(os.path.join(IMAGES, "logo.svg")).convert("RGBA")
    img.save(os.path.join(IMAGES, "Logo120.png"))
    write_header(img, "Logo120")
    write_header(render_svg(os.path.join(IMAGES, "logo_sleep.svg")).convert("RGBA"), "LogoSleep120")

if __name__ == "__main__":
    main()
