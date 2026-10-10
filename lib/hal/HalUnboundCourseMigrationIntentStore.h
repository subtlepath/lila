#pragma once

#include "CompanionUnboundCourseMigrationIntentStore.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseRemovalMetadata.h"

namespace companion {
// Admit off stack with the shared session workspace and all state writers excluded.
// Persistence records native verification; loading never grants mutation authority.
class HalUnboundCourseMigrationIntentStore final {
 public:
  using Permission = bool (*)(void*);
  using VerifyPhase = UnboundCourseMigrationIntentStore::VerifyPhase;
  HalUnboundCourseMigrationIntentStore(const Identity& reader, const Identity& generation, std::span<uint8_t> scratch,
                                       Permission permitted, void* context)
      : reader(reader),
        generation(generation),
        scratch(scratch),
        permitted(permitted),
        context(context),
        storage(reader, generation, allowed, this),
        store(storage, scratch, allowed, this) {}
  ~HalUnboundCourseMigrationIntentStore() { closeReaders(); }
  UnboundCourseIntentResult load(UnboundCourseMigrationIntent& output) {
    if (operating) return UnboundCourseIntentResult::Busy;
    if (!workspace(&output, sizeof(output)) || reader == Identity{} || generation == Identity{})
      return UnboundCourseIntentResult::Invalid;
    operating = true;
    auto result = store.load(selected);
    if (result == UnboundCourseIntentResult::Ok && !native(selected)) result = UnboundCourseIntentResult::Conflict;
    result = finish(result);
    if (result == UnboundCourseIntentResult::Ok) output = selected;
    return result;
  }
  UnboundCourseIntentResult persist(const UnboundCourseMigrationIntent& intent, const Identity& owner,
                                    VerifyPhase verifyTorn = nullptr, void* verificationContext = nullptr) {
    if (operating) return UnboundCourseIntentResult::Busy;
    if (!workspace(&intent, sizeof(intent)) || !workspace(owner.data(), owner.size()) ||
        !validUnboundCourseMigrationIntent(intent) || !native(intent) || owner == Identity{} ||
        intent.request.original.owner != owner)
      return UnboundCourseIntentResult::Invalid;
    selected = intent;
    operating = true;
    storage.selectOwner(owner);
    if (!guard() || !storage.prepare()) return finish(UnboundCourseIntentResult::IoError);
    return finish(store.persist(selected, verifyTorn, verificationContext));
  }
  bool closeReaders() { return storage.closeReaders(); }

 private:
  class IntentStorage final : public TransferStorage {
   public:
    IntentStorage(const Identity& reader, const Identity& generation, Permission permitted, void* context)
        : reader(reader),
          generation(generation),
          permitted(permitted),
          context(context),
          metadata(permitted, context) {}
    void selectOwner(const Identity& value) { owner = value; }
    bool prepare() override {
      return (guard() && Storage.ready() && Storage.ensureDirectoryExists(TRANSFER_DIRECTORY) && guard()) ||
             fail("prepare");
    }
    FileStatus stat(const char* path, uint64_t& size) override {
      return valid(path) && guard() ? metadata.stat(path, size) : FileStatus::Error;
    }
    bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
      return (valid(path) && guard() && metadata.read(path, offset, bytes) && guard()) || fail("read");
    }
    bool write(const char* path, uint64_t offset, std::span<const uint8_t> bytes, bool truncate) override {
      const auto phase = stagePhase(path);
      uint64_t size = 0;
      if (!phase || offset || !truncate || bytes.size() != UNBOUND_COURSE_MIGRATION_INTENT_SIZE || bytes[5] != phase ||
          owner == Identity{} || !std::equal(reader.begin(), reader.end(), bytes.begin() + 8) ||
          !std::equal(generation.begin(), generation.end(), bytes.begin() + 32) ||
          !std::equal(owner.begin(), owner.end(), bytes.begin() + 48) || !closeReaders() || !guard() ||
          metadata.stat(path, size) != FileStatus::Missing || !guard() ||
          !Storage.openFileForWriteReusing("COMPANION", path, writer))
        return fail("write admission");
      const bool written = guard() && !writer.isDirectory() &&
                           writer.write(bytes.data(), bytes.size()) == bytes.size() && writer.truncate(bytes.size()) &&
                           writer.sync();
      const bool closed = writer.close();
      return (written && closed && guard()) || fail("write/sync/close");
    }
    bool rename(const char* from, const char* to) override {
      const auto phase = stagePhase(from);
      uint64_t size = 0;
      return (phase && to && std::strcmp(to, UnboundCourseMigrationIntentStore::PATHS[phase - 1]) == 0 &&
              closeReaders() && guard() && metadata.stat(from, size) == FileStatus::Present &&
              size == UNBOUND_COURSE_MIGRATION_INTENT_SIZE && guard() &&
              metadata.stat(to, size) == FileStatus::Missing && guard() && Storage.rename(from, to) && guard()) ||
             fail("rename");
    }
    bool remove(const char* path) override {
      uint64_t size = 0;
      return (stagePhase(path) && closeReaders() && guard() && metadata.stat(path, size) == FileStatus::Present &&
              size < UNBOUND_COURSE_MIGRATION_INTENT_SIZE && closeReaders() && guard() && Storage.remove(path) &&
              guard()) ||
             fail("torn stage removal");
    }
    bool resize(const char*, uint64_t) override { return fail("resize refused"); }
    bool verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) override { return fail("verify refused"); }
    bool closeReaders() {
      const bool writerClosed = !writer.isOpen() || writer.close();
      const bool metadataClosed = metadata.closeReaders();
      return writerClosed && metadataClosed;
    }

   private:
    Identity reader, generation, owner{};
    Permission permitted;
    void* context;
    HalCourseRemovalMetadata metadata;
    HalFile writer;
    bool guard() const { return permitted && permitted(context); }
    static unsigned stagePhase(const char* path) {
      if (path)
        for (unsigned i = 0; i < 3; ++i)
          if (std::strcmp(path, UnboundCourseMigrationIntentStore::STAGES[i]) == 0) return i + 1;
      return 0;
    }
    static bool valid(const char* path) {
      if (!path) return false;
      for (unsigned i = 0; i < 3; ++i)
        if (std::strcmp(path, UnboundCourseMigrationIntentStore::PATHS[i]) == 0 ||
            std::strcmp(path, UnboundCourseMigrationIntentStore::STAGES[i]) == 0)
          return true;
      return false;
    }
    static bool fail(const char* operation) {
      LOG_ERR("COMPANION", "Unbound intent storage %s failed", operation);
      return false;
    }
  };
  Identity reader, generation;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  IntentStorage storage;
  UnboundCourseMigrationIntentStore store;
  UnboundCourseMigrationIntent selected;
  bool operating = false;
  bool workspace(const void* value, size_t length) const {
    return scratch.size() >= UNBOUND_COURSE_MIGRATION_INTENT_SIZE &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), value, length) &&
           !course_baseline_detail::overlaps(this, sizeof(*this), value, length);
  }
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseMigrationIntentStore*>(context)->guard(); }
  bool native(const UnboundCourseMigrationIntent& intent) const {
    return reader != Identity{} && generation != Identity{} && intent.reader == reader &&
           intent.request.original.generation == generation;
  }
  UnboundCourseIntentResult finish(UnboundCourseIntentResult result) {
    if (!closeReaders()) result = UnboundCourseIntentResult::IoError;
    if (!guard()) result = UnboundCourseIntentResult::Busy;
    operating = false;
    return result;
  }
};
}  // namespace companion
