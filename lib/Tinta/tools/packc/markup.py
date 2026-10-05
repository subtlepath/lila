"""Inline markup of sentences and notes (docs/content-style.md).

Sentences:
  {surface}               target word: emphasised, and a cloze item in lesson sentences
  {surface|lemma}         target linked to a lemma key
  {surface|lemma:form}    ... with an explicit form tag (pres.3s, pl, ...)
  [surface|lemma(:form)]  link only (disambiguation, or forcing a multi-word token)

Notes:
  _Spanish_   *strong*   _..*strong Spanish*.._   `respelling`   ~not Mexican~
  blank line = paragraph; a line starting with "- " = bullet item
"""

from __future__ import annotations

from dataclasses import dataclass


class MarkupError(ValueError):
    pass


@dataclass
class Mark:
    start: int          # character offsets into the plain text
    end: int
    lemma: str | None   # lemma key, or None to infer from the surface
    form: str | None    # form tag text, or None
    target: bool


def parse_sentence(text: str) -> tuple[str, list[Mark]]:
    plain: list[str] = []
    marks: list[Mark] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in "{[":
            close = "}" if ch == "{" else "]"
            j = text.find(close, i + 1)
            if j < 0:
                raise MarkupError(f"unclosed '{ch}' in: {text}")
            inner = text[i + 1:j]
            if any(c in inner for c in "{}[]"):
                raise MarkupError(f"nested markup in: {text}")
            surface, bar, link = inner.partition("|")
            if not surface.strip() or surface != surface.strip():
                raise MarkupError(f"empty or padded surface in '{text[i:j + 1]}'")
            lemma = form = None
            if bar:
                lemma, colon, form_text = link.partition(":")
                lemma = lemma.strip()
                form = form_text.strip() if colon else None
                if not lemma:
                    raise MarkupError(f"missing lemma after '|' in '{text[i:j + 1]}'")
            start = sum(len(p) for p in plain)
            plain.append(surface)
            marks.append(Mark(start, start + len(surface), lemma, form, ch == "{"))
            i = j + 1
        elif ch in "}]|":
            raise MarkupError(f"stray '{ch}' in: {text}")
        else:
            plain.append(ch)
            i += 1
    return "".join(plain), marks


# Note span styles (docs/pack-format.md 3.10).
TEXT, STRONG, SPANISH, SPANISH_STRONG, RESPELLING, NOT_MEXICAN, PARAGRAPH, BULLET = range(8)
SPANISH_STYLES = (SPANISH, SPANISH_STRONG)


def parse_note(text: str) -> list[tuple[int, str]]:
    """Note text -> [(style, text)], with PARAGRAPH/BULLET break spans (text "")."""
    spans: list[tuple[int, str]] = []
    blocks: list[tuple[bool, list[str]]] = []  # (is_bullet, lines)
    for raw in text.strip("\n").split("\n"):
        line = raw.strip()
        if not line:
            blocks.append((False, []))
            continue
        if line.startswith("- "):
            blocks.append((True, [line[2:].strip()]))
        elif blocks and blocks[-1][1]:
            blocks[-1][1].append(line)
        else:
            blocks.append((False, [line]))
    first = True
    for is_bullet, lines in blocks:
        if not lines:
            continue
        if not first or is_bullet:
            spans.append((BULLET if is_bullet else PARAGRAPH, ""))
        first = False
        spans.extend(_inline(" ".join(lines)))
    return spans


def _inline(text: str) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    buf: list[str] = []
    spanish = strong = False
    i = 0

    def style() -> int:
        if spanish:
            return SPANISH_STRONG if strong else SPANISH
        return STRONG if strong else TEXT

    def flush() -> None:
        if buf:
            out.append((style(), "".join(buf)))
            buf.clear()

    while i < len(text):
        ch = text[i]
        if ch == "\\" and i + 1 < len(text):
            buf.append(text[i + 1])
            i += 2
            continue
        if ch in "`~":
            j = text.find(ch, i + 1)
            if j < 0:
                raise MarkupError(f"unclosed '{ch}' in note: {text}")
            flush()
            out.append((RESPELLING if ch == "`" else NOT_MEXICAN, text[i + 1:j]))
            i = j + 1
            continue
        if ch == "_":
            flush()
            spanish = not spanish
            if not spanish:
                strong = False
        elif ch == "*":
            flush()
            strong = not strong
        else:
            buf.append(ch)
        i += 1
    if spanish or strong:
        raise MarkupError(f"unclosed '{'_' if spanish else '*'}' in note: {text}")
    flush()
    return out
