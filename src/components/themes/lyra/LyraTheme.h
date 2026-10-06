#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Lyra theme metrics (zero runtime cost)
namespace LyraMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 // One band: title, clock and battery share a single line, ruled below.
                                 // 6px of paper above it balances the band against the glass edge.
                                 .topPadding = 6,
                                 .batteryBarHeight = 46,
                                 .headerHeight = 46,
                                 .verticalSpacing = 16,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 // Rows, header title and info lines share one text column:
                                 // listInset + listSidePadding == headerSidePadding.
                                 .listRowHeight = 44,
                                 .listWithSubtitleRowHeight = 60,
                                 .listRowGap = 0,
                                 .listRowRadius = 0,
                                 .listInset = 8,
                                 .listSidePadding = 16,
                                 // Square black bar across the row: bitmap text stays on solid paper
                                 // or solid ink, never on a dither.
                                 .listSelectionStyle = 0,
                                 .listScrollWidth = 4,
                                 .listScrollSide = 0,
                                 .listTitleBold = false,
                                 .headerSidePadding = 24,
                                 .headerUnderlineSize = 2,
                                 .headerTitleAlign = 0,  // left
                                 .headerBatterySide = 0,
                                 // The title holds the left of the single line, so the clock centers.
                                 .headerClockCentered = true,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 48,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 52,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 14,
                                 .keyboardKeyHeight = 48,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.165f,
                                 .popupMarginX = 16,
                                 .popupMarginY = 12,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = 0,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .optionPopupItemSpacing = 8,
                                 .optionPopupInnerPadding = 20,
                                 .optionPopupSelectionVPadding = 12,
                                 .optionPopupDialogSideMargin = 20,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0,
                                 .controlRadius = 6,
                                 .sheetRadius = 6,
                                 .capsuleRadius = 6};
}  // namespace LyraMetrics

class LyraTheme : public BaseTheme {
 public:
  // Component drawing methods
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  bool showsFileIcons() const override { return true; }

  // The front-key hints as one plain band on the bottom edge: a 1px rule,
  // labels in `fontId` centred over their keys (line box `labelTop` below the
  // band top), directions as chevrons centred between `iconAreaTop` and
  // `iconAreaBottom` above the band's bottom. With `aboveBezel`, the band sits
  // on the bezel's bottom viewable inset instead.
  static void drawHintBand(GfxRenderer& renderer, const char* const labels[4], int bandHeight, int fontId, int labelTop,
                           int iconAreaTop, bool aboveBezel = false, int iconAreaBottom = 0);
};
