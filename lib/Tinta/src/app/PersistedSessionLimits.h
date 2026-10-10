#pragma once

#include <stdint.h>

namespace tinta::app {
inline constexpr uint8_t kSessionStackCapacity = 8;
inline constexpr uint16_t kSessionQueueCapacity = 200;

// session.bin and the usage log store these numbers, so an id is never
// removed or reordered. Reserved ids are the standalone firmware's screens,
// which lila has no use for; App::view() shows Home for them.
enum class ScreenId : uint8_t {
  None,
  Home,
  Settings,
  SettingsStudy,
  SettingsDisplay,
  SettingsTime,
  SettingsSleep,  // reserved
  About,
  Diagnostics,
  Specimen,  // reserved
  BringUp,   // reserved
  Pause,
  Light,
  DatePrompt,
  DatePicker,
  SetClock,  // reserved
  Session,
  Summary,
  Progress,
  Dictionary,
  DictLetters,
  DictEntry,
  VerbTable,
  PackError,
  Lesson,
  Course,
  Update,       // reserved
  UsbTransfer,  // reserved
  Readings,
  Reader,
  Quiz,
  Phrasebook,
  Phrases,
  Search,
  EntryActions,
  Welcome,
  KeyGuide,
  VulgarChoice,
  Licences,
  Count,
};
}  // namespace tinta::app
