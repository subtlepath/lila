#include "CompanionReaderPreferences.h"

#if LILA_COMPANION
#include <Epub.h>
#include <HalIdentityStorage.h>
#include <Memory.h>

#include <mutex>

#include "CompanionBookmarkReplay.h"
#include "CompanionBookmarkSaveSession.h"
#include "CompanionReaderPreferenceSaveSession.h"
#include "CompanionReadingPositionReplay.h"
#include "CompanionReadingPositionSaveSession.h"
#include "SdCardFontSystem.h"
#include "activities/reader/ProgressFile.h"

namespace companion {
namespace {
struct PreferenceRuntime {
  std::mutex mutex;
  ReaderPreferenceValues baseline;
  SdCardFontSystem* fonts = nullptr;
  uint64_t progressRevision = 0;
  bool suspended = true, failed = false, saveError = false;
};
PreferenceRuntime runtime;
constexpr size_t PREFERENCE_SCRATCH_SIZE = 8192;

bool bookIdentity(const Epub& epub, Digest& digest) {
  if (epub.hasCompanionContentIdentity()) return epub.getCompanionContentIdentity({}, digest);
  // Proof scratch exceeds stack limits and is released before journal allocation.
  auto scratch = makeUniqueNoThrow<uint8_t[]>(PREFERENCE_SCRATCH_SIZE);
  if (!scratch) {
    LOG_ERR("COMPANION", "OOM: reading identity scratch");
    return false;
  }
  return epub.getCompanionContentIdentity(std::span(scratch.get(), PREFERENCE_SCRATCH_SIZE), digest);
}
bool publishProgress(void* context, const ReadingAnchor& anchor) {
  const auto& epub = *static_cast<const Epub*>(context);
  std::array<uint8_t, 10> bytes{};
  bytes[0] = anchor.spine & 0xff;
  bytes[1] = anchor.spine >> 8;
  for (unsigned at = 0; at < 4; ++at) bytes[6 + at] = anchor.visibleTextOffset >> (8 * at);
  return ProgressFile::writeAtomic(epub.getCachePath(), bytes.data(), bytes.size());
}
bool save(void* context, const ReaderPreferenceValues& candidate, ReaderPreferenceValues& rollback) {
  auto& owner = *static_cast<PreferenceRuntime*>(context);
  std::lock_guard<std::mutex> lock(owner.mutex);
  rollback = owner.baseline;
  if (!ReaderPreferenceChangeCapture::changedKeys(owner.baseline, candidate)) return true;
  if (owner.suspended || owner.failed || !owner.fonts) {
    LOG_ERR("COMPANION", "Reader preference capture unavailable");
    owner.saveError = true;
    return false;
  }
  // Shared proof scratch and metadata/journal owners exceed the task stack.
  // Allocate only for changed portable fields, and release before JSON serialization.
  auto scratch = makeUniqueNoThrow<uint8_t[]>(PREFERENCE_SCRATCH_SIZE);
  if (!scratch) {
    LOG_ERR("COMPANION", "OOM: reader preference save scratch");
    owner.saveError = true;
    return false;
  }
  auto session = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(
      owner.fonts->registry(), std::span(scratch.get(), PREFERENCE_SCRATCH_SIZE));
  if (!session) {
    LOG_ERR("COMPANION", "OOM: reader preference save session");
    owner.saveError = true;
    return false;
  }
  HalIdentityStorage identities;
  const auto result = session->persist(owner.baseline, candidate, identities);
  owner.failed = session->requiresRecovery();
  session.reset();
  if (result != TintaJournalResult::Ok) {
    LOG_ERR("COMPANION", "Reader preference save refused: %u", static_cast<unsigned>(result));
    owner.saveError = true;
    return false;
  }
  owner.baseline = candidate;
  return true;
}
}  // namespace

ReaderPreferenceApplicationResult restoreReaderPreferenceRuntime(CrossPointSettings& settings, SdCardFontSystem& fonts,
                                                                 bool enableCapture) {
  // Unbind before taking the runtime mutex: saves acquire settings then runtime.
  settings.unbindPortablePreferenceSave(&runtime);
  ReaderPreferenceApplicationResult result = ReaderPreferenceApplicationResult::IoError;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    runtime.suspended = true;
    ++runtime.progressRevision;
    bool importRequiresRecovery = false;
    fonts.refreshIfDirty();
    auto scratch = makeUniqueNoThrow<uint8_t[]>(PREFERENCE_SCRATCH_SIZE);
    if (scratch) {
      auto phase = makeUniqueNoThrow<NativeReaderPreferencePhase>(settings, fonts.registry(),
                                                                  std::span(scratch.get(), PREFERENCE_SCRATCH_SIZE));
      if (phase)
        result = phase->apply();
      else
        LOG_ERR("COMPANION", "OOM: reader preference startup phase");
      phase.reset();
      settings.readPortablePreferences(runtime.baseline);
      if (enableCapture && (result == ReaderPreferenceApplicationResult::Applied ||
                            result == ReaderPreferenceApplicationResult::Unchanged)) {
        // Replay and import owners never overlap; reuse the same bounded scratch.
        auto importer = makeUniqueNoThrow<NativeReaderPreferenceSaveSession>(
            fonts.registry(), std::span(scratch.get(), PREFERENCE_SCRATCH_SIZE));
        if (!importer) {
          LOG_ERR("COMPANION", "OOM: initial reader preference import");
          importRequiresRecovery = true;
        } else {
          HalIdentityStorage identities;
          const auto imported = importer->importMissing(runtime.baseline, identities);
          importRequiresRecovery = importer->requiresRecovery() ||
                                   (imported != TintaJournalResult::Ok && imported != TintaJournalResult::Conflict &&
                                    imported != TintaJournalResult::Invalid);
          if (imported != TintaJournalResult::Ok)
            LOG_ERR("COMPANION", "Initial reader preference import refused: %u", static_cast<unsigned>(imported));
        }
      }
    } else {
      LOG_ERR("COMPANION", "OOM: reader preference startup scratch");
    }
    settings.readPortablePreferences(runtime.baseline);
    runtime.fonts = &fonts;
    runtime.failed = importRequiresRecovery || result == ReaderPreferenceApplicationResult::IoError ||
                     result == ReaderPreferenceApplicationResult::InvalidBody;
    runtime.suspended = !enableCapture;
  }
  settings.bindPortablePreferenceSave(&runtime, &save);
  return result;
}
TintaJournalResult restoreReaderProgress(const Epub& epub) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.suspended || runtime.failed) return TintaJournalResult::Unavailable;
  Digest edition{};
  if (!bookIdentity(epub, edition)) return TintaJournalResult::IoError;
  ++runtime.progressRevision;
  NativeReadingPositionReplay replay;
  const auto result = replay.run(edition, epub.getSpineItemsCount(), &publishProgress, const_cast<Epub*>(&epub));
  if (result == TintaJournalResult::IoError) runtime.failed = true;
  return result;
}
bool saveReaderProgress(const Epub& epub, const ReadingAnchor& anchor, std::span<const uint8_t> bytes) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.suspended || runtime.failed || anchor.spine >= epub.getSpineItemsCount()) {
    LOG_ERR("COMPANION", "Reading capture unavailable or outside book");
    return false;
  }
  if (epub.matchesCompanionProgress(runtime.progressRevision, bytes)) return true;
  Digest edition{};
  if (!bookIdentity(epub, edition)) return false;
  // Writer/event owners exceed stack limits; release immediately after publication.
  auto session = makeUniqueNoThrow<NativeReadingPositionSaveSession>();
  if (!session) {
    LOG_ERR("COMPANION", "OOM: reading save session");
    return false;
  }
  HalIdentityStorage identities;
  const auto result = session->persist(anchor, edition, identities);
  runtime.failed = session->requiresRecovery();
  session.reset();
  if (result != TintaJournalResult::Ok) {
    LOG_ERR("COMPANION", "Reading save refused: %u", static_cast<unsigned>(result));
    return false;
  }
  ++runtime.progressRevision;
  if (!ProgressFile::writeAtomic(epub.getCachePath(), bytes.data(), bytes.size())) return false;
  epub.rememberCompanionProgress(runtime.progressRevision, bytes);
  return true;
}
TintaJournalResult captureReaderBookmark(const Epub& epub, const BookmarkBodyView& bookmark) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.suspended || runtime.failed) {
    LOG_ERR("COMPANION", "Bookmark capture unavailable");
    return TintaJournalResult::Unavailable;
  }
  if (!bookmark.deleted && bookmark.anchor.spine >= epub.getSpineItemsCount()) {
    LOG_ERR("COMPANION", "Bookmark capture outside book");
    return TintaJournalResult::Invalid;
  }
  Digest edition{};
  if (!bookIdentity(epub, edition)) return TintaJournalResult::IoError;
  // Body/event owners exceed stack limits and are released before JSON publication.
  auto session = makeUniqueNoThrow<NativeBookmarkSaveSession>();
  if (!session) {
    LOG_ERR("COMPANION", "OOM: bookmark save session");
    return TintaJournalResult::IoError;
  }
  HalIdentityStorage identities;
  const auto result = session->persist(bookmark, edition, identities);
  runtime.failed = session->requiresRecovery();
  session.reset();
  if (result != TintaJournalResult::Ok) {
    LOG_ERR("COMPANION", "Bookmark save refused: %u", static_cast<unsigned>(result));
    return result;
  }
  ++runtime.progressRevision;
  return TintaJournalResult::Ok;
}
TintaJournalResult restoreReaderBookmark(const Epub& epub, const Identity& bookmark, ReaderBookmarkPublisher publish,
                                         void* context) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.suspended || runtime.failed) return TintaJournalResult::Unavailable;
  Digest edition{};
  if (!bookIdentity(epub, edition)) return TintaJournalResult::IoError;
  auto replay = makeUniqueNoThrow<NativeBookmarkReplay>();
  if (!replay) {
    LOG_ERR("COMPANION", "OOM: bookmark replay owner");
    return TintaJournalResult::IoError;
  }
  ++runtime.progressRevision;
  const auto result = replay->run(edition, bookmark, epub.getSpineItemsCount(), publish, context);
  if (result == TintaJournalResult::IoError) runtime.failed = true;
  return result;
}
void suspendReaderPreferenceCapture() {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  runtime.suspended = true;
}
void resumeReaderPreferenceCapture() {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  runtime.suspended = false;
}
void notifyReaderPreferenceSaveError() {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  runtime.saveError = true;
}
bool takeReaderPreferenceSaveError() {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  const bool result = runtime.saveError;
  runtime.saveError = false;
  return result;
}
}  // namespace companion
#endif
