#pragma once

#include "CompanionCourseBaselineImportConsent.h"
#include "HalCourseBaselineImportPreparation.h"

namespace companion {
// Admit off stack. Native/authenticated identities and writer exclusion outlive
// approval. Loading durable consent alone never offers an approved-state loan.
class HalCourseBaselineImportConsentStore final {
 public:
  using Permission = bool (*)(void*);
  HalCourseBaselineImportConsentStore(const Identity& reader, const Identity& generation, const Identity& owner,
                                      std::span<uint8_t> scratch, Permission permitted, void* context)
      : reader(reader),
        generation(generation),
        owner(owner),
        scratch(scratch),
        permitted(permitted),
        context(context),
        storage(allowed, this),
        consent(storage, scratch, allowed, this),
        preparation(reader, generation, owner, scratch, allowed, this) {}
  ~HalCourseBaselineImportConsentStore() { close(); }
  HalCourseBaselineImportConsentStore(const HalCourseBaselineImportConsentStore&) = delete;
  HalCourseBaselineImportConsentStore& operator=(const HalCourseBaselineImportConsentStore&) = delete;
  CourseBaselineConsentResult approve(const CourseBaselineImportRequest& request,
                                      const TransferDeclaration& declaration) {
    if (operating) return CourseBaselineConsentResult::Busy;
    ready = false;
    if (!workspace() || !validCourseBaselineImportRequest(request))
      return refused(CourseBaselineConsentResult::Invalid);
    selected = request;
    storage.select(selected.transaction);
    operating = true;
    outcome = CourseBaselineConsentResult::Conflict;
    const bool prepared = preparation.prepare(selected, declaration, persist, this);
    return finish(prepared                                     ? CourseBaselineConsentResult::Ok
                  : outcome == CourseBaselineConsentResult::Ok ? CourseBaselineConsentResult::Busy
                                                               : outcome,
                  true);
  }
  CourseBaselineConsentResult load(const Identity& transaction, CourseBaselineImportRequest& output) {
    if (operating) return CourseBaselineConsentResult::Busy;
    ready = false;
    if (!workspace() || course_baseline_detail::overlaps(&output, sizeof(output), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(&output, sizeof(output), scratch.data(), scratch.size()))
      return refused(CourseBaselineConsentResult::Invalid);
    storage.select(transaction);
    operating = true;
    auto result = consent.load(transaction, selected);
    if (result == CourseBaselineConsentResult::Ok && (selected.generation != generation || selected.owner != owner))
      result = CourseBaselineConsentResult::Conflict;
    if (result == CourseBaselineConsentResult::Ok) result = review();
    result = finish(result, false);
    if (result == CourseBaselineConsentResult::Ok) output = selected;
    return result;
  }
  const CourseBaselineImportRequest* approved() const {
    if (!guard()) ready = false;
    return ready ? &selected : nullptr;
  }
  bool close() {
    ready = false;
    preparation.close();
    return storage.closeReaders();
  }

 private:
  class ConsentStorage final : public TransferStorage {
   public:
    ConsentStorage(Permission permitted, void* context)
        : permitted(permitted), context(context), metadata(permitted, context) {}
    void select(const Identity& transaction) { selected = transaction; }
    bool prepare() override { return guard() && metadata.prepare(); }
    FileStatus stat(const char* path, uint64_t& size) override {
      return valid(path, false) && guard() ? metadata.stat(path, size) : FileStatus::Error;
    }
    bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
      return (valid(path, false) && guard() && metadata.read(path, offset, bytes) && guard()) || failure("read");
    }
    bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override {
      uint64_t size = 0;
      if (!valid(path, true) || offset || !truncate || bytes.size() != COURSE_BASELINE_IMPORT_REQUEST_SIZE ||
          !std::equal(selected.begin(), selected.end(), bytes.begin() + 40) || !closeReaders() || !guard() ||
          metadata.stat(path, size) != FileStatus::Missing || !guard() ||
          !Storage.openFileForWriteReusing("COMPANION", path, writer))
        return failure("write admission");
      const bool written = guard() && !writer.isDirectory() &&
                           writer.write(bytes.data(), bytes.size()) == bytes.size() && writer.truncate(bytes.size()) &&
                           writer.sync();
      const bool closed = writer.close();
      return (written && closed && guard()) || failure("write/sync/close");
    }
    bool rename(const char* from, const char* to) override {
      uint64_t size = 0;
      return (valid(from, true) && valid(to, false) && !staged(to) && closeReaders() && guard() &&
              metadata.stat(from, size) == FileStatus::Present && size == COURSE_BASELINE_IMPORT_REQUEST_SIZE &&
              guard() && metadata.stat(to, size) == FileStatus::Missing && guard() && Storage.rename(from, to) &&
              guard()) ||
             failure("rename");
    }
    bool resize(const char*, uint64_t) override { return failure("resize refused"); }
    bool remove(const char*) override { return failure("remove refused"); }
    bool verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) override { return failure("verify refused"); }
    bool closeReaders() {
      const bool writerClosed = !writer.isOpen() || writer.close(), metadataClosed = metadata.closeReaders();
      return writerClosed && metadataClosed;
    }

   private:
    Permission permitted;
    void* context;
    HalCourseRemovalMetadata metadata;
    HalFile writer;
    Identity selected{};
    bool guard() const { return permitted && permitted(context); }
    static bool staged(const char* path) { return std::string_view(path).ends_with(".tmp"); }
    bool valid(const char* path, bool requireStage) const {
      static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-baseline-";
      static constexpr char HEX_DIGITS[] = "0123456789abcdef";
      if (!path || selected == Identity{}) return false;
      const auto size = strnlen(path, 96);
      const std::string_view view(path, size);
      if (size == 96 || !view.starts_with(PREFIX) ||
          (size != PREFIX.size() + 32 + 8 && size != PREFIX.size() + 32 + 12))
        return false;
      for (size_t i = 0; i < selected.size(); ++i)
        if (view[PREFIX.size() + 2 * i] != HEX_DIGITS[selected[i] >> 4] ||
            view[PREFIX.size() + 2 * i + 1] != HEX_DIGITS[selected[i] & 15])
          return false;
      const auto suffix = view.substr(PREFIX.size() + 32);
      return suffix == ".consent.tmp" || (!requireStage && suffix == ".consent");
    }
    static bool failure(const char* operation) {
      LOG_ERR("COMPANION", "Baseline consent storage %s failed", operation);
      return false;
    }
  };
  Identity reader, generation, owner;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  ConsentStorage storage;
  CourseBaselineImportConsent consent;
  HalCourseBaselineImportPreparation preparation;
  CourseBaselineImportRequest selected;
  CourseBaselineConsentResult outcome = CourseBaselineConsentResult::Conflict;
  bool operating = false;
  mutable bool ready = false;
  bool workspace() const {
    return scratch.size() >= COURSE_BASELINE_REVIEW_MAX_SIZE + 512 &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this));
  }
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalCourseBaselineImportConsentStore*>(context)->guard(); }
  static bool persist(void* context, const CourseBaselineImportRequest& request) {
    auto& store = *static_cast<HalCourseBaselineImportConsentStore*>(context);
    store.outcome = store.consent.persist(request);
    if (!store.storage.closeReaders()) store.outcome = CourseBaselineConsentResult::IoError;
    return store.outcome == CourseBaselineConsentResult::Ok && store.guard();
  }
  CourseBaselineConsentResult review() {
    if (!guard() || !admitCompanionHeap(sizeof(HalCourseBaselineReviewStore), sizeof(HalCourseBaselineReviewStore)))
      return CourseBaselineConsentResult::Busy;
    auto review = makeUniqueNoThrow<HalCourseBaselineReviewStore>(scratch.subspan(COURSE_BASELINE_REVIEW_MAX_SIZE),
                                                                  allowed, this);
    if (!review) {
      LOG_ERR("COMPANION", "OOM: native consent review reader");
      return CourseBaselineConsentResult::IoError;
    }
    const auto result = review->open(selected.reviewHash, reader, generation, selected.manifest.logicalIdentity,
                                     scratch.first(COURSE_BASELINE_REVIEW_MAX_SIZE));
    const bool closed = review->closeReaders();
    return !closed || result == CourseBaselineReviewStoreResult::IoError ? CourseBaselineConsentResult::IoError
           : result == CourseBaselineReviewStoreResult::Busy             ? CourseBaselineConsentResult::Busy
           : result == CourseBaselineReviewStoreResult::Ok               ? CourseBaselineConsentResult::Ok
                                                                         : CourseBaselineConsentResult::Corrupt;
  }
  CourseBaselineConsentResult finish(CourseBaselineConsentResult result, bool approval) {
    if (!close()) result = CourseBaselineConsentResult::IoError;
    if (!guard()) result = CourseBaselineConsentResult::Busy;
    operating = false;
    ready = approval && result == CourseBaselineConsentResult::Ok;
    if (result != CourseBaselineConsentResult::Ok && result != CourseBaselineConsentResult::Missing)
      return refused(result);
    return result;
  }
  static CourseBaselineConsentResult refused(CourseBaselineConsentResult result) {
    LOG_ERR("COMPANION", "Native baseline consent refused: %u", static_cast<unsigned>(result));
    return result;
  }
};
}  // namespace companion
