#pragma once
#include <string>

#include "activities/Activity.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  // frameRedrawn: the outgoing activity drew a new frame for Current Page
  // sleep (Activity::drawSleepFrame()), so it goes up with a clean refresh.
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false,
                         bool frameRedrawn = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout), frameRedrawn(frameRedrawn) {}
  void onEnter() override;

  // True when this sleep keeps the current frame on the glass (Current Page).
  static bool keepsCurrentFrame(bool fromTimeout);

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool preserveBackground = false) const;
  bool renderSleepOverlayFile(HalFile& file, const char* pathForLog) const;
  bool renderTransparentOverlayPng(const std::string& path) const;
  bool renderSleepOverlayPath(const std::string& path) const;
  void renderLastScreenSleepScreen() const;
  void renderTransparentCustomSleepScreen() const;
  void renderBlankSleepScreen() const;

  bool fromTimeout = false;
  bool frameRedrawn = false;
};
