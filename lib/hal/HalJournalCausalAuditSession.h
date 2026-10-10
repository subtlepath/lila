#pragma once

#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <optional>

#include "CompanionBookmarkIdentityCursor.h"
#include "CompanionBookmarkResolution.h"
#include "CompanionJournalCourseMembership.h"
#include "CompanionJournalExportPage.h"
#include "CompanionJournalFrontierStream.h"
#include "CompanionJournalKnowledgeHeads.h"
#include "CompanionJournalMigration.h"
#include "CompanionJournalTintaUndoValidation.h"
#include "CompanionPortablePreferenceResolution.h"
#include "CompanionReadingPositionResolution.h"
#include "CompanionTintaJournalPaths.h"
#include "CompanionTintaPreferenceResolution.h"
#include "CompanionTintaWriter.h"
#include "HalJournalIdentityIndexSink.h"
#include "HalJournalIdentitySortStorage.h"
#include "HalJournalReplayVisits.h"
#include "HalTintaJournalStorage.h"

namespace companion {
// Short-lived checked heap workspace, released before allocating the reader UI.
class HalJournalCausalAuditSession {
 public:
  explicit HalJournalCausalAuditSession(TintaJournalLocation location = TintaJournalLocation::Active)
      : storage(location) {
    mbedtls_sha256_init(&hash);
  }
  // Source/context outlive the audit. Cleanup is idempotent, including failure
  // paths; the caller reopens a borrowed source before subsequent read operations.
  HalJournalCausalAuditSession(TintaJournalStorage& source, bool (*close)(void*), void* context)
      : journal(source, journalScratch), borrowedSource(true), closeBorrowed(close), closeContext(context) {
    mbedtls_sha256_init(&hash);
  }
  ~HalJournalCausalAuditSession() {
    endPreferenceKnowledge();
    if (borrowedSource && !closeJournal()) failure("borrowed journal cleanup");
    mbedtls_sha256_free(&hash);
  }
  bool run(Digest* frontier = nullptr, const Identity* course = nullptr, TintaSubjectCatalog* catalog = nullptr) {
    if (preferenceKnowledge) return failure("audit while preference knowledge is retained");
    exportPages.reset();
    audited = false;
    if (borrowedSource && !closeBorrowed) return failure("borrowed journal cleanup unavailable");
    if ((course == nullptr) != (catalog == nullptr) || (course && !tinta_body_detail::nonzero(*course)))
      return failure("course membership arguments");
    const auto opened = journal.open();
    if (opened != TintaJournalResult::Ok) return failure("journal recovery");
    CommittedJournalIdentitySource source(journal);
    if (!sorter.build(source)) return failure("identity sorting");
    {
      // Retained handles exceed the local budget; construct in session-owned storage.
      sink.emplace(journal.count(), indexScratch);
      JournalIdentityIndexBuilder builder(*sink, indexScratch);
      if (!builder.build(sorter, journal.count())) return failure("index publication");
      sink.reset();
    }
    if (!runs.close() || !reader.open() || !index.open(journal.count())) return failure("index open");
    const auto result = audit.validate(journal, index);
    if (result != TintaJournalResult::Ok) return failure("causal closure");
    if (undos.validate(journal, index) != TintaJournalResult::Ok) return failure("undo targets");
    if (course && membership.validate(journal, *course, *catalog) != TintaJournalResult::Ok)
      return failure("course subject membership");
    if (frontier && (mbedtls_sha256_starts(&hash, 0) != 0 ||
                     streamJournalFrontier(journal, index, encoding) != TintaJournalResult::Ok ||
                     mbedtls_sha256_finish(&hash, computed.data()) != 0))
      return failure("frontier SHA-256");
    if (!reader.close()) return failure("index close");
    if (!closeJournal()) return failure("journal close");
    audited = true;
    if (frontier) *frontier = computed;
    return true;
  }

  bool copyTo(TintaJournal& destination) {
    if (!audited) return failure("copy before audit");
    audited = false;
    if (!reader.open() || !index.open(journal.count())) return failure("copy index open");
    // Reusable envelope/body storage exceeds the local budget, allocated only for migration.
    auto migration = makeUniqueNoThrow<JournalMigration>();
    if (!migration) return failure("OOM: migration workspace");
    const auto result = migration->copy(journal, destination, index);
    const bool closed = reader.close();
    if (result != TintaJournalResult::Ok || !closed || !closeJournal()) return failure("journal copy");
    return true;
  }
  using ReplayVisitor = bool (*)(void*, uint32_t, const SyncEvent&, std::span<const uint8_t>, bool);
  static constexpr size_t replayWorkspaceBytes() { return sizeof(ReplayWorkspace); }
  using KnowledgeHeadVisitor = bool (*)(void*, const EventIdentity&);
  using BookmarkIdentityVisitor = bool (*)(void*, const Identity&);
  // Explicit IDs in legacy caches must not be borrowed from another edition.
  // Caller excludes all journal writers through import/publication.
  TintaJournalResult checkBookmarkEdition(const Digest& edition, const Identity& bookmark) {
    if (!audited) {
      failure("bookmark edition proof before audit");
      return TintaJournalResult::Unavailable;
    }
    audited = false;
    if (!tinta_body_detail::nonzero(edition) || !tinta_body_detail::nonzero(bookmark)) {
      failure("bookmark edition proof arguments");
      return TintaJournalResult::Invalid;
    }
    const auto count = journal.count();
    auto result = TintaJournalResult::Ok;
    for (uint32_t record = 0; record < count; ++record) {
      result = journal.read(record);
      if (result != TintaJournalResult::Ok) break;
      const auto& event = journal.event();
      if (event.kind == EventKind::BookmarkPut || event.kind == EventKind::BookmarkDelete) {
        BookmarkBodyView value;
        if (!decodeBookmarkBody(journal.body(), value)) {
          failure("bookmark edition proof body");
          result = TintaJournalResult::Corrupt;
          break;
        }
        if (value.identity == bookmark && event.resource != edition) {
          result = TintaJournalResult::Conflict;
          break;
        }
      }
      if (journal.count() != count) {
        result = TintaJournalResult::Conflict;
        break;
      }
      vTaskDelay(1);
    }
    const bool indexClosed = reader.close();
    const bool journalClosed = closeJournal();
    if (!indexClosed || !journalClosed) {
      failure("bookmark edition proof close");
      return TintaJournalResult::IoError;
    }
    return result;
  }
  // Visitor stages borrowed IDs only; discard staged data unless the call is Ok.
  // Caller excludes journal writers and must not re-enter this audit session.
  TintaJournalResult bookmarkIdentities(const Digest& edition, void* context, BookmarkIdentityVisitor visitor) {
    if (!audited) {
      failure("bookmark enumeration before audit");
      return TintaJournalResult::Unavailable;
    }
    if (!visitor || !tinta_body_detail::nonzero(edition)) return TintaJournalResult::Invalid;
    audited = false;
    static_assert(sizeof(BookmarkIdentityCursor) <= 128);
    auto& cursor = bookmarkCursor;
    Identity identity{};
    auto result = cursor.begin(journal, edition) ? TintaJournalResult::Ok : cursor.error();
    while (result == TintaJournalResult::Ok) {
      const auto step = cursor.step(journal, identity);
      if (step == BookmarkCursorResult::End) break;
      if (step == BookmarkCursorResult::Error) {
        result = cursor.error();
        break;
      }
      if (step == BookmarkCursorResult::Found && !visitor(context, identity)) {
        result = TintaJournalResult::IoError;
        break;
      }
      vTaskDelay(1);
    }
    const bool indexClosed = reader.close();
    const bool journalClosed = closeJournal();
    if (!indexClosed || !journalClosed) {
      failure("bookmark enumeration close");
      return TintaJournalResult::IoError;
    }
    if (result != TintaJournalResult::Ok) failure("bookmark enumeration");
    return result;
  }
  // Caller excludes index/visit builders and every journal writer except its
  // append-only preference writer. The returned snapshot is borrowed until end.
  PreferenceKnowledgeHeads* beginPreferenceKnowledge(uint32_t expectedJournalCount) {
    if (!audited || preferenceKnowledge || journal.count() != expectedJournalCount) {
      failure("preference knowledge before audit or changed journal count");
      return nullptr;
    }
    audited = false;
    preferenceKnowledge = makeUniqueNoThrow<PreferenceKnowledgeWorkspace>(index);
    if (!preferenceKnowledge) {
      failure("OOM: preference knowledge workspace");
      return nullptr;
    }
    const bool valid = reader.open() && index.open(journal.count()) && preferenceKnowledge->begin(journal);
    const bool journalClosed = closeJournal();
    if (!valid || !journalClosed) {
      failure("preference knowledge preparation or journal close");
      endPreferenceKnowledge();
      return nullptr;
    }
    return preferenceKnowledge.get();
  }
  bool endPreferenceKnowledge() {
    if (!preferenceKnowledge) return true;
    const bool visitsClosed = preferenceKnowledge->close();
    const bool indexClosed = reader.close();
    preferenceKnowledge.reset();
    return visitsClosed && indexClosed ? true : failure("preference knowledge close");
  }
  // Borrowed identities must be consumed immediately; journal writers remain excluded.
  bool knowledgeHeads(void* context, KnowledgeHeadVisitor visitor) {
    if (!audited || !visitor) return failure("knowledge heads before audit or missing visitor");
    audited = false;
    auto workspace = makeUniqueNoThrow<KnowledgeWorkspace>();
    if (!workspace) return failure("OOM: knowledge head workspace");
    const auto finish = [&](bool success) {
      const bool visitsClosed = workspace->visits.close();
      const bool indexClosed = reader.close();
      const bool journalClosed = closeJournal();
      return success && visitsClosed && indexClosed && journalClosed;
    };
    if (!reader.open() || !index.open(journal.count()) ||
        workspace->heads.begin(journal, index, workspace->visits) != TintaJournalResult::Ok) {
      failure("knowledge head preparation");
      return finish(false);
    }
    for (;;) {
      bool complete = false;
      const auto result = workspace->heads.next(workspace->identity, complete);
      if (result != TintaJournalResult::Ok) {
        failure("knowledge head selection");
        return finish(false);
      }
      if (complete) return finish(true);
      if (!visitor(context, workspace->identity)) {
        failure("knowledge head visitor");
        return finish(false);
      }
    }
  }
  // The visitor consumes borrowed views immediately; caller excludes journal writers.
  bool replay(void* context, ReplayVisitor visitor, bool markUndoneReviews = false) {
    if (!audited || !visitor) return failure("replay before audit or missing visitor");
    audited = false;
    // Full envelope storage exceeds the stack budget; allocate once for this replay.
    auto workspace = makeUniqueNoThrow<ReplayWorkspace>();
    if (!workspace) return failure("OOM: replay workspace");
    const auto finish = [&](bool success) {
      const bool exclusionsClosed = workspace->exclusions.close();
      const bool visitsClosed = workspace->visits.close();
      const bool indexClosed = reader.close();
      const bool journalClosed = closeJournal();
      return success && exclusionsClosed && visitsClosed && indexClosed && journalClosed;
    };
    if (!reader.open() || !index.open(journal.count()) ||
        workspace->order.begin(journal, index, workspace->visits) != TintaJournalResult::Ok) {
      failure("replay preparation");
      return finish(false);
    }
    if (markUndoneReviews && undos.validate(journal, index, &workspace->exclusions) != TintaJournalResult::Ok) {
      failure("replay undo exclusions");
      return finish(false);
    }
    for (;;) {
      uint32_t record = 0;
      bool complete = false;
      const auto result = workspace->order.next(record, complete);
      if (result != TintaJournalResult::Ok) {
        failure("replay selection");
        return finish(false);
      }
      if (complete) return finish(true);
      bool excluded = false;
      if (markUndoneReviews && !workspace->exclusions.visited(record, excluded)) {
        failure("replay exclusions read");
        return finish(false);
      }
      if (journal.read(record) != TintaJournalResult::Ok) {
        failure("replay record read");
        return finish(false);
      }
      if (markUndoneReviews && journal.event().kind == EventKind::UndoReview) continue;
      if (!visitor(context, record, journal.event(), journal.body(), excluded)) {
        failure("replay visitor or record");
        return finish(false);
      }
    }
  }
  bool containsTintaCourse(const Identity& course, bool& output) {
    if (!audited || !tinta_body_detail::nonzero(course)) return failure("course lookup before audit");
    audited = false;
    const auto count = journal.count();
    bool found = false;
    bool valid = true;
    for (uint32_t at = 0; at < count; ++at) {
      if (journal.read(at) != TintaJournalResult::Ok) {
        valid = false;
        break;
      }
      const auto kind = journal.event().kind;
      if (kind < EventKind::Review || kind > EventKind::ReadingComplete) continue;
      TintaBody body;
      if (!decodeTintaBody(journal.body(), body)) {
        valid = false;
        break;
      }
      if (body.course == course) {
        found = true;
        break;
      }
    }
    const bool closed = closeJournal();
    if (!valid || !closed || journal.count() != count) return failure("course history lookup");
    output = found;
    return true;
  }
  // Resolution views remain in the caller's retained owner after checked closes.
  TintaJournalResult resolveTintaPreferences(TintaPreferenceResolution& resolution) {
    resolution.clear();
    if (!audited) {
      failure("preference resolution before audit");
      return TintaJournalResult::Unavailable;
    }
    audited = false;
    auto visits = makeUniqueNoThrow<HalJournalReplayVisits>();
    if (!visits) {
      failure("OOM: preference visit storage owner");
      return TintaJournalResult::IoError;
    }
    const auto result = reader.open() && index.open(journal.count()) ? resolution.run(journal, index, *visits)
                                                                     : TintaJournalResult::IoError;
    const bool visitsClosed = visits->close();
    const bool indexClosed = reader.close();
    const bool journalClosed = closeJournal();
    if (!visitsClosed || !indexClosed || !journalClosed ||
        (result != TintaJournalResult::Ok && result != TintaJournalResult::Conflict)) {
      resolution.clear();
      failure("preference resolution or close");
      return !visitsClosed || !indexClosed || !journalClosed ? TintaJournalResult::IoError : result;
    }
    return result;
  }
  // Resolution views remain in the caller's retained owner after checked closes.
  TintaJournalResult resolvePortablePreferences(PortablePreferenceResolution& resolution) {
    resolution.clear();
    if (!audited) {
      failure("preference resolution before audit");
      return TintaJournalResult::Unavailable;
    }
    audited = false;
    auto visits = makeUniqueNoThrow<HalJournalReplayVisits>();
    if (!visits) {
      failure("OOM: preference visit storage owner");
      return TintaJournalResult::IoError;
    }
    const auto result = reader.open() && index.open(journal.count()) ? resolution.run(journal, index, *visits)
                                                                     : TintaJournalResult::IoError;
    const bool visitsClosed = visits->close();
    const bool indexClosed = reader.close();
    const bool journalClosed = closeJournal();
    if (!visitsClosed || !indexClosed || !journalClosed ||
        (result != TintaJournalResult::Ok && result != TintaJournalResult::Conflict)) {
      resolution.clear();
      failure("preference resolution or close");
      return !visitsClosed || !indexClosed || !journalClosed ? TintaJournalResult::IoError : result;
    }
    return result;
  }
  TintaJournalResult resolveReadingPosition(const Digest& edition, ReadingAnchor& output) {
    if (!audited) {
      failure("reading resolution before audit");
      return TintaJournalResult::Unavailable;
    }
    audited = false;
    // Event/validation and visit handles exceed the local stack budget.
    auto resolution = makeUniqueNoThrow<ReadingPositionResolution>();
    auto visits = makeUniqueNoThrow<HalJournalReplayVisits>();
    if (!resolution || !visits) {
      failure("OOM: reading resolution owners");
      return TintaJournalResult::IoError;
    }
    ReadingAnchor candidate;
    const auto result = reader.open() && index.open(journal.count())
                            ? resolution->run(journal, index, *visits, edition, candidate)
                            : TintaJournalResult::IoError;
    const bool visitsClosed = visits->close();
    const bool indexClosed = reader.close();
    const bool journalClosed = closeJournal();
    if (!visitsClosed || !indexClosed || !journalClosed) {
      failure("reading resolution close");
      return TintaJournalResult::IoError;
    }
    if (result == TintaJournalResult::Ok)
      output = candidate;
    else if (result != TintaJournalResult::Conflict && result != TintaJournalResult::Unavailable)
      failure("reading resolution");
    return result;
  }
  TintaJournalResult resolveBookmark(const Digest& edition, const Identity& bookmark, std::span<uint8_t> output,
                                     size_t& length) {
    if (!audited) {
      failure("bookmark resolution before audit");
      return TintaJournalResult::Unavailable;
    }
    audited = false;
    if (output.size() < MAX_BOOKMARK_BODY_SIZE) return TintaJournalResult::Invalid;
    auto workspace = makeUniqueNoThrow<BookmarkResolutionWorkspace>();
    if (!workspace) {
      failure("OOM: bookmark resolution workspace");
      return TintaJournalResult::IoError;
    }
    size_t candidateLength = 0;
    const auto result = reader.open() && index.open(journal.count())
                            ? workspace->resolution.run(journal, index, workspace->visits, edition, bookmark,
                                                        workspace->body, candidateLength)
                            : TintaJournalResult::IoError;
    const bool visitsClosed = workspace->visits.close();
    const bool indexClosed = reader.close();
    const bool journalClosed = closeJournal();
    if (!visitsClosed || !indexClosed || !journalClosed) {
      failure("bookmark resolution close");
      return TintaJournalResult::IoError;
    }
    if (result == TintaJournalResult::Ok) {
      std::copy_n(workspace->body.begin(), candidateLength, output.begin());
      length = candidateLength;
    } else if (result != TintaJournalResult::Conflict && result != TintaJournalResult::Unavailable) {
      failure("bookmark resolution");
    }
    return result;
  }
  TintaJournalResult visitBookmarkHeads(const Digest& edition, const Identity& bookmark,
                                        BookmarkResolution::Visitor visitor, void* context) {
    if (!audited) {
      failure("bookmark choices before audit");
      return TintaJournalResult::Unavailable;
    }
    audited = false;
    if (!visitor) return TintaJournalResult::Invalid;
    // Visit marks and canonical-body storage exceed the local stack budget.
    auto workspace = makeUniqueNoThrow<BookmarkResolutionWorkspace>();
    if (!workspace) {
      failure("OOM: bookmark choices workspace");
      return TintaJournalResult::IoError;
    }
    const auto result =
        reader.open() && index.open(journal.count())
            ? workspace->resolution.visitHeads(journal, index, workspace->visits, edition, bookmark, visitor, context)
            : TintaJournalResult::IoError;
    const bool visitsClosed = workspace->visits.close();
    const bool indexClosed = reader.close();
    const bool journalClosed = closeJournal();
    if (!visitsClosed || !indexClosed || !journalClosed) {
      failure("bookmark choices close");
      return TintaJournalResult::IoError;
    }
    if (result != TintaJournalResult::Ok && result != TintaJournalResult::Conflict &&
        result != TintaJournalResult::Unavailable)
      failure("bookmark choices enumeration");
    return result;
  }
  // Caller freezes the audited journal/index and supplies a durable baseline count.
  bool prefixFrontier(uint32_t count, Digest& output) {
    if (preferenceKnowledge) return failure("prefix frontier while preference knowledge is retained");
    const bool valid = audited && reader.open() && index.open(journal.count()) &&
                       mbedtls_sha256_starts(&hash, 0) == 0 &&
                       streamJournalPrefixFrontier(journal, index, count, encoding) == TintaJournalResult::Ok &&
                       mbedtls_sha256_finish(&hash, computed.data()) == 0;
    const bool indexClosed = reader.close();
    const bool journalClosed = closeJournal();
    if (!valid || !indexClosed || !journalClosed) return failure("prefix frontier");
    output = computed;
    return true;
  }

  uint32_t recordCount() const { return journal.count(); }
  uint16_t recordSize() const { return journal.recordSize(); }
  bool beginExport() {
    if (!run(&computed)) return false;
    audited = false;
    return exportPages.begin(computed);
  }
  size_t exportPage(std::span<const uint8_t> request, std::span<uint8_t> output) {
    return exportPages.page(request, output);
  }
  bool endExport() {
    if (preferenceKnowledge) return failure("export end while preference knowledge is retained");
    exportPages.reset();
    audited = false;
    return closeJournal();
  }

 private:
  class PreferenceKnowledgeWorkspace final : public PreferenceKnowledgeHeads {
   public:
    explicit PreferenceKnowledgeWorkspace(IndexedJournalIdentities& index) : index(index) {}
    bool begin(TintaJournal& journal) {
      ready = false;
      size = cursor = emitted = 0;
      if (heads.begin(journal, index, visits) != TintaJournalResult::Ok) return false;
      for (uint32_t at = 0; at < index.recordCount(); ++at) {
        bool hasChild = false;
        if (!index.read(at, entry) || !visits.visited(entry.record, hasChild)) return false;
        if (!hasChild) ++size;
      }
      ready = true;
      return true;
    }
    uint32_t count() const override { return ready ? size : 0; }
    bool read(uint32_t at, EventIdentity& output) override {
      if (!ready || at != emitted || at >= size) return fail();
      while (cursor < index.recordCount()) {
        bool hasChild = false;
        if (!index.read(cursor++, entry) || !visits.visited(entry.record, hasChild)) return fail();
        if (hasChild) continue;
        output = entry.identity;
        ++emitted;
        return true;
      }
      return fail();
    }
    bool close() {
      ready = false;
      return visits.close();
    }

   private:
    bool fail() {
      ready = false;
      LOG_ERR("COMPANION", "Preference knowledge snapshot read failed");
      return false;
    }
    IndexedJournalIdentities& index;
    JournalKnowledgeHeads heads;
    HalJournalReplayVisits visits;
    JournalIdentityEntry entry{};
    uint32_t size = 0, cursor = 0, emitted = 0;
    bool ready = false;
  };
  struct KnowledgeWorkspace {
    JournalKnowledgeHeads heads;
    HalJournalReplayVisits visits;
    EventIdentity identity{};
  };
  struct BookmarkResolutionWorkspace {
    BookmarkResolution resolution;
    HalJournalReplayVisits visits;
    std::array<uint8_t, MAX_BOOKMARK_BODY_SIZE> body{};
  };
  struct ReplayWorkspace {
    JournalReplayOrder order;
    HalJournalReplayVisits visits;
    HalJournalReplayVisits exclusions{true};
  };
  static bool hashBytes(void* context, std::span<const uint8_t> bytes) {
    auto& session = *static_cast<HalJournalCausalAuditSession*>(context);
    return mbedtls_sha256_update(&session.hash, bytes.data(), bytes.size()) == 0;
  }
  static bool failure(const char* stage) {
    LOG_ERR("COMPANION", "Journal causal audit failed: %s", stage);
    return false;
  }
  std::array<uint8_t, TintaJournal::EXTENDED_RECORD_SIZE> journalScratch{};
  BookmarkIdentityCursor bookmarkCursor;
  std::array<uint8_t, 512> sortScratch{};
  std::array<uint8_t, JOURNAL_IDENTITY_ENTRY_SIZE> indexScratch{};
  HalTintaJournalStorage storage;
  TintaJournal journal{storage, journalScratch};
  JournalExportPage exportPages{journal};
  HalJournalIdentitySortStorage runs;
  JournalIdentitySorter sorter{runs, sortScratch};
  std::optional<HalJournalIdentityIndexSink> sink;
  HalJournalIdentityIndexStorage reader;
  IndexedJournalIdentities index{reader, indexScratch};
  JournalCausalValidation audit;
  JournalTintaUndoValidation undos;
  JournalCourseMembershipValidation membership;
  mbedtls_sha256_context hash{};
  Digest computed{};
  bool audited = false;
  bool borrowedSource = false;
  bool (*closeBorrowed)(void*) = nullptr;
  void* closeContext = nullptr;
  bool closeJournal() { return borrowedSource ? closeBorrowed && closeBorrowed(closeContext) : storage.close(); }
  std::unique_ptr<PreferenceKnowledgeWorkspace> preferenceKnowledge;
  TintaJournalFrontierEncoding encoding{sortScratch, this, hashBytes};
};
// The caller owns the lookup outside the task stack and excludes journal writers.
inline bool auditExistingCompanionJournal(HalCompanionFileLookup& lookup) {
  if (!Storage.ensureDirectoryExists(TRANSFER_DIRECTORY)) {
    LOG_ERR("COMPANION", "Cannot prepare journal audit lookup");
    return false;
  }
  const auto presence = lookup.inspect(TINTA_JOURNAL_DIRECTORY);
  if (presence == CompanionFilePresence::Missing) return true;
  if (presence == CompanionFilePresence::Error) return false;
  auto session = makeUniqueNoThrow<HalJournalCausalAuditSession>();
  if (!session) {
    LOG_ERR("COMPANION", "OOM: journal causal audit workspace");
    return false;
  }
  return session->run();
}
}  // namespace companion
