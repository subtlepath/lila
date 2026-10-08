#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace companion {

using Identity = std::array<uint8_t, 16>;
using Digest = std::array<uint8_t, 32>;
inline constexpr uint8_t RECORD_VERSION = 1;
inline constexpr size_t MAX_ANCESTORS = 4;

enum class Board : uint8_t { X4 = 1, Sticky = 2, X4Pro = 3, X4Classic = 4, PaperMono = 5 };
enum class ContentKind : uint8_t { Epub = 1, Course = 2, Font = 3, Dictionary = 4, Firmware = 5 };
enum class EventKind : uint8_t {
  ReadingPosition = 1,
  BookmarkPut = 2,
  BookmarkDelete = 3,
  Preference = 4,
  Review = 5,
  UndoReview = 6,
  Suspension = 7,
  LessonComplete = 8,
  Star = 9,
  ReadingComplete = 10,
};
enum class ClockQuality : uint8_t { Unknown = 0, Device = 1, Trusted = 2 };
enum class TransferPhase : uint8_t { Receiving = 1, Verified = 2, Installing = 3, Committed = 4, Aborted = 5 };

struct EventIdentity {
  Identity origin{};
  uint64_t epoch = 0;
  uint64_t sequence = 0;
  bool operator==(const EventIdentity&) const = default;
};

struct DeviceDescriptor {
  Identity device{};
  Identity storageGeneration{};
  Board board = Board::X4;
  uint32_t capabilities = 0;
  uint8_t batteryPercent = 0;
  uint8_t minimumProtocol = 1;
  uint8_t maximumProtocol = 1;
  Digest runningBuild{};
  bool operator==(const DeviceDescriptor&) const = default;
};

struct ContentManifest {
  Digest contentHash{};
  ContentKind kind = ContentKind::Epub;
  uint64_t length = 0;
  uint32_t formatVersion = 0;
  // Stable course identity; zero for content without a logical edition family.
  Identity logicalIdentity{};
  bool operator==(const ContentManifest&) const = default;
};

// Keep this object in session-owned storage; it exceeds the local stack budget.
struct SyncEvent {
  EventIdentity identity{};
  Identity storageGeneration{};
  EventKind kind = EventKind::ReadingPosition;
  Digest resource{};
  Digest bodyHash{};
  uint32_t studyDay = 0;
  uint64_t timestamp = 0;
  ClockQuality clockQuality = ClockQuality::Unknown;
  uint32_t schedulerVersion = 0;
  // Hash of the exact scheduler configuration, not just its implementation version.
  Digest schedulerConfiguration{};
  // At most four direct dependencies fit in one event envelope.
  uint8_t ancestorCount = 0;
  std::array<EventIdentity, MAX_ANCESTORS> ancestors{};
  bool operator==(const SyncEvent&) const = default;
};

struct SyncCheckpoint {
  Identity device{};
  Identity storageGeneration{};
  EventIdentity cursor{};
  Digest ancestryHash{};
  bool operator==(const SyncCheckpoint&) const = default;
};

struct TransferState {
  Identity transaction{};
  Identity owner{};
  Identity storageGeneration{};
  Digest contentHash{};
  uint64_t length = 0;
  uint64_t durableOffset = 0;
  TransferPhase phase = TransferPhase::Receiving;
  bool operator==(const TransferState&) const = default;
};

enum class RecordKind : uint8_t {
  DeviceDescriptor = 1,
  ContentManifest = 2,
  SyncEvent = 3,
  SyncCheckpoint = 4,
  TransferState = 5
};

// Exact sizes include version and record-kind bytes. SyncEvent uses a variable
// tail of at most four causal identities; callers must bound the input first.
inline constexpr size_t DEVICE_DESCRIPTOR_SIZE = 74;
inline constexpr size_t CONTENT_MANIFEST_SIZE = 63;
inline constexpr size_t SYNC_EVENT_BASE_SIZE = 165;
inline constexpr size_t SYNC_CHECKPOINT_SIZE = 98;
inline constexpr size_t TRANSFER_STATE_SIZE = 99;
inline constexpr size_t MAX_RECORD_SIZE = SYNC_EVENT_BASE_SIZE + 32 * MAX_ANCESTORS;

size_t encodeRecord(const DeviceDescriptor& record, std::span<uint8_t> output);
size_t encodeRecord(const ContentManifest& record, std::span<uint8_t> output);
size_t encodeRecord(const SyncEvent& record, std::span<uint8_t> output);
size_t encodeRecord(const SyncCheckpoint& record, std::span<uint8_t> output);
size_t encodeRecord(const TransferState& record, std::span<uint8_t> output);
bool decodeRecord(std::span<const uint8_t> input, DeviceDescriptor& record);
bool decodeRecord(std::span<const uint8_t> input, ContentManifest& record);
bool decodeRecord(std::span<const uint8_t> input, SyncEvent& record);
bool decodeRecord(std::span<const uint8_t> input, SyncCheckpoint& record);
bool decodeRecord(std::span<const uint8_t> input, TransferState& record);

}  // namespace companion
