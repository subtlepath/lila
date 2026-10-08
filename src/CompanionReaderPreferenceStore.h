#pragma once

#include <CrossPointSettings.h>
#include <I18n.h>
#include <Logging.h>

#include "CompanionReaderPreferenceApplication.h"

namespace companion {
// The serialized caller owns an audited durable journal and startup replay.
class NativeReaderPreferenceStore final : public ReaderPreferenceStore {
 public:
  using Proof = bool (*)(void*, const ReaderPreferenceValues&);
  NativeReaderPreferenceStore(CrossPointSettings& settings, Proof proof, void* context)
      : settings(settings), proof(proof), context(context) {}
  bool read(ReaderPreferenceValues& output) override {
    settings.readPortablePreferences(output);
    return true;
  }
  ReaderPreferenceStoreResult replace(const ReaderPreferenceValues& expected,
                                      const ReaderPreferenceValues& replacement) override {
    if (!proof || !proof(context, replacement)) {
      LOG_ERR("COMPANION", "Reader preference durable authority proof failed");
      return ReaderPreferenceStoreResult::IoError;
    }
    const auto result = settings.applyPortablePreferencesIfUnchanged(expected, replacement);
    if (result == ReaderPreferenceStoreResult::Ok)
      I18N.setLanguage(static_cast<Language>(replacement.language));
    else
      LOG_ERR("COMPANION", "Reader preferences changed before application");
    return result;
  }

 private:
  CrossPointSettings& settings;
  Proof proof;
  void* context;
};
}  // namespace companion
