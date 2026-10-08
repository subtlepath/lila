#pragma once

#include "CompanionReaderPreferenceApplication.h"
#include "CompanionReadingBody.h"
#include "CompanionTintaJournal.h"

class Epub;
class CrossPointSettings;
class SdCardFontSystem;

namespace companion {
// Startup or companion teardown only, with settings/content/journal writers stopped.
ReaderPreferenceApplicationResult restoreReaderPreferenceRuntime(CrossPointSettings& settings, SdCardFontSystem& fonts,
                                                                 bool enableCapture = true);
TintaJournalResult restoreReaderProgress(const Epub& epub);
bool saveReaderProgress(const Epub& epub, const ReadingAnchor& anchor, std::span<const uint8_t> bytes);
// Only Ok permits the caller to publish derived bookmark JSON.
TintaJournalResult captureReaderBookmark(const Epub& epub, const BookmarkBodyView& bookmark);
using ReaderBookmarkPublisher = bool (*)(void*, std::span<const uint8_t>);
// Publisher must complete checked storage and must not re-enter this runtime.
TintaJournalResult restoreReaderBookmark(const Epub& epub, const Identity& bookmark, ReaderBookmarkPublisher publish,
                                         void* context);
void suspendReaderPreferenceCapture();
void resumeReaderPreferenceCapture();
void notifyReaderPreferenceSaveError();
bool takeReaderPreferenceSaveError();
}  // namespace companion
