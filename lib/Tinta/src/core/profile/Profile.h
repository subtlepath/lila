#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/StateStore.h"

namespace tinta::core {

enum class TextSize : uint8_t { Small = 0, Medium = 1, Large = 2 };
enum class UiLanguage : uint8_t { English = 0, Spanish = 1, Auto = 2 };
// The standalone firmware's sleep screen setting; lila does not use it.
// Word: the weakest word, streak and tomorrow's count (PLAN.md 4.7).
// Rotating: the same, redrawn by timer wakes where the board allows it.
// Plain: a static Tinta screen.
enum class SleepScreen : uint8_t { Word = 0, Rotating = 1, Plain = 2 };

// The learner's settings and the little state that goes with them, in
// profile.bin (PLAN.md 8.3), written whole with StateStore::replace().
//
//   0 magic "TPRF"  4 version u16  6 payload length u16  8 payload  then crc32
//
// Payload fields sit at fixed offsets and are only ever appended. A shorter
// payload (an older version) gives defaults for the fields it lacks; a longer
// one (a newer version) has its extra fields ignored; a field outside its
// range takes its default. A missing or corrupt file gives all defaults.
// Fields marked "standalone" were the standalone firmware's settings; lila
// keeps them in the file but uses its own.
struct Profile {
  static constexpr const char* kFile = "profile.bin";
  static constexpr uint16_t kVersion = 1;
  static constexpr uint32_t kPayloadSize = 31;
  static constexpr uint32_t kEncodedSize = 8 + kPayloadSize + 4;

  enum class LoadResult : uint8_t {
    Loaded,    // this version, every field valid
    Upgraded,  // another version or out-of-range fields; save() to rewrite
    Defaults,  // no file
    Corrupt,   // unreadable; defaults
  };

  uint16_t newPerDay = 10;           // 0..200
  uint16_t reviewCap = 100;          // 0..9999
  uint16_t retentionPermille = 900;  // 700..970: desired retention 0.70..0.97
  uint16_t maxInterval = 365;        // 1..36500 days
  uint16_t sessionSize = 40;         // 5..500 items
  TextSize textSize = TextSize::Medium;
  UiLanguage uiLanguage = UiLanguage::English;
  bool showVulgar = false;
  uint8_t fullRefreshEvery = 8;                 // 1..50 transitions
  uint16_t sleepTimeoutSeconds = 180;           // 30..3600; standalone
  SleepScreen sleepScreen = SleepScreen::Word;  // standalone
  uint8_t rolloverHour = 4;                     // 0..23, local
  int16_t utcOffsetMinutes = 0;                 // -720..840; standalone
  DayNumber lastConfirmedDay = 0;               // the date confirmed without a trusted clock; 0 = never
  uint16_t currentLesson = 0;
  uint16_t unlockedThrough = 0;
  uint8_t frontlightBrightness = 50;  // 0..100 %; standalone
  uint8_t frontlightWarmth = 50;      // 0..100 %; standalone
  bool typedAnswers = false;          // touch devices: type produce, cloze and conjugation answers
  uint16_t sleepCount = 0;            // sleep cards drawn: picks the next one's word

  float desiredRetention() const { return static_cast<float>(retentionPermille) / 1000.0f; }

  // Fills *this from the file (defaults where needed).
  LoadResult load(StateStore& store);
  bool save(StateStore& store) const;

  void encode(uint8_t out[kEncodedSize]) const;
  LoadResult decode(const uint8_t* in, uint32_t length);

 private:
  // `in` holds the 8-byte header and the first `available` payload bytes,
  // already checked.
  LoadResult decodeFields(const uint8_t* in, uint32_t available);
};

}  // namespace tinta::core
