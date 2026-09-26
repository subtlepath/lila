#!/bin/bash

set -e

cd "$(dirname "$0")"

# Built-in fonts are native X11 bitmap strikes (Adobe 100 DPI Times/Helvetica),
# never resampled. Sizes follow CrossPoint's 150-DPI point convention, matching
# the x11/cpfont names produced by convert-x11-bdf.py:
#   source pt @ 100 DPI -> 12:8  14:9  18:12  21:14  24:16
# The 21pt strikes are CrossPoint's hand-tuned additions (x11/font-crosspoint-100dpi).
# GNU Unifont fills codepoints the Adobe strikes lack (Cyrillic, Hebrew, Arabic,
# symbols, U+FFFD) at its native 16px.
X11_DIR="../../../x11/font-adobe-100dpi-1.0.4"
CROSSPOINT_X11_DIR="../../../x11/font-crosspoint-100dpi"
UNIFONT="../../../x11/unifont-18.0.01.bdf"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
READER_FONT_SIZES=(8 9 12 14 16)

source_pt() {
  case "$1" in
    8) echo 12 ;;
    9) echo 14 ;;
    12) echo 18 ;;
    14) echo 21 ;;
    16) echo 24 ;;
    *) echo "No X11 strike mapped for ${1}pt" >&2; exit 1 ;;
  esac
}

strike_dir() {
  case "$1" in
    14) echo "$CROSSPOINT_X11_DIR" ;;
    *) echo "$X11_DIR" ;;
  esac
}

# X11 file-name style suffixes (Helvetica uses Oblique for italic).
times_style() {
  case "$1" in Regular) echo R ;; Italic) echo I ;; Bold) echo B ;; BoldItalic) echo BI ;; esac
}
helvetica_style() {
  case "$1" in Regular) echo R ;; Italic) echo O ;; Bold) echo B ;; BoldItalic) echo BO ;; esac
}

for size in ${READER_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    lower_style=$(echo $style | tr '[:upper:]' '[:lower:]')

    output_path="../builtinFonts/times_${size}_${lower_style}.h"
    python fontconvert.py times_${size}_${lower_style} $size \
      "$(strike_dir $size)/tim$(times_style $style)$(source_pt $size).bdf" "$UNIFONT" \
      --2bit --compress --zopfli > $output_path
    echo "Generated $output_path"

    output_path="../builtinFonts/helvetica_${size}_${lower_style}.h"
    python fontconvert.py helvetica_${size}_${lower_style} $size \
      "$(strike_dir $size)/helv$(helvetica_style $style)$(source_pt $size).bdf" "$UNIFONT" \
      --2bit --compress --zopfli > $output_path
    echo "Generated $output_path"
  done
done

# UI fonts: UI_10_FONT_ID uses the 9pt strike (closest to the former 10pt UI
# font's pixel size); SMALL_FONT_ID uses 8pt.
UI_FONT_SIZES=(9 12)
UI_FONT_STYLES=("Regular" "Bold")

# Arabic glyphs for UI text (menus, file browser titles). The built-in fonts
# must cover the *output* of MiniBidi's do_shape() — contextual presentation
# forms — not base letters, or shaped UI text silently drops glyphs.
# Curated for firmware-size budget: core Arabic (Presentation Forms-B,
# incl. the Lam-Alef ligature forms) plus the Farsi/Urdu extra letters'
# Presentation Forms-A blocks, the few characters shaping leaves at their
# base codepoint, Arabic punctuation, and both digit sets. No harakat and
# no Sindhi/Pashto/Kurdish forms — book text gets those from SD-card fonts.
ARABIC_INTERVALS=(
  --additional-intervals 0x060C,0x060C  # Arabic comma
  --additional-intervals 0x061B,0x061B  # Arabic semicolon
  --additional-intervals 0x061F,0x061F  # Arabic question mark
  --additional-intervals 0x0621,0x0621  # hamza (non-joining, never shaped)
  --additional-intervals 0x0640,0x0640  # tatweel
  --additional-intervals 0x0654,0x0654  # Persian/Urdu ezafe hamza, Arabic hamza carriers
  --additional-intervals 0x0660,0x0669  # Arabic-Indic digits
  --additional-intervals 0x06BA,0x06BA  # noon ghunna base (initial/medial keep base cp)
  --additional-intervals 0x06D4,0x06D4  # Urdu full stop
  --additional-intervals 0x06D5,0x06D5  # ae (isolated; Kurdish/Uyghur/Ottoman) — has no presentation form
  --additional-intervals 0x06F0,0x06F9  # extended Arabic-Indic digits (Farsi/Urdu)
  --additional-intervals 0xFB56,0xFB59  # peh (Farsi)
  --additional-intervals 0xFB66,0xFB69  # tteh (Urdu)
  --additional-intervals 0xFB7A,0xFB7D  # tcheh (Farsi)
  --additional-intervals 0xFB88,0xFB95  # ddal, jeh, rreh (Urdu), keheh, gaf (Farsi/Urdu)
  --additional-intervals 0xFB9E,0xFB9F  # noon ghunna isolated/final (Urdu)
  --additional-intervals 0xFBA6,0xFBB1  # heh goal, heh doachashmee, yeh barree(+hamza) (Urdu)
  --additional-intervals 0xFBFC,0xFBFF  # farsi yeh (Farsi/Urdu)
  --additional-intervals 0xFE80,0xFEFC  # Presentation Forms-B: core Arabic + Lam-Alef
)

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="helveticaui_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size \
      "${X11_DIR}/helv$(helvetica_style $style)$(source_pt $size).bdf" "$UNIFONT" \
      --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py helveticaui_8_regular 8 \
  "${X11_DIR}/helvR$(source_pt 8).bdf" "$UNIFONT" \
  --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" > ../builtinFonts/helveticaui_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
