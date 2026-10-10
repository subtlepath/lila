#include "CompanionReaderPreferences.h"

#if LILA_COMPANION
#include <Epub.h>
#include <HalIdentityStorage.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include <mutex>

#include "CompanionBookmarkChoicePage.h"
#include "CompanionBookmarkReaderContext.h"
#include "CompanionBookmarkReaderSession.h"
#include "CompanionBookmarkReplay.h"
#include "CompanionBookmarkSaveSession.h"
#include "CompanionReaderBookmarks.h"
#include "CompanionReaderPreferenceSaveSession.h"
#include "CompanionReadingPositionReplay.h"
#include "CompanionReadingPositionSaveSession.h"
#include "SdCardFontSystem.h"
#include "activities/RenderLock.h"
#include "activities/reader/ProgressFile.h"
#include "util/BookmarkUtil.h"

namespace companion {
namespace {
struct PreferenceRuntime {
  std::mutex mutex;
  ReaderPreferenceValues baseline;
  SdCardFontSystem* fonts = nullptr;
  uint64_t progressRevision = 0;
  bool suspended = true, failed = false, saveError = false, bookmarkRecoveryRequired = false;
};
PreferenceRuntime runtime;
constexpr size_t PREFERENCE_SCRATCH_SIZE = 8192;

enum class BookmarkOperation {
  Restore,
  Create,
  Rename,
  Erase,
  EraseMatching,
  LoadChoices,
  ResolveChoice,
  DecideLegacy
};
struct BookmarkRequest {
  BookmarkOperation operation;
  BookmarkEntry* candidate = nullptr;
  const BookmarkEntry* selected = nullptr;
  std::string_view name{};
  ReaderBookmarkMatcher matches = nullptr;
  void* matchContext = nullptr;
  NativeBookmarkChoicePage* choices = nullptr;
  uint32_t choiceIndex = 0;
  LegacyBookmarkDecision legacyDecision = LegacyBookmarkDecision::Undecided;
};
// Path/proof owners exceed stack limits. One checked workspace is reused for
// EPUB identity, legacy migration and publication, then released with the batch.
class BookmarkRuntimeBatch final {
 public:
  BookmarkRuntimeBatch(Epub& epub, ReaderBookmarkBinding& binding) : epub(epub), binding(binding) {}
  TintaJournalResult run(const BookmarkRequest& request, std::vector<BookmarkEntry>& entries) {
    const bool restoring = request.operation == BookmarkOperation::Restore;
    const bool associating = request.operation == BookmarkOperation::DecideLegacy;
    const bool choosing =
        request.operation == BookmarkOperation::LoadChoices || request.operation == BookmarkOperation::ResolveChoice;
    if (restoring) {
      binding.ready = false;
      binding.conflict.fill(0);
      binding.legacyAssociationRequired = false;
      binding.legacyDecision = LegacyBookmarkDecision::Undecided;
    }
    decision = associating ? request.legacyDecision : binding.legacyDecision;
    const auto initialized = initialize(restoring, choosing, associating);
    if (initialized != TintaJournalResult::Ok) return initialized;
    TintaJournalResult result = TintaJournalResult::Invalid;
    switch (request.operation) {
      case BookmarkOperation::Restore:
      case BookmarkOperation::DecideLegacy:
        result = session->restoreEdition(active.data(), legacy.data(), spineCount, entries, decision,
                                         &NativeBookmarkReaderContext::resolveLegacy);
        break;
      case BookmarkOperation::Create:
        if (request.candidate) result = session->create(active.data(), spineCount, *request.candidate, entries);
        break;
      case BookmarkOperation::Rename:
        if (request.selected)
          result = session->rename(active.data(), spineCount, *request.selected, request.name, entries);
        break;
      case BookmarkOperation::Erase:
        if (request.selected) result = session->erase(active.data(), spineCount, *request.selected, entries);
        break;
      case BookmarkOperation::EraseMatching:
        result = session->eraseMatching(active.data(), spineCount, entries, request.matches, request.matchContext);
        break;
      case BookmarkOperation::LoadChoices:
      case BookmarkOperation::ResolveChoice:
        result = runChoice(request, entries);
        break;
    }
    recoveryRequired = recoveryRequired || (session && session->requiresRecovery()) ||
                       (request.choices && request.choices->requiresRecovery());
    if ((restoring || associating || request.operation == BookmarkOperation::ResolveChoice) &&
        result == TintaJournalResult::Ok)
      recoveryRequired = false;
    if (recoveryRequired) binding.ready = false;
    if ((restoring || associating || request.operation == BookmarkOperation::ResolveChoice) &&
        result == TintaJournalResult::Ok)
      publishBinding(true);
    if (result == TintaJournalResult::Conflict && session && session->needsLegacyAssociation() &&
        NativeBookmarkReaderContext::current(context.get())) {
      publishBinding(false);
      binding.legacyAssociationRequired = true;
    }
    if (result == TintaJournalResult::Conflict && session && tinta_body_detail::nonzero(session->conflictIdentity()) &&
        NativeBookmarkReaderContext::current(context.get()))
      publishBinding(false);
    return result;
  }
  bool requiresRecovery() const { return recoveryRequired; }

 private:
  void publishBinding(bool ready) {
    binding.edition = edition;
    binding.device = identity.device;
    binding.storageGeneration = identity.storageGeneration;
    binding.ready = ready;
    binding.legacyAssociationRequired = false;
    binding.legacyDecision = ready ? LegacyBookmarkDecision::Undecided : decision;
    if (ready)
      binding.conflict.fill(0);
    else
      binding.conflict = session->conflictIdentity();
  }
  TintaJournalResult runChoice(const BookmarkRequest& request, std::vector<BookmarkEntry>& entries) {
    if (!request.choices) return TintaJournalResult::Invalid;
    if (request.operation == BookmarkOperation::ResolveChoice && !request.choices->matches(edition, binding.conflict))
      return TintaJournalResult::Invalid;
    const auto prepared = session->prepareChoice(active.data(), spineCount);
    if (prepared != TintaJournalResult::Ok) return prepared;
    if (request.operation == BookmarkOperation::LoadChoices) {
      const auto loaded = request.choices->load(edition, binding.conflict, spineCount, request.choiceIndex);
      if (loaded == TintaJournalResult::Ok && !NativeBookmarkReaderContext::current(context.get())) {
        request.choices->invalidate();
        return failure("loaded choices context");
      }
      return loaded;
    }
    const auto saved = request.choices->resolve(request.choiceIndex, identities);
    if (saved != TintaJournalResult::Ok) return saved;
    recoveryRequired = true;
    // The preflight owner is one-shot; release it before replacing it for the
    // full restore, which publishes only after every remaining conflict resolves.
    session.reset();
    session = makeSession();
    if (!session) return failure("choice restore allocation");
    return session->restoreEdition(active.data(), legacy.data(), spineCount, entries, decision,
                                   &NativeBookmarkReaderContext::resolveLegacy);
  }
  std::unique_ptr<NativeBookmarkReaderSession> makeSession() {
    return makeUniqueNoThrow<NativeBookmarkReaderSession>(
        std::span(scratch.get(), PREFERENCE_SCRATCH_SIZE), edition, identity.storageGeneration, identities,
        &NativeBookmarkReaderContext::current, &NativeBookmarkReaderContext::migration,
        &NativeBookmarkReaderContext::publication, &NativeBookmarkReaderContext::preparation, context.get(),
        &NativeBookmarkReaderContext::release);
  }
  TintaJournalResult initialize(bool restoring, bool choosing, bool associating) {
    const auto count = epub.getSpineItemsCount();
    if (count <= 0 || count > 65536 || !BookmarkUtil::getBookmarkPath(epub.getPath(), legacy) ||
        !validInventoryPath(std::string_view(legacy.data())) || !hal_filename::valid(std::string_view(legacy.data()))) {
      binding.ready = false;
      LOG_ERR("COMPANION", "Bookmark reader path/spine unavailable");
      return TintaJournalResult::Invalid;
    }
    spineCount = count;
    scratch = makeUniqueNoThrow<uint8_t[]>(PREFERENCE_SCRATCH_SIZE);
    if (!scratch) return failure("workspace allocation");
    const std::span<uint8_t> workspace(scratch.get(), PREFERENCE_SCRATCH_SIZE);
    if (!epub.getCompanionContentIdentity(workspace, edition)) {
      binding.ready = false;
      return failure("edition proof");
    }
    if (!BookmarkUtil::getBookmarkEditionPath(edition, active)) return failure("canonical path");
    const auto prepared = restoring     ? prepareBookmarkReaderIdentity(identities, edition, identity)
                          : associating ? validateBookmarkAssociationBinding(identities, edition, binding, identity)
                          : choosing    ? validateBookmarkChoiceBinding(identities, edition, binding, identity)
                                        : validateBookmarkReaderBinding(identities, edition, binding, identity);
    if (prepared != TintaJournalResult::Ok) {
      recoveryRequired = restoring && prepared == TintaJournalResult::IoError;
      return prepared;
    }
    context = makeUniqueNoThrow<NativeBookmarkReaderContext>(epub, identities, edition, identity);
    if (!context) return failure("context allocation");
    session = makeSession();
    return session ? TintaJournalResult::Ok : failure("session allocation");
  }
  static TintaJournalResult failure(const char* operation) {
    LOG_ERR("COMPANION", "Bookmark runtime %s failed", operation);
    return TintaJournalResult::IoError;
  }
  Epub& epub;
  ReaderBookmarkBinding& binding;
  HalIdentityStorage identities;
  IdentityState identity{};
  Digest edition{};
  std::array<char, 128> active{};
  std::array<char, 512> legacy{};
  std::unique_ptr<uint8_t[]> scratch;
  std::unique_ptr<NativeBookmarkReaderContext> context;
  std::unique_ptr<NativeBookmarkReaderSession> session;
  uint32_t spineCount = 0;
  bool recoveryRequired = false;
  LegacyBookmarkDecision decision = LegacyBookmarkDecision::Undecided;
};

TintaJournalResult runReaderBookmarks(Epub& epub, ReaderBookmarkBinding& binding, std::vector<BookmarkEntry>& entries,
                                      const BookmarkRequest& request) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  const bool restoring = request.operation == BookmarkOperation::Restore;
  const bool associating = request.operation == BookmarkOperation::DecideLegacy;
  const bool choosing =
      request.operation == BookmarkOperation::LoadChoices || request.operation == BookmarkOperation::ResolveChoice;
  if (restoring) {
    binding.ready = false;
    binding.conflict.fill(0);
    binding.legacyAssociationRequired = false;
    binding.legacyDecision = LegacyBookmarkDecision::Undecided;
  }
  if (restoring || associating) entries.clear();
  if (runtime.suspended || runtime.failed ||
      (!restoring && !choosing && !associating && runtime.bookmarkRecoveryRequired)) {
    LOG_ERR("COMPANION", "Bookmark runtime unavailable or recovery required");
    return TintaJournalResult::Unavailable;
  }
  LOG_DBG("MEM", "bookmark batch %u begin: free=%u max_block=%u", static_cast<unsigned>(request.operation),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  auto batch = makeUniqueNoThrow<BookmarkRuntimeBatch>(epub, binding);
  if (!batch) {
    LOG_ERR("COMPANION", "OOM: bookmark runtime batch");
    return TintaJournalResult::IoError;
  }
  const auto result = batch->run(request, entries);
  if (batch->requiresRecovery()) runtime.bookmarkRecoveryRequired = true;
  if ((restoring || associating || request.operation == BookmarkOperation::ResolveChoice) &&
      result == TintaJournalResult::Ok)
    runtime.bookmarkRecoveryRequired = false;
  batch.reset();
  LOG_DBG("MEM", "bookmark batch %u end: free=%u max_block=%u", static_cast<unsigned>(request.operation),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  if (result != TintaJournalResult::Ok)
    LOG_ERR("COMPANION", "Bookmark runtime refused: %u", static_cast<unsigned>(result));
  return result;
}

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
bool persistPreferences(PreferenceRuntime& owner, const ReaderPreferenceValues& candidate,
                        uint16_t contentChanges = 0) {
  if (!contentChanges && !ReaderPreferenceChangeCapture::changedKeys(owner.baseline, candidate)) return true;
  if (owner.suspended || owner.failed || !owner.fonts) {
    LOG_ERR("COMPANION", "Reader preference capture unavailable");
    owner.saveError = true;
    return false;
  }
  // Shared proof scratch and metadata/journal owners exceed the task stack.
  // Allocate for changed fields or explicit dependency proof, then release before settings serialization.
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
  const auto result = session->persist(owner.baseline, candidate, identities, contentChanges);
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
bool save(void* context, const ReaderPreferenceValues& candidate, ReaderPreferenceValues& rollback) {
  auto& owner = *static_cast<PreferenceRuntime*>(context);
  std::lock_guard<std::mutex> lock(owner.mutex);
  rollback = owner.baseline;
  return persistPreferences(owner, candidate);
}
}  // namespace

bool captureLocalFontReplacement(std::string_view family, std::string_view path) {
  RenderLock renderLock;
  std::lock_guard<std::mutex> lock(runtime.mutex);
  const auto& selected = runtime.baseline.sdFontFamilyName;
  const auto end = std::find(selected.begin(), selected.end(), '\0');
  if (family.empty() || family != std::string_view(selected.data(), end - selected.begin())) return true;
  if (runtime.fonts) {
    runtime.fonts->refreshIfDirty();
    const auto* installed = runtime.fonts->registry().findFamily(family);
    const auto* file =
        installed ? installed->findFile(installed->vector ? 0 : runtime.baseline.fontPointSize, 0) : nullptr;
    if (!file || std::string_view(file->path) != path) return true;
  }
  return persistPreferences(runtime, runtime.baseline, uint16_t{1});
}

bool captureLocalDictionaryReplacement() {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (!runtime.baseline.dictionaryName.front()) return true;
  return persistPreferences(runtime, runtime.baseline, uint16_t{0x400});
}

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
TintaJournalResult restoreReaderBookmarks(Epub& epub, std::vector<BookmarkEntry>& entries,
                                          ReaderBookmarkBinding& binding) {
  return runReaderBookmarks(epub, binding, entries, {BookmarkOperation::Restore});
}
TintaJournalResult decideReaderLegacyBookmarks(Epub& epub, std::vector<BookmarkEntry>& entries,
                                               ReaderBookmarkBinding& binding, LegacyBookmarkDecision decision) {
  if (decision != LegacyBookmarkDecision::Associate && decision != LegacyBookmarkDecision::LeaveUnassociated)
    return TintaJournalResult::Invalid;
  BookmarkRequest request{BookmarkOperation::DecideLegacy};
  request.legacyDecision = decision;
  return runReaderBookmarks(epub, binding, entries, request);
}
TintaJournalResult createReaderBookmark(Epub& epub, BookmarkEntry& candidate, std::vector<BookmarkEntry>& entries,
                                        ReaderBookmarkBinding& binding) {
  return runReaderBookmarks(epub, binding, entries, {BookmarkOperation::Create, &candidate});
}
TintaJournalResult renameReaderBookmark(Epub& epub, const BookmarkEntry& selected, std::string_view name,
                                        std::vector<BookmarkEntry>& entries, ReaderBookmarkBinding& binding) {
  return runReaderBookmarks(epub, binding, entries, {BookmarkOperation::Rename, nullptr, &selected, name});
}
TintaJournalResult deleteReaderBookmark(Epub& epub, const BookmarkEntry& selected, std::vector<BookmarkEntry>& entries,
                                        ReaderBookmarkBinding& binding) {
  return runReaderBookmarks(epub, binding, entries, {BookmarkOperation::Erase, nullptr, &selected});
}
TintaJournalResult deleteMatchingReaderBookmarks(Epub& epub, std::vector<BookmarkEntry>& entries,
                                                 ReaderBookmarkBinding& binding, ReaderBookmarkMatcher matches,
                                                 void* matchContext) {
  return runReaderBookmarks(epub, binding, entries,
                            {BookmarkOperation::EraseMatching, nullptr, nullptr, {}, matches, matchContext});
}
TintaJournalResult loadReaderBookmarkChoices(Epub& epub, NativeBookmarkChoicePage& choices,
                                             ReaderBookmarkBinding& binding, uint32_t offset) {
  choices.invalidate();
  std::vector<BookmarkEntry> unused;
  return runReaderBookmarks(epub, binding, unused,
                            {BookmarkOperation::LoadChoices, nullptr, nullptr, {}, nullptr, nullptr, &choices, offset});
}
TintaJournalResult resolveReaderBookmarkChoice(Epub& epub, NativeBookmarkChoicePage& choices, uint32_t selected,
                                               std::vector<BookmarkEntry>& entries, ReaderBookmarkBinding& binding) {
  const auto result = runReaderBookmarks(
      epub, binding, entries,
      {BookmarkOperation::ResolveChoice, nullptr, nullptr, {}, nullptr, nullptr, &choices, selected});
  choices.invalidate();
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
  if (runtime.suspended || runtime.failed || runtime.bookmarkRecoveryRequired) {
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
  runtime.bookmarkRecoveryRequired = session->requiresRecovery();
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
  if (runtime.suspended || runtime.failed || runtime.bookmarkRecoveryRequired) return TintaJournalResult::Unavailable;
  Digest edition{};
  if (!bookIdentity(epub, edition)) return TintaJournalResult::IoError;
  auto replay = makeUniqueNoThrow<NativeBookmarkReplay>();
  if (!replay) {
    LOG_ERR("COMPANION", "OOM: bookmark replay owner");
    return TintaJournalResult::IoError;
  }
  ++runtime.progressRevision;
  const auto result = replay->run(edition, bookmark, epub.getSpineItemsCount(), publish, context);
  if (result == TintaJournalResult::IoError) runtime.bookmarkRecoveryRequired = true;
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
