"""The Tinta font character set (PLAN.md section 5.4).

A FreeInkUI BitmapFont covers one contiguous codepoint range. Tinta's range is
U+0020..U+00FF, which holds every Spanish letter. Typographic punctuation lies
outside it, so those glyphs live in the unused C1 slots at their Windows-1252
positions. The font converter draws each slot from its source codepoint; the
content compiler rewrites text to slot codepoints. Both import this module so
the two can never disagree.
"""

from __future__ import annotations

import unicodedata

FIRST = 0x20
LAST = 0xFF

# Unicode codepoint -> slot inside FIRST..LAST.
REMAP = {
    0x20AC: 0x80,  # €
    0x2018: 0x91,  # ‘
    0x2019: 0x92,  # ’
    0x201C: 0x93,  # “
    0x201D: 0x94,  # ”
    0x2022: 0x95,  # •
    0x2013: 0x96,  # –
    0x2014: 0x97,  # —
}

# Drawn by DisplayTarget itself (U+2026 becomes three full stops), so it stays
# as-is in text and needs no slot.
PASSTHROUGH = frozenset({0x2026})

_SLOT_SOURCE = {slot: cp for cp, slot in REMAP.items()}


class CharsetError(ValueError):
    """Text contains a character the fonts cannot draw."""


def slot_source(slot: int) -> int | None:
    """The Unicode codepoint whose glyph fills `slot`, or None for an empty slot."""
    if slot in _SLOT_SOURCE:
        return _SLOT_SOURCE[slot]
    if 0x20 <= slot <= 0x7E or 0xA0 <= slot <= 0xFF:
        return slot
    return None


def slots():
    """Yield (slot, source codepoint or None) for every slot, in order."""
    for slot in range(FIRST, LAST + 1):
        yield slot, slot_source(slot)


def is_supported(cp: int) -> bool:
    if cp in REMAP or cp in PASSTHROUGH:
        return True
    return 0x20 <= cp <= 0x7E or 0xA0 <= cp <= 0xFF


def unsupported(text: str) -> list[str]:
    """Characters of `text` (after NFC) that the fonts cannot draw, in order, deduplicated."""
    seen: list[str] = []
    for ch in unicodedata.normalize("NFC", text):
        if ch == "\n":
            continue
        if not is_supported(ord(ch)) and ch not in seen:
            seen.append(ch)
    return seen


def encode(text: str) -> str:
    """Rewrite `text` into the font charset: NFC, then remapped characters moved to their slots."""
    out = []
    for ch in unicodedata.normalize("NFC", text):
        cp = ord(ch)
        if cp in REMAP:
            out.append(chr(REMAP[cp]))
        elif ch == "\n" or is_supported(cp):
            out.append(ch)
        else:
            raise CharsetError(f"unsupported character {ch!r} (U+{cp:04X}) in {text!r}")
    return "".join(out)


def decode(text: str) -> str:
    """Inverse of encode(), for tools that print pack strings."""
    return "".join(chr(_SLOT_SOURCE.get(ord(ch), ord(ch))) for ch in text)
