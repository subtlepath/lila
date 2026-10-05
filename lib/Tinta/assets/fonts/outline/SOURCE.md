# Vendored outline font

`texgyretermes-bold.otf` is TeX Gyre Termes Bold, version 2.004 (OpenType
name table: "Version 2.004;PS 2.004"), copied from TeX Live 2025
(`texmf-dist/fonts/opentype/public/tex-gyre/`), sha256
`2fb3e952065fa153c7e4e64e04b98b9d79225739b6025aa3f0f0782d299ff61e`.
Upstream: <https://www.gust.org.pl/projects/e-foundry/tex-gyre>.

Licence: the GUST Font License (`GUST-FONT-LICENSE.txt`), which is the LaTeX
Project Public License 1.3c (`lppl.txt`) plus a request — not a requirement —
that modified fonts be renamed. `MANIFEST-TeX-Gyre-Termes.txt` and
`README-TeX-Gyre-Termes.txt` come from the same TeX Live package. The file here
is unmodified; Tinta ships only bitmaps rasterised from it.

`tools/otf2freeink.py` rasterises it into the Spanish headword strikes
(`TermesBold50` … `TermesBold87` in `tools/fonts.manifest`): FreeType 2.13.2
monochrome with the font's own hints. The 34 px headword step and all body
text stay on the Adobe BDF strikes in `../bdf/`.
