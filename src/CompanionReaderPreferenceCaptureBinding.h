#pragma once

#include <CrossPointSettings.h>

#include "CompanionReaderPreferenceChangeCapture.h"
#include "CompanionReaderPreferenceMetadata.h"

namespace companion {
// Retain off-stack. Bootstrap excludes settings editors, journal/content writers
// and registry discovery. Writer, metadata banks and initial heads outlive this binding.
class NativeReaderPreferenceCaptureBinding final {
 public:
  NativeReaderPreferenceCaptureBinding(CrossPointSettings& settings, TintaWriter& writer,
                                       NativeReaderPreferenceMetadata& metadata,
                                       PreferenceKnowledgeHeads* initialHeads = nullptr)
      : settings(settings), writer(writer), metadata(metadata), capture(writer, initialHeads) {}
  ~NativeReaderPreferenceCaptureBinding() { settings.unbindPortablePreferenceSave(this); }
  NativeReaderPreferenceCaptureBinding(const NativeReaderPreferenceCaptureBinding&) = delete;
  NativeReaderPreferenceCaptureBinding& operator=(const NativeReaderPreferenceCaptureBinding&) = delete;
  bool initialize() {
    if (initialized || !writer.available()) return false;
    ReaderPreferenceValues values;
    settings.readPortablePreferences(values);
    if (!capture.initialize(values)) {
      LOG_ERR("COMPANION", "Reader preference save binding initialization failed");
      return false;
    }
    initialized = true;
    settings.bindPortablePreferenceSave(this, &save);
    return true;
  }

 private:
  static bool save(void* context, const ReaderPreferenceValues& values, ReaderPreferenceValues& rollback) {
    auto& owner = *static_cast<NativeReaderPreferenceCaptureBinding*>(context);
    rollback = owner.capture.baselineValues();
    const auto result = owner.capture.persist(values, &encode, &owner.metadata, 0, 0, ClockQuality::Unknown);
    if (result == TintaJournalResult::Ok || result == TintaJournalResult::Duplicate) return true;
    LOG_ERR("COMPANION", "Reader preference save journal failed: %u", static_cast<unsigned>(result));
    return false;
  }
  static size_t encode(void* context, const ReaderPreferenceValues& values, uint8_t key, std::span<uint8_t> output) {
    return static_cast<NativeReaderPreferenceMetadata*>(context)->encodePreference(values, key, output);
  }
  CrossPointSettings& settings;
  TintaWriter& writer;
  NativeReaderPreferenceMetadata& metadata;
  ReaderPreferenceChangeCapture capture;
  bool initialized = false;
};
}  // namespace companion
