"""Standard Latin ligatures carried by BDF strikes.

BDF has no GSUB table. X11 strikes such as Adobe Times and Helvetica keep
their ligatures as unencoded glyphs (``STARTCHAR fi`` / ``ENCODING -1``), which
FreeType cannot reach by codepoint. ``open_face`` loads such a strike with
those glyphs encoded at their Alphabetic Presentation Forms codepoints, and
``ligature_pairs`` builds the reader's pair table for the ones a face serves.

Only the ligatures OpenType ``liga`` covers (ff, fi, fl, ffi, ffl) are used.
Monospaced and character-cell strikes are left alone: a ligature would take
one cell where its letters take two.
"""

from __future__ import annotations

import io
import os
import re

# Adobe Glyph List names, plus the uniXXXX and underscore forms.
LIGATURE_GLYPH_NAMES = {
    "ff": 0xFB00, "f_f": 0xFB00, "uniFB00": 0xFB00,
    "fi": 0xFB01, "f_i": 0xFB01, "uniFB01": 0xFB01,
    "fl": 0xFB02, "f_l": 0xFB02, "uniFB02": 0xFB02,
    "ffi": 0xFB03, "f_f_i": 0xFB03, "uniFB03": 0xFB03,
    "ffl": 0xFB04, "f_f_l": 0xFB04, "uniFB04": 0xFB04,
}

LIGATURE_SEQUENCES = {
    0xFB00: (0x66, 0x66),        # ff
    0xFB01: (0x66, 0x69),        # fi
    0xFB02: (0x66, 0x6C),        # fl
    0xFB03: (0x66, 0x66, 0x69),  # ffi
    0xFB04: (0x66, 0x66, 0x6C),  # ffl
}

_ENCODING_RE = re.compile(rb"^ENCODING\s+(-?\d+)", re.MULTILINE)
_SPACING_RE = re.compile(rb'^SPACING\s+"?([A-Za-z])', re.MULTILINE)


def is_bdf(font_path) -> bool:
    return os.path.splitext(str(font_path))[1].lower() == ".bdf"


def encode_ligature_glyphs(source: bytes) -> bytes:
    """Give a BDF's unencoded ligature glyphs their U+FB0x encodings.

    A codepoint the strike already encodes keeps its glyph.
    """
    spacing = _SPACING_RE.search(source)
    if spacing and spacing.group(1).upper() in (b"M", b"C"):
        return source

    encoded = {int(value) for value in _ENCODING_RE.findall(source) if int(value) >= 0}
    lines = source.split(b"\n")
    glyph_name = None
    changed = False
    for index, line in enumerate(lines):
        if line.startswith(b"STARTCHAR"):
            glyph_name = line[len(b"STARTCHAR"):].strip().decode("latin-1")
        elif line.startswith(b"ENCODING") and glyph_name in LIGATURE_GLYPH_NAMES:
            fields = line.split()
            codepoint = LIGATURE_GLYPH_NAMES[glyph_name]
            if len(fields) >= 2 and fields[1] == b"-1" and codepoint not in encoded:
                lines[index] = b"ENCODING %d" % codepoint
                encoded.add(codepoint)
                changed = True
    return b"\n".join(lines) if changed else source


def open_face(font_path):
    """Open a font with FreeType, encoding a BDF's ligature glyphs first."""
    import freetype

    if not is_bdf(font_path):
        return freetype.Face(str(font_path))
    with open(font_path, "rb") as source:
        return freetype.Face(io.BytesIO(encode_ligature_glyphs(source.read())))


def ligature_pairs(codepoints) -> list[tuple[int, int]]:
    """Packed (left << 16 | right, ligature) pairs, sorted, for one face's glyphs.

    ``codepoints`` must hold only glyphs the bitmap face itself serves, so a
    ligature never joins letters from one strike into another's glyph.
    Three-letter ligatures chain through their two-letter prefix (ff + i).
    """
    available = set(codepoints)
    prefix_ligature = {seq: cp for cp, seq in LIGATURE_SEQUENCES.items()}
    pairs = []
    for lig_cp, seq in LIGATURE_SEQUENCES.items():
        if lig_cp not in available or not all(cp in available for cp in seq):
            continue
        left = seq[0]
        if len(seq) > 2:
            left = prefix_ligature.get(seq[:-1])
            if left is None or left not in available:
                continue
        pairs.append(((left << 16) | seq[-1], lig_cp))
    return sorted(pairs)
