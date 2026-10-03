# CrossPoint 16pt (22px) and 21pt (29px) Times and Helvetica strikes

These BDF strikes fill gaps in the X.org Adobe 100dpi sizes, so
`convert-x11-bdf.py` produces `Times_11.cpfont`/`Helvetica_11.cpfont` (between
the Adobe 14pt/20px and 18pt/25px strikes) and `Times_14.cpfont`/
`Helvetica_14.cpfont` (between the 18pt/25px and 24pt/34px strikes) alongside
the existing sizes:

| 16pt (22px)     | 21pt (29px)     | Style                  |
| --------------- | --------------- | ---------------------- |
| `timR16.bdf`    | `timR21.bdf`    | Times Roman            |
| `timB16.bdf`    | `timB21.bdf`    | Times Bold             |
| `timI16.bdf`    | `timI21.bdf`    | Times Italic           |
| `timBI16.bdf`   | `timBI21.bdf`   | Times Bold Italic      |
| `helvR16.bdf`   | `helvR21.bdf`   | Helvetica              |
| `helvB16.bdf`   | `helvB21.bdf`   | Helvetica Bold         |
| `helvO16.bdf`   | `helvO21.bdf`   | Helvetica Oblique      |
| `helvBO16.bdf`  | `helvBO21.bdf`  | Helvetica Bold Oblique |

Each strike has the same encoded glyph set as the matching Adobe 100dpi
ISO10646-1 strike. Metrics follow the proportions of the neighbouring Adobe
strikes rather than the raw outlines:

| | 16pt (22px) | 21pt (29px) |
| --- | --- | --- |
| Times x-height / cap and ascender / descender | 10 / 15 / 5 | 13 (Bold 14) / 20 (Roman and Italic caps 19) / 6 |
| Times ascent / descent | 18 / 5 | 23 / 7 |
| Helvetica x-height / cap / figures / descender | 12 / 17 / 16 / 5 | 16 / 22 / 21 / 6 |
| Helvetica ascent / descent | 19 / 5 | 25 / 6 |
| Regular and italic stems | 2px | 2px |
| Times Bold stems (lowercase / capitals) | 3px / 4px | 4px / 4px |
| Helvetica Bold verticals / horizontals | 3px / 2px | 4px / 3px |

- At 16pt, regular and italic bowls and diagonals are drawn to the same 2px
  colour as the stems, as in Adobe's 20px and 25px strikes. At 21pt, Times
  keeps the heavier round strokes and thick diagonals of Adobe's 34px strike,
  so its stroke contrast survives. In Times Roman capitals the 3px stress is
  held to the middle three rows of each bowl, the steep thick diagonals of A
  and W alternate 2px and 3px runs so both edges step evenly, and the legs of
  K and R are 3px, so the capitals sit at the colour of the lowercase. Times
  Italic capitals keep 3px only on the stress axis: four rows at the lower left
  and upper right of O, D and Q, three rows at the left of C and G. Times Bold
  capitals share the lowercase 4px stem and 4px thick diagonals; the 5px stress
  of O, C, D, G and Q is held to the middle two rows, and the bars of E, F and
  the barred letters (Ð Ħ Ŧ Ɨ đ ħ ƚ) are one row, as in Adobe's 25px strike.
- Even colour: blurring a glyph (Gaussian, sigma 1.25px) approximates how the
  eye integrates its ink. No point of any glyph is darker than the darkest of
  the strike's control characters `H`, `O`, `n` and `o`, which set the face's
  colour. Blots at joins, crossings and lumpy diagonals are reshaped to meet
  that; where a join still exceeds it, a one-pixel ink trap sits in the crotch.
  The solid symbols (• ¶ ♠ ♣ ♥ ♦) are exempt.

## Combining marks

Every Times strike here and in `font-adobe-100dpi-1.0.4` carries the combining
marks that decomposed Sanskrit transliteration (IAST, ISO 15919, Vedic accents)
leaves after NFC composition: U+0300–0304, U+0306–0308, U+0310 (candrabindu),
U+0323, U+0325 (ring below, for r̥ l̥) and U+0331. Each mark is lifted from the
strike's own accented letters (à á â ã ā ă ȧ ä ạ ḁ and ḇ…ẕ), keeps their
height, and has zero advance. The renderer centres a mark on its base glyph's
bitmap, so blank columns pad each mark to reproduce Adobe's placement (the
rightward lean of italic accents). Candrabindu is the breve with the dot above.

## Provenance

The base glyphs were grid-fitted from URW++ Nimbus Roman/Sans outlines (and
Standard Symbols PS for the Times Greek and math glyphs, which Adobe's Times
strikes copy from Symbol). They were then hand-edited pixel by pixel against the
neighbouring Adobe strikes (20px and 25px for the 16pt strikes, 25px and 34px
for the 21pt strikes). Accented letters, turned and barred letters, and
modifier glyphs are composed from those tuned parts using placement rules
fitted to Adobe's own accented letters, and then reviewed and corrected one by
one. Helvetica Oblique and Bold Oblique start as 12-degree row shears of the
upright strikes; their base glyphs are hand-corrected, and their accented
letters are rebuilt from those corrected oblique parts.

The URW outlines are from Ghostscript's URW base35 fonts. Check their licence
terms before redistributing these files.
