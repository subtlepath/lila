#pragma once

#include "CompanionContentRemovalRequest.h"
#include "CompanionInventoryIndex.h"
#include "CompanionTransfer.h"

namespace companion {
enum class ContentRemovalPhase : uint8_t { Prepared, Quarantined, Committed, Retired };
enum class ContentRemovalJournalResult { Ok, Missing, Invalid, Conflict, Corrupt, IoError };
inline constexpr size_t CONTENT_REMOVAL_RECORD_SIZE = 168;
inline constexpr const char* CONTENT_REMOVAL_JOURNALS[] = {"/.crosspoint/companion/removal-a",
                                                           "/.crosspoint/companion/removal-b"};
struct ContentRemovalRecord {
  ContentRemovalRequest request;
  Digest planHash{};
  ContentRemovalPhase phase = ContentRemovalPhase::Prepared;
  uint64_t revision = 1;
  bool operator==(const ContentRemovalRecord&) const = default;
};
static_assert(sizeof(ContentRemovalRecord) < 256);
inline bool validContentRemovalRecord(const ContentRemovalRecord& record) {
  const auto phase = static_cast<uint8_t>(record.phase);
  return validContentRemovalRequest(record.request) && phase <= 3 && record.revision == uint64_t{phase} + 1 &&
         std::any_of(record.planHash.begin(), record.planHash.end(), [](uint8_t byte) { return byte != 0; });
}
inline size_t encodeContentRemovalRecord(const ContentRemovalRecord& record, std::span<uint8_t> output) {
  if (output.size() < CONTENT_REMOVAL_RECORD_SIZE || !validContentRemovalRecord(record)) return 0;
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'J', 'N', 1, 0, 0, 0};
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  if (!encodeContentRemovalRequest(record.request, output.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE))) return 0;
  std::copy(record.planHash.begin(), record.planHash.end(), output.begin() + 123);
  output[155] = static_cast<uint8_t>(record.phase);
  inventory_detail::write(output, 156, record.revision, 8);
  inventory_detail::write(output, 164, inventoryIndexCrc(output.first(164)), 4);
  return CONTENT_REMOVAL_RECORD_SIZE;
}
inline bool decodeContentRemovalRecord(std::span<const uint8_t> bytes, ContentRemovalRecord& output) {
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'J', 'N', 1, 0, 0, 0};
  if (bytes.size() != CONTENT_REMOVAL_RECORD_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
      inventory_detail::read(bytes, 164, 4) != inventoryIndexCrc(bytes.first(164)))
    return false;
  ContentRemovalRecord parsed;
  if (!decodeContentRemovalRequest(bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE), parsed.request)) return false;
  std::copy_n(bytes.begin() + 123, 32, parsed.planHash.begin());
  parsed.phase = static_cast<ContentRemovalPhase>(bytes[155]);
  parsed.revision = inventory_detail::read(bytes, 156, 8);
  if (!validContentRemovalRecord(parsed)) return false;
  output = parsed;
  return true;
}
class ContentRemovalJournalStorage {
 public:
  virtual ~ContentRemovalJournalStorage() = default;
  virtual bool prepare() = 0;
  virtual FileStatus stat(const char* path, uint64_t& size) = 0;
  virtual bool read(const char* path, std::span<uint8_t> bytes) = 0;
  // Replace the complete slot, truncate, sync and close before returning success.
  virtual bool write(const char* path, std::span<const uint8_t> bytes) = 0;
};
// Session-owned; verified participant plans and all filesystem changes remain
// serialized by the caller. Scratch must be disjoint from command payloads.
class ContentRemovalJournal final {
 public:
  ContentRemovalJournal(ContentRemovalJournalStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  const ContentRemovalRecord* current() const { return ready ? &active : nullptr; }
  // Forget cached authority after checked external release or ambiguous IO.
  void invalidate() {
    ready = false;
    slot = -1;
  }
  ContentRemovalJournalResult recover(const ContentRemovalRecord& expected) {
    ready = false;
    slot = -1;
    if (scratch.size() < CONTENT_REMOVAL_RECORD_SIZE || !validContentRemovalRecord(expected))
      return ContentRemovalJournalResult::Invalid;
    seed = expected;
    selectedGeneration = seed.request.generation;
    hasExpected = true;
    return recoverSlots();
  }
  // Boot recovery has no connected owner; every valid slot must still agree on
  // the full persisted request/plan and belong to the current physical card.
  ContentRemovalJournalResult recover(const Identity& generation) {
    ready = false;
    slot = -1;
    if (scratch.size() < CONTENT_REMOVAL_RECORD_SIZE || !inventory_detail::nonzero(generation))
      return ContentRemovalJournalResult::Invalid;
    selectedGeneration = generation;
    hasExpected = false;
    return recoverSlots();
  }
  ContentRemovalJournalResult begin(const ContentRemovalRecord& initial) {
    if (initial.phase != ContentRemovalPhase::Prepared || initial.revision != 1)
      return ContentRemovalJournalResult::Invalid;
    const auto result = recover(initial);
    if (result != ContentRemovalJournalResult::Missing) return result;
    candidate = initial;
    return persist();
  }
  // A record recovered after ambiguous I/O may still be buffered in this boot.
  // Sync/read back a replacement copy before authorizing filesystem mutations.
  ContentRemovalJournalResult confirmRecovered() {
    if (!ready) return ContentRemovalJournalResult::Invalid;
    candidate = active;
    return persist();
  }
  // Call only after verifying the corresponding participant operation completed.
  ContentRemovalJournalResult advance(ContentRemovalPhase next) {
    if (!ready) return ContentRemovalJournalResult::Invalid;
    if (next == active.phase) return ContentRemovalJournalResult::Ok;
    if (static_cast<unsigned>(next) != static_cast<unsigned>(active.phase) + 1 || static_cast<unsigned>(next) > 3)
      return ContentRemovalJournalResult::Invalid;
    candidate = active;
    candidate.phase = next;
    ++candidate.revision;
    return persist();
  }

 private:
  ContentRemovalJournalStorage& storage;
  std::span<uint8_t> scratch;
  ContentRemovalRecord active, candidate, readback, seed;
  Identity selectedGeneration{};
  bool hasExpected = false;
  int slot = -1;
  bool ready = false;
  ContentRemovalJournalResult recoverSlots() {
    if (!storage.prepare()) return ContentRemovalJournalResult::IoError;
    bool present = false;
    for (int at = 0; at < 2; ++at) {
      uint64_t size = 0;
      const auto status = storage.stat(CONTENT_REMOVAL_JOURNALS[at], size);
      if (status == FileStatus::Error) return ContentRemovalJournalResult::IoError;
      if (status == FileStatus::Missing) continue;
      present = true;
      if (size != CONTENT_REMOVAL_RECORD_SIZE) continue;
      auto bytes = scratch.first(CONTENT_REMOVAL_RECORD_SIZE);
      if (!storage.read(CONTENT_REMOVAL_JOURNALS[at], bytes)) return ContentRemovalJournalResult::IoError;
      if (!decodeContentRemovalRecord(bytes, candidate)) continue;
      if (candidate.request.generation != selectedGeneration ||
          (hasExpected && (candidate.request != seed.request || candidate.planHash != seed.planHash)) ||
          (slot >= 0 && (candidate.request != active.request || candidate.planHash != active.planHash)))
        return ContentRemovalJournalResult::Conflict;
      if (slot >= 0 && candidate != active) {
        const auto lower = std::min(candidate.revision, active.revision);
        const auto higher = std::max(candidate.revision, active.revision);
        if (higher != lower + 1) return ContentRemovalJournalResult::Corrupt;
      }
      if (slot < 0 || candidate.revision > active.revision) {
        active = candidate;
        slot = at;
      }
    }
    if (slot < 0) return present ? ContentRemovalJournalResult::Corrupt : ContentRemovalJournalResult::Missing;
    ready = true;
    return ContentRemovalJournalResult::Ok;
  }
  ContentRemovalJournalResult persist() {
    ready = false;
    auto bytes = scratch.first(CONTENT_REMOVAL_RECORD_SIZE);
    if (!encodeContentRemovalRecord(candidate, bytes)) return ContentRemovalJournalResult::Invalid;
    const int next = slot == 0 ? 1 : 0;
    const auto path = CONTENT_REMOVAL_JOURNALS[next];
    if (!storage.write(path, bytes)) return ContentRemovalJournalResult::IoError;
    uint64_t size = 0;
    if (storage.stat(path, size) != FileStatus::Present || size != bytes.size() || !storage.read(path, bytes))
      return ContentRemovalJournalResult::IoError;
    if (!decodeContentRemovalRecord(bytes, readback) || readback != candidate)
      return ContentRemovalJournalResult::Corrupt;
    active = candidate;
    slot = next;
    ready = true;
    return ContentRemovalJournalResult::Ok;
  }
};
}  // namespace companion
