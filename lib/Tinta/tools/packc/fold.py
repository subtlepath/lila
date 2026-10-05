"""Search-key folding (docs/pack-format.md section 2).

The device folds what the learner types with tinta::core::foldKey
(src/core/pack/Fold.h). Both must agree byte for byte, so this module can
print test vectors: `python3 tools/packc/fold.py --vectors`.
"""

from __future__ import annotations

import sys
import unicodedata

_LATIN1 = {
    "a": "ÀÁÂÃÄÅàáâãäå",
    "ae": "Ææ",
    "c": "Çç",
    "e": "ÈÉÊËèéêë",
    "i": "ÌÍÎÏìíîï",
    "d": "Ðð",
    "n": "Ññ",
    "o": "ÒÓÔÕÖØòóôõöø",
    "u": "ÙÚÛÜùúûü",
    "y": "Ýýÿ",
    "th": "Þþ",
    "ss": "ß",
}
FOLD_MAP = {ch: out for out, chars in _LATIN1.items() for ch in chars}
SEPARATORS = " \t\n"


def fold(text: str) -> str:
    out: list[str] = []
    pending_space = False
    for ch in unicodedata.normalize("NFC", text):
        if "a" <= ch <= "z" or "0" <= ch <= "9":
            piece = ch
        elif "A" <= ch <= "Z":
            piece = ch.lower()
        elif ch in FOLD_MAP:
            piece = FOLD_MAP[ch]
        elif ch in SEPARATORS:
            pending_space = bool(out)
            continue
        else:
            continue
        if pending_space:
            out.append(" ")
            pending_space = False
        out.append(piece)
    return "".join(out)


def vectors() -> list[tuple[str, str]]:
    """Inputs covering every charset codepoint, plus words and edge cases."""
    sys.path.insert(0, __file__.rsplit("/", 2)[0])
    import charset  # noqa: E402  (tools/charset.py)

    cases: list[str] = []
    for slot, _source in charset.slots():
        if charset.slot_source(slot) is not None:
            cases.append("x" + chr(slot) + "y")
    cases += [
        "¿Qué onda?", "Niño", "año", "e-mail", "¡Órale!", "  dos   espacios  ",
        "Ávila", "pingüino", "MÉXICO", "señor\tSánchez\nGarcía", "…ya", "“cita”",
        "Straße", "Æsir", "Þing", "", "   ", "¿?", "a…b", "xĀy", "é",
    ]
    return [(case, fold(case)) for case in cases]


def main(argv: list[str]) -> int:
    if argv[1:] == ["--vectors"]:
        # One vector per line: input and expected key as hex UTF-8, tab-separated,
        # so control characters and tabs survive the trip into the C++ test.
        for case, key in vectors():
            print(case.encode("utf-8").hex() + "\t" + key.encode("ascii").hex())
        return 0
    for word in argv[1:]:
        print(fold(word))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
