#pragma once

#include <cstdint>

class GfxRenderer;

// CrossPoint's reading typography: the leading, measure and body alignment every book is set in. The built-in faces
// are bitmap strikes, so their leading is a designed pixel pitch per strike rather than a multiplier of the font's
// line box. Validate changes with test/typeset_preview.
namespace ReaderTypography {

// Longest line, in average characters of running text, before the side margins grow to hold it. Only landscape and
// the smallest sizes reach it.
constexpr int MAX_MEASURE_CHARS = 70;
// Shortest line that is justified; shorter ones are set ragged-right. In portrait, Times 12 (47 characters) opens a
// gap past twice a space on 7% of justified lines, Helvetica 12 (43) and Times 14 (41) on 14%.
constexpr int MIN_JUSTIFIED_CHARS = 45;

// Line pitch in px for a built-in face at a CrossPointSettings::LINE_COMPRESSION step, or 0 when `pointSize` has no
// built-in strike. WIDE, the default step, is the designed leading.
int builtinLinePitch(bool sans, uint8_t pointSize, uint8_t lineSpacing);

// Average advance of running text in 1/16 px: lowercase letters weighted by English letter frequency, plus the
// word spaces between them.
int averageCharAdvanceFP(const GfxRenderer& renderer, int fontId);

// Extra inset for each side that keeps a line `availableWidth` px wide within MAX_MEASURE_CHARS.
int measureInset(const GfxRenderer& renderer, int fontId, int availableWidth);

// Whether a line `width` px wide holds enough characters to be justified.
bool measureJustifies(const GfxRenderer& renderer, int fontId, int width);

}  // namespace ReaderTypography
