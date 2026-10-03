#include <BidiUtils.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdlib>

namespace {

constexpr int trackingBetween(const uint32_t leftCp, const uint32_t rightCp, const int8_t tracking) {
  const auto isSpace = [](const uint32_t cp) { return cp == ' ' || cp == 0xA0 || cp == 0x3000; };
  return leftCp == 0 || isSpace(leftCp) || isSpace(rightCp) ? 0 : tracking;
}

}  // namespace

GfxRenderer::GfxRenderer() : decompressor(new FontDecompressor()) { decompressor->init(); }

GfxRenderer::~GfxRenderer() { delete decompressor; }

void GfxRenderer::setCanvas(const int width, const int height) {
  canvasWidth = width;
  canvasHeight = height;
  pixels.assign(static_cast<size_t>(width) * height, 0xFF);
}

void GfxRenderer::clearScreen(const uint8_t color) { std::fill(pixels.begin(), pixels.end(), color); }

const EpdFontFamily* GfxRenderer::family(const int fontId) const {
  const auto it = fontMap.find(fontId);
  if (it == fontMap.end()) {
    std::fprintf(stderr, "font %d not registered\n", fontId);
    std::abort();
  }
  return &it->second;
}

void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  if (x < 0 || y < 0 || x >= canvasWidth || y >= canvasHeight) return;
  pixels[static_cast<size_t>(y) * canvasWidth + x] = state ? 0x00 : 0xFF;
}

void GfxRenderer::drawLine(int x1, int y1, const int x2, const int y2, const bool state) const {
  const int dx = std::abs(x2 - x1);
  const int dy = -std::abs(y2 - y1);
  const int sx = x1 < x2 ? 1 : -1;
  const int sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    drawPixel(x1, y1, state);
    if (x1 == x2 && y1 == y2) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x1 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y1 += sy;
    }
  }
}

void GfxRenderer::drawLine(const int x1, const int y1, const int x2, const int y2, const int lineWidth,
                           const bool state) const {
  for (int i = 0; i < lineWidth; ++i) drawLine(x1, y1 + i, x2, y2 + i, state);
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x, y + height - 1, x + width - 1, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  fillRectGray(x, y, width, height, state ? 0x00 : 0xFF);
}

void GfxRenderer::fillRectGray(const int x, const int y, const int width, const int height, const uint8_t gray) const {
  for (int row = std::max(0, y); row < std::min(canvasHeight, y + height); ++row) {
    for (int col = std::max(0, x); col < std::min(canvasWidth, x + width); ++col) {
      pixels[static_cast<size_t>(row) * canvasWidth + col] = gray;
    }
  }
}

const uint8_t* GfxRenderer::glyphBitmap(const EpdFontData* data, const EpdGlyph* glyph) const {
  if (data->groups != nullptr) {
    return decompressor->getBitmap(data, glyph, static_cast<uint32_t>(glyph - data->glyph));
  }
  return &data->bitmap[glyph->dataOffset];
}

void GfxRenderer::renderChar(const EpdFontFamily& font, const uint32_t cp, const int cursorX, const int cursorY,
                             const bool black, const EpdFontFamily::Style style) const {
  const EpdGlyph* glyph = font.getGlyph(cp, style);
  if (!glyph) return;
  const EpdFontData* data = font.getData(style);
  const uint8_t* bitmap = glyphBitmap(data, glyph);
  if (!bitmap) return;
  const int x0 = cursorX + glyph->left;
  const int y0 = cursorY - glyph->top;
  for (int row = 0; row < glyph->height; ++row) {
    for (int col = 0; col < glyph->width; ++col) {
      const int source = row * glyph->width + col;
      const uint8_t ink = data->is2Bit ? ((bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3)
                                       : ((bitmap[source >> 3] >> (7 - (source & 7))) & 1);
      if (ink != 0) drawPixel(x0 + col, y0 + row, black);
    }
  }
}

void GfxRenderer::renderCharScaled(const EpdFontFamily& font, const uint32_t cp, const int cursorX, const int cursorY,
                                   const bool black, const EpdFontFamily::Style style) const {
  const EpdGlyph* glyph = font.getGlyph(cp, style);
  if (!glyph) return;
  const EpdFontData* data = font.getData(style);
  const uint8_t* bitmap = glyphBitmap(data, glyph);
  if (!bitmap) return;
  const int srcW = glyph->width;
  const int srcH = glyph->height;
  const int baseX = cursorX + glyph->left / 2;
  const int baseY = cursorY - glyph->top / 2;
  for (int dstY = 0; dstY < (srcH + 1) / 2; ++dstY) {
    for (int dstX = 0; dstX < (srcW + 1) / 2; ++dstX) {
      uint8_t coverage = 0;
      uint8_t maxRaw = 0;
      for (int sy = 0; sy < 2 && dstY * 2 + sy < srcH; ++sy) {
        for (int sx = 0; sx < 2 && dstX * 2 + sx < srcW; ++sx) {
          const int pos = (dstY * 2 + sy) * srcW + dstX * 2 + sx;
          const uint8_t raw = data->is2Bit ? ((bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 3)
                                           : static_cast<uint8_t>(((bitmap[pos >> 3] >> (7 - (pos & 7))) & 1) * 3);
          coverage += raw;
          maxRaw = std::max(maxRaw, raw);
        }
      }
      if (maxRaw >= 2 || coverage >= 2) drawPixel(baseX + dstX, baseY + dstY, black);
    }
  }
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style, BidiUtils::BidiBaseDir, const int8_t tracking) const {
  if (text == nullptr || *text == '\0') return;
  const EpdFontFamily& font = *family(fontId);
  const int yPos = y + getFontAscenderSize(fontId);
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;
  uint32_t prevCp = 0;
  const char* cursor = text;
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor))) {
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      const EpdGlyph* mark = font.getGlyph(cp, style);
      if (!mark) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy = combiningMark::raiseAboveBase(anchor, mark->top, mark->height, lastBaseTop);
      const int markX =
          combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth, mark->left, mark->width);
      renderChar(font, cp, markX, yPos - raiseBy, black, style);
      continue;
    }
    cp = font.applyLigatures(cp, cursor, style);
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP) + trackingBetween(prevCp, cp, tracking);
    }
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    const bool supSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    if (supSub) {
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
      renderCharScaled(font, cp, lastBaseX, yPos, black, style);
    } else {
      renderChar(font, cp, lastBaseX, yPos, black, style);
    }
    prevCp = cp;
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              BidiUtils::BidiBaseDir) const {
  if (text == nullptr || *text == '\0') return 0;
  int w = 0;
  int h = 0;
  family(fontId)->getTextDimensions(text, &w, &h, style);
  return w;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  const EpdGlyph* space = family(fontId)->getGlyph(' ', style);
  return space ? fp4::toPixel(space->advanceX) : 0;
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  const EpdFontFamily& font = *family(fontId);
  const EpdGlyph* space = font.getGlyph(' ', style);
  const int32_t spaceFP = space ? static_cast<int32_t>(space->advanceX) : 0;
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceFP + kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style, const int8_t tracking) const {
  return fp4::toPixel(family(fontId)->getKerning(leftCp, rightCp, style)) + trackingBetween(leftCp, rightCp, tracking);
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, const EpdFontFamily::Style style,
                                 const int8_t tracking, BidiUtils::BidiBaseDir, TextMeasureMode) const {
  const EpdFontFamily& font = *family(fontId);
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;
  while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
    if (BidiUtils::isTransparentMark(cp) || utf8IsCombiningMark(cp)) continue;
    cp = font.applyLigatures(cp, text, style);
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP) + trackingBetween(prevCp, cp, tracking);
    }
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    prevCp = cp;
  }
  return widthPx + fp4::toPixel(prevAdvanceFP);
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  return family(fontId)->getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  return family(fontId)->getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getLineHeight(const int fontId, const float compression) const {
  return static_cast<int>(getLineHeight(fontId) * compression + 0.5f);
}

int GfxRenderer::getTextHeight(const int fontId) const { return getFontAscenderSize(fontId); }
