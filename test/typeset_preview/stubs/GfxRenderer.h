#pragma once

// Host GfxRenderer for typeset previews: the layout-facing surface of lib/GfxRenderer backed by the real built-in
// bitmap fonts, drawing into an 8-bit canvas instead of the panel framebuffer. Measurement mirrors
// GfxRenderer.cpp exactly (12.4 fixed-point advances snapped together with the following kern, tracking skipped
// around spaces), so line breaks and word positions match the device. Bidi shaping and SD/fallback fonts are
// omitted; previews are for the built-in Latin faces.

#include <EpdFontFamily.h>
#include <Utf8.h>

#include <cstdint>
#include <map>
#include <vector>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class FontDecompressor;
class FontCacheManager;

class GfxRenderer {
 public:
  enum class TextMeasureMode { Layout, Rendered };

  GfxRenderer();
  ~GfxRenderer();

  void insertFont(int fontId, const EpdFontFamily& family) { fontMap.insert_or_assign(fontId, family); }

  // Canvas in logical (oriented) coordinates; 255 = paper, 0 = ink.
  void setCanvas(int width, int height);
  void clearScreen(uint8_t color = 0xFF);
  const std::vector<uint8_t>& canvas() const { return pixels; }
  int getScreenWidth() const { return canvasWidth; }
  int getScreenHeight() const { return canvasHeight; }

  bool isFontCacheScanning() const { return false; }
  FontCacheManager* getFontCacheManager() const { return nullptr; }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const char* const*, const size_t*, size_t, bool, bool, uint8_t) const {}
  bool glyphIntersectsStrip(int, int, int, int) const { return true; }
  void preserveImagePolarity(int, int, int, int) const {}

  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectGray(int x, int y, int width, int height, uint8_t gray) const;

  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t tracking = 0) const;

  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style, int8_t tracking = 0) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode mode = TextMeasureMode::Layout) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  int getLineHeight(int fontId, float compression) const;
  int getTextHeight(int fontId) const;

 private:
  const EpdFontFamily* family(int fontId) const;
  void renderChar(const EpdFontFamily& font, uint32_t cp, int cursorX, int cursorY, bool black,
                  EpdFontFamily::Style style) const;
  void renderCharScaled(const EpdFontFamily& font, uint32_t cp, int cursorX, int cursorY, bool black,
                        EpdFontFamily::Style style) const;
  const uint8_t* glyphBitmap(const EpdFontData* data, const EpdGlyph* glyph) const;

  std::map<int, EpdFontFamily> fontMap;
  FontDecompressor* decompressor;
  int canvasWidth = 0;
  int canvasHeight = 0;
  mutable std::vector<uint8_t> pixels;
};
