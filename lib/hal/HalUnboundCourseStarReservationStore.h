#pragma once

#include "CompanionIdentity.h"
#include "CompanionUnboundCourseStarReservationStore.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseRemovalMetadata.h"

namespace companion {
// Admit off stack with the shared session workspace and all state writers excluded.
// Verify proves exclusive epoch ownership and the exact frozen plan; identity inspection
// checks hardware/card context without allocating an epoch. Loading grants no authority.
class HalUnboundCourseStarReservationStore final {
 public:
  using Permission = bool (*)(void*);
  using Verify = UnboundCourseStarReservationStore::Verify;
  HalUnboundCourseStarReservationStore(IdentityStorage& identities, const Identity& reader, const Identity& generation,
                                       std::span<uint8_t> scratch, Permission permitted, void* context)
      : identities(identities),
        reader(reader),
        generation(generation),
        scratch(scratch),
        permitted(permitted),
        context(context),
        storage(reader, generation, allowed, this),
        store(storage, scratch, allowed, this) {}
  ~HalUnboundCourseStarReservationStore() { closeReaders(); }
  HalUnboundCourseStarReservationStore(const HalUnboundCourseStarReservationStore&) = delete;
  HalUnboundCourseStarReservationStore& operator=(const HalUnboundCourseStarReservationStore&) = delete;
  UnboundCourseIntentResult load(UnboundCourseStarReservation& output) {
    if (operating) return UnboundCourseIntentResult::Busy;
    if (!workspace(&output, sizeof(output)) || reader == Identity{} || generation == Identity{})
      return UnboundCourseIntentResult::Invalid;
    operating = true;
    cancelled = false;
    auto result = store.load(selected);
    if (result == UnboundCourseIntentResult::Ok && !native(selected)) result = UnboundCourseIntentResult::Conflict;
    result = finish(result);
    if (result == UnboundCourseIntentResult::Ok) output = selected;
    return result;
  }
  UnboundCourseIntentResult persist(const UnboundCourseStarReservation& intent, const Identity& owner, Verify verify,
                                    void* verificationContext) {
    if (operating) return UnboundCourseIntentResult::Busy;
    if (!verify || !workspace(&intent, sizeof(intent)) || !workspace(owner.data(), owner.size()) ||
        !validUnboundCourseStarReservation(intent) || owner == Identity{} ||
        intent.reviews.intent.request.original.owner != owner)
      return UnboundCourseIntentResult::Invalid;
    selected = intent;
    operating = true;
    cancelled = false;
    storage.selectOwner(owner);
    if (!guard() || !native(selected)) return finish(UnboundCourseIntentResult::Invalid);
    if (!storage.prepare()) return finish(UnboundCourseIntentResult::IoError);
    return finish(store.persist(selected, verify, verificationContext));
  }
  bool closeReaders() {
    if (operating) cancelled = true;
    return storage.closeReaders();
  }

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
      const auto phase = isStage(path);
      uint64_t size = 0;
      if (!phase || offset || !truncate || bytes.size() != UNBOUND_COURSE_STAR_RESERVATION_SIZE ||
          bytes[21] != static_cast<uint8_t>(UnboundCourseMigrationPhase::Prepared) || owner == Identity{} ||
          !std::equal(reader.begin(), reader.end(), bytes.begin() + 24) ||
          !std::equal(generation.begin(), generation.end(), bytes.begin() + 48) ||
          !std::equal(owner.begin(), owner.end(), bytes.begin() + 64) || !closeReaders() || !guard() ||
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
      const auto phase = isStage(from);
      uint64_t size = 0;
      return (phase && to && std::strcmp(to, UnboundCourseStarReservationStore::PATH) == 0 && closeReaders() &&
              guard() && metadata.stat(from, size) == FileStatus::Present &&
              size == UNBOUND_COURSE_STAR_RESERVATION_SIZE && guard() &&
              metadata.stat(to, size) == FileStatus::Missing && guard() && Storage.rename(from, to) && guard()) ||
             fail("rename");
    }
    bool remove(const char* path) override {
      uint64_t size = 0;
      return (isStage(path) && closeReaders() && guard() && metadata.stat(path, size) == FileStatus::Present &&
              size < UNBOUND_COURSE_STAR_RESERVATION_SIZE && closeReaders() && guard() && Storage.remove(path) &&
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
    static bool isStage(const char* path) {
      return path && std::strcmp(path, UnboundCourseStarReservationStore::STAGE) == 0;
    }
    static bool valid(const char* path) {
      return path && (isStage(path) || std::strcmp(path, UnboundCourseStarReservationStore::PATH) == 0);
    }
    static bool fail(const char* operation) {
      LOG_ERR("COMPANION", "Unbound star reservation storage %s failed", operation);
      return false;
    }
  };
  IdentityStorage& identities;
  Identity reader, generation;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* context;
  IntentStorage storage;
  UnboundCourseStarReservationStore store;
  UnboundCourseStarReservation selected;
  bool operating = false, cancelled = false;
  bool workspace(const void* value, size_t length) const {
    return scratch.size() > UNBOUND_COURSE_STAR_RESERVATION_SIZE &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) &&
           !course_baseline_detail::overlaps(scratch.data(), scratch.size(), value, length) &&
           !course_baseline_detail::overlaps(this, sizeof(*this), value, length);
  }
  [[gnu::noinline]] bool current() const {
    IdentityState state;
    return reader != Identity{} && generation != Identity{} &&
           inspectIdentity(identities, state) == IdentityInspectionResult::Ok && state.device == reader &&
           state.storageGeneration == generation;
  }
  bool guard() const {
    return !cancelled && permitted && permitted(context) && !cancelled && admitCompanionHeap() && current() &&
           !cancelled;
  }
  static bool allowed(void* context) { return static_cast<HalUnboundCourseStarReservationStore*>(context)->guard(); }
  [[gnu::noinline]] bool native(const UnboundCourseStarReservation& value) const {
    IdentityState state;
    return value.reviews.intent.reader == reader && value.reviews.intent.request.original.generation == generation &&
           value.epoch && inspectIdentity(identities, state) == IdentityInspectionResult::Ok &&
           state.device == reader && state.storageGeneration == generation && value.epoch <= state.eventEpoch;
  }
  UnboundCourseIntentResult finish(UnboundCourseIntentResult result) {
    if (!storage.closeReaders()) result = UnboundCourseIntentResult::IoError;
    if (result == UnboundCourseIntentResult::Ok && !native(selected)) result = UnboundCourseIntentResult::Conflict;
    if (!guard()) result = UnboundCourseIntentResult::Busy;
    operating = false;
    return result;
  }
};
}  // namespace companion
