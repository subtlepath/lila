#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "components/UITheme.h"
#include "components/icons/hintIcons.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuColumns = 2;
// SMALL_FONT_ID has little space above its capitals; rotated hint labels
// shift this far inside their strip to sit clear of the screen edge.
constexpr int rotatedHintInset = 4;

// The chevron for a direction hint (Up, Down, Left, Right), or null for a
// label that names an action.
const freeink::Icon* directionIcon(const char* label) {
  if (strcmp(label, tr(STR_DIR_UP)) == 0) return &icon_hint_up_24;
  if (strcmp(label, tr(STR_DIR_DOWN)) == 0) return &icon_hint_down_24;
  if (strcmp(label, tr(STR_DIR_LEFT)) == 0) return &icon_hint_left_24;
  if (strcmp(label, tr(STR_DIR_RIGHT)) == 0) return &icon_hint_right_24;
  return nullptr;
}

// Lucide icons are not pre-rotated (GfxRenderer::drawIcon expects that), so
// plot them through drawPixel in the current orientation.
void drawLineIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y) {
  const int rowBytes = (icon.w + 7) / 8;
  for (int row = 0; row < icon.h; row++) {
    for (int col = 0; col < icon.w; col++) {
      if (((icon.bits[row * rowBytes + (col >> 3)] >> (7 - (col & 7))) & 1) == 0) {
        renderer.drawPixel(x + col, y + row, true);
      }
    }
  }
}

}  // namespace

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
    }
  }
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  const int contentWidth = std::max(0, rect.width - LyraMetrics::values.contentSidePadding * 2);

  int labelWidth = contentWidth;
  if (rightLabel) {
    auto truncatedRightLabel = renderer.truncatedText(SMALL_FONT_ID, rightLabel, contentWidth, EpdFontFamily::REGULAR);
    const int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - LyraMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 11, truncatedRightLabel.c_str());
    labelWidth = std::max(0, contentWidth - rightLabelWidth - hPaddingInSelection);
  }

  if (labelWidth > 0) {
    auto truncatedLabel = renderer.truncatedText(UI_10_FONT_ID, label, labelWidth, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding, rect.y + 9,
                      truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // A plain band over the front keys: one rule, each label centered over its
  // key, and the four directions as chevrons.
  const int pageWidth = renderer.getScreenWidth();
  constexpr int bandHeight = LyraMetrics::values.buttonHintsHeight;
  const int bandTop = renderer.getScreenHeight() - bandHeight;
  constexpr int buttonWidth = 80;
  // Keyed to the portrait panel width: the 528-wide X3 gets more spacing than
  // the 480-wide boards (X4, X4 Pro, and the other 800x480 panels).
  constexpr int narrowButtonPositions[] = {58, 146, 254, 342};
  constexpr int wideButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = pageWidth >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  // Zero gray-plane bits leave the monochrome band from the base pass intact.
  if (renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute()) {
    renderer.fillRect(0, bandTop, pageWidth, bandHeight, true);
    renderer.setOrientation(orig_orientation);
    return;
  }

  renderer.fillRect(0, bandTop, pageWidth, bandHeight, false);
  renderer.fillRect(0, bandTop, pageWidth, 1, true);
  const int textYOffset = (bandHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  for (int i = 0; i < 4; i++) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;
    const int x = buttonPositions[i];
    if (const freeink::Icon* icon = directionIcon(labels[i])) {
      drawLineIcon(renderer, *icon, x + (buttonWidth - icon->w) / 2, bandTop + (bandHeight - icon->h) / 2);
    } else {
      drawHintLabel(renderer, UI_10_FONT_ID, labels[i], x, buttonWidth, bandTop, bandHeight, textYOffset);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;

  if (gpio.hasEdgeSideButtons()) {
    // Edge-button layout (X3, X4 Pro): Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin + rotatedHintInset,
                                   x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX + rotatedHintInset, x3ButtonY + (buttonHeight + textWidth) / 2,
                                   bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x + rotatedHintInset, y + (buttonHeight + textWidth) / 2,
                                     labels[i]);
      }
    }
  }
}
