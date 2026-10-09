#pragma once

#include "CompanionCourseBaselinePublicationStore.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseRemovalMetadata.h"

namespace companion {
// Admit off stack. Caller excludes state/namespace writers and supplies native
// artifact verification and archive publication; durable phase bytes alone are
// not proof that a baseline is installed.
class HalCourseBaselinePublicationStore final {
 public:
  using Permission = bool (*)(void*);
  HalCourseBaselinePublicationStore(const Identity& reader, const Identity& generation, const Identity& owner,
                                    std::span<uint8_t> scratch, Permission permitted, void* context)
      : reader(reader),
        generation(generation),
        owner(owner),
        scratch(scratch),
        permitted(permitted),
        context(context),
        storage(allowed, this),
        publication(storage, scratch, allowed, this) {}
  ~HalCourseBaselinePublicationStore() { close(); }
  HalCourseBaselinePublicationStore(const HalCourseBaselinePublicationStore&) = delete;
  HalCourseBaselinePublicationStore& operator=(const HalCourseBaselinePublicationStore&) = delete;
  CourseBaselinePublicationResult publish(const CourseBaselinePublicationRecord& request,
                                          const CourseBaselinePublicationHooks& hooks) {
    if (operating) return CourseBaselinePublicationResult::Busy;
    ready = false;
    if (scratch.size() < COURSE_BASELINE_PUBLICATION_SIZE ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        !validCourseBaselinePublicationRecord(request) || request.phase != CourseBaselinePublicationPhase::Prepared)
      return refused(CourseBaselinePublicationResult::Invalid);
    if (request.reader != reader || request.request.generation != generation || request.request.owner != owner)
      return refused(CourseBaselinePublicationResult::Conflict);
    selected = request;
    storage.select(selected.request.transaction);
    operating = true;
    auto result = publication.publish(selected, hooks);
    if (!storage.closeReaders()) result = CourseBaselinePublicationResult::IoError;
    if (!guard()) result = CourseBaselinePublicationResult::Busy;
    operating = false;
    ready = result == CourseBaselinePublicationResult::Ok && publication.published();
    if (!ready) publication.close();
    return result == CourseBaselinePublicationResult::Ok ? result : refused(result);
  }
  const CourseBaselinePublicationRecord* published() const {
    if (!guard()) ready = false;
    return ready ? publication.published() : nullptr;
  }
  bool close() {
    ready = false;
    publication.close();
    return storage.closeReaders();
  }

 private:
  class PublicationStorage final : public TransferStorage {
   public:
    PublicationStorage(Permission permitted, void* context)
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
      if (!valid(path, true) || offset || !truncate || bytes.size() != COURSE_BASELINE_PUBLICATION_SIZE ||
          !std::equal(selected.begin(), selected.end(), bytes.begin() + 64) || bytes[5] != phase(path) ||
          !closeReaders() || !guard() || metadata.stat(path, size) != FileStatus::Missing || !guard() ||
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
      return (valid(from, true) && valid(to, false) && !staged(to) && phase(from) == phase(to) && closeReaders() &&
              guard() && metadata.stat(from, size) == FileStatus::Present && size == COURSE_BASELINE_PUBLICATION_SIZE &&
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
    static uint8_t phase(const char* path) {
      return std::string_view(path).ends_with(".prepared") || std::string_view(path).ends_with(".prepared.tmp")
                 ? static_cast<uint8_t>(CourseBaselinePublicationPhase::Prepared)
                 : static_cast<uint8_t>(CourseBaselinePublicationPhase::Published);
    }
    bool valid(const char* path, bool requireStage) const {
      static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-baseline-";
      static constexpr char HEX_DIGITS[] = "0123456789abcdef";
      if (!path || selected == Identity{}) return false;
      const auto size = strnlen(path, 96);
      const std::string_view view(path, size);
      if (size == 96 || !view.starts_with(PREFIX) || size < PREFIX.size() + 32) return false;
      for (size_t i = 0; i < selected.size(); ++i)
        if (view[PREFIX.size() + 2 * i] != HEX_DIGITS[selected[i] >> 4] ||
            view[PREFIX.size() + 2 * i + 1] != HEX_DIGITS[selected[i] & 15])
          return false;
      const auto suffix = view.substr(PREFIX.size() + 32);
      return suffix == ".prepared.tmp" || suffix == ".published.tmp" ||
             (!requireStage && (suffix == ".prepared" || suffix == ".published"));
    }
    static bool failure(const char* operation) {
      LOG_ERR("COMPANION", "Baseline publication storage %s failed", operation);
      return false;
    }
  };
  Identity reader, generation, owner;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  PublicationStorage storage;
  CourseBaselinePublicationStore publication;
  CourseBaselinePublicationRecord selected;
  bool operating = false;
  mutable bool ready = false;
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalCourseBaselinePublicationStore*>(context)->guard(); }
  static CourseBaselinePublicationResult refused(CourseBaselinePublicationResult result) {
    LOG_ERR("COMPANION", "Native baseline publication refused: %u", static_cast<unsigned>(result));
    return result;
  }
};
}  // namespace companion
