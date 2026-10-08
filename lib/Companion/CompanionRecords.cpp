#include "CompanionRecords.h"

#include <algorithm>

namespace companion {
namespace {

class Writer {
 public:
  explicit Writer(std::span<uint8_t> bytes) : bytes(bytes) {}
  void number(uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) bytes[position++] = static_cast<uint8_t>(value >> (8 * i));
  }
  void data(std::span<const uint8_t> value) {
    for (const uint8_t byte : value) bytes[position++] = byte;
  }
  void event(const EventIdentity& value) {
    data(value.origin);
    number(value.epoch, 8);
    number(value.sequence, 8);
  }
  void header(RecordKind kind) {
    number(RECORD_VERSION, 1);
    number(static_cast<uint8_t>(kind), 1);
  }

 private:
  std::span<uint8_t> bytes;
  size_t position = 0;
};

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> bytes) : bytes(bytes) {}
  uint64_t number(size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= static_cast<uint64_t>(bytes[position++]) << (8 * i);
    return value;
  }
  void data(std::span<uint8_t> value) {
    for (uint8_t& byte : value) byte = bytes[position++];
  }
  void event(EventIdentity& value) {
    data(value.origin);
    value.epoch = number(8);
    value.sequence = number(8);
  }

 private:
  std::span<const uint8_t> bytes;
  size_t position = 2;
};

bool header(std::span<const uint8_t> bytes, RecordKind kind, size_t size) {
  return bytes.size() == size && bytes[0] == RECORD_VERSION && bytes[1] == static_cast<uint8_t>(kind);
}
bool range(uint8_t value, uint8_t first, uint8_t last) { return value >= first && value <= last; }
bool valid(const DeviceDescriptor& value) {
  return range(static_cast<uint8_t>(value.board), 1, 5) && value.batteryPercent <= 100 && value.minimumProtocol != 0 &&
         value.maximumProtocol >= value.minimumProtocol;
}
bool valid(const TransferState& value) {
  return range(static_cast<uint8_t>(value.phase), 1, 5) && value.durableOffset <= value.length &&
         (value.phase == TransferPhase::Receiving || value.phase == TransferPhase::Aborted ||
          value.durableOffset == value.length);
}
}  // namespace

size_t encodeRecord(const DeviceDescriptor& record, std::span<uint8_t> output) {
  if (output.size() < DEVICE_DESCRIPTOR_SIZE || !valid(record)) return 0;
  Writer writer(output);
  writer.header(RecordKind::DeviceDescriptor);
  writer.data(record.device);
  writer.data(record.storageGeneration);
  writer.number(static_cast<uint8_t>(record.board), 1);
  writer.number(record.capabilities, 4);
  writer.number(record.batteryPercent, 1);
  writer.number(record.minimumProtocol, 1);
  writer.number(record.maximumProtocol, 1);
  writer.data(record.runningBuild);
  return DEVICE_DESCRIPTOR_SIZE;
}

bool decodeRecord(std::span<const uint8_t> input, DeviceDescriptor& record) {
  if (!header(input, RecordKind::DeviceDescriptor, DEVICE_DESCRIPTOR_SIZE)) return false;
  DeviceDescriptor parsed;
  Reader reader(input);
  reader.data(parsed.device);
  reader.data(parsed.storageGeneration);
  parsed.board = static_cast<Board>(reader.number(1));
  parsed.capabilities = reader.number(4);
  parsed.batteryPercent = reader.number(1);
  parsed.minimumProtocol = reader.number(1);
  parsed.maximumProtocol = reader.number(1);
  reader.data(parsed.runningBuild);
  if (!valid(parsed)) return false;
  record = parsed;
  return true;
}

size_t encodeRecord(const ContentManifest& record, std::span<uint8_t> output) {
  if (output.size() < CONTENT_MANIFEST_SIZE || !range(static_cast<uint8_t>(record.kind), 1, 5)) return 0;
  Writer writer(output);
  writer.header(RecordKind::ContentManifest);
  writer.data(record.contentHash);
  writer.number(static_cast<uint8_t>(record.kind), 1);
  writer.number(record.length, 8);
  writer.number(record.formatVersion, 4);
  writer.data(record.logicalIdentity);
  return CONTENT_MANIFEST_SIZE;
}

bool decodeRecord(std::span<const uint8_t> input, ContentManifest& record) {
  if (!header(input, RecordKind::ContentManifest, CONTENT_MANIFEST_SIZE) || !range(input[34], 1, 5)) return false;
  Reader reader(input);
  reader.data(record.contentHash);
  record.kind = static_cast<ContentKind>(reader.number(1));
  record.length = reader.number(8);
  record.formatVersion = reader.number(4);
  reader.data(record.logicalIdentity);
  return true;
}

size_t encodeRecord(const SyncEvent& record, std::span<uint8_t> output) {
  if (record.ancestorCount > MAX_ANCESTORS || !range(static_cast<uint8_t>(record.kind), 1, 10) ||
      !range(static_cast<uint8_t>(record.clockQuality), 0, 2))
    return 0;
  const size_t size = SYNC_EVENT_BASE_SIZE + 32 * record.ancestorCount;
  if (output.size() < size) return 0;
  Writer writer(output);
  writer.header(RecordKind::SyncEvent);
  writer.event(record.identity);
  writer.data(record.storageGeneration);
  writer.number(static_cast<uint8_t>(record.kind), 1);
  writer.data(record.resource);
  writer.data(record.bodyHash);
  writer.number(record.studyDay, 4);
  writer.number(record.timestamp, 8);
  writer.number(static_cast<uint8_t>(record.clockQuality), 1);
  writer.number(record.schedulerVersion, 4);
  writer.data(record.schedulerConfiguration);
  writer.number(record.ancestorCount, 1);
  for (size_t i = 0; i < record.ancestorCount; ++i) writer.event(record.ancestors[i]);
  return size;
}

bool decodeRecord(std::span<const uint8_t> input, SyncEvent& record) {
  // Validate the complete variable-length record before writing into the caller's
  // session-owned object; a temporary SyncEvent would exceed the stack budget.
  if (input.size() < SYNC_EVENT_BASE_SIZE || input[164] > MAX_ANCESTORS ||
      !header(input, RecordKind::SyncEvent, SYNC_EVENT_BASE_SIZE + 32 * input[164]) || !range(input[50], 1, 10) ||
      !range(input[127], 0, 2))
    return false;
  Reader reader(input);
  reader.event(record.identity);
  reader.data(record.storageGeneration);
  record.kind = static_cast<EventKind>(reader.number(1));
  reader.data(record.resource);
  reader.data(record.bodyHash);
  record.studyDay = reader.number(4);
  record.timestamp = reader.number(8);
  record.clockQuality = static_cast<ClockQuality>(reader.number(1));
  record.schedulerVersion = reader.number(4);
  reader.data(record.schedulerConfiguration);
  record.ancestorCount = reader.number(1);
  for (size_t i = 0; i < MAX_ANCESTORS; ++i) {
    if (i < record.ancestorCount)
      reader.event(record.ancestors[i]);
    else
      record.ancestors[i] = {};
  }
  return true;
}

size_t encodeRecord(const SyncCheckpoint& record, std::span<uint8_t> output) {
  if (output.size() < SYNC_CHECKPOINT_SIZE) return 0;
  Writer writer(output);
  writer.header(RecordKind::SyncCheckpoint);
  writer.data(record.device);
  writer.data(record.storageGeneration);
  writer.event(record.cursor);
  writer.data(record.ancestryHash);
  return SYNC_CHECKPOINT_SIZE;
}

bool decodeRecord(std::span<const uint8_t> input, SyncCheckpoint& record) {
  if (!header(input, RecordKind::SyncCheckpoint, SYNC_CHECKPOINT_SIZE)) return false;
  Reader reader(input);
  reader.data(record.device);
  reader.data(record.storageGeneration);
  reader.event(record.cursor);
  reader.data(record.ancestryHash);
  return true;
}

size_t encodeRecord(const TransferState& record, std::span<uint8_t> output) {
  if (output.size() < TRANSFER_STATE_SIZE || !valid(record)) return 0;
  Writer writer(output);
  writer.header(RecordKind::TransferState);
  writer.data(record.transaction);
  writer.data(record.owner);
  writer.data(record.storageGeneration);
  writer.data(record.contentHash);
  writer.number(record.length, 8);
  writer.number(record.durableOffset, 8);
  writer.number(static_cast<uint8_t>(record.phase), 1);
  return TRANSFER_STATE_SIZE;
}

bool decodeRecord(std::span<const uint8_t> input, TransferState& record) {
  if (!header(input, RecordKind::TransferState, TRANSFER_STATE_SIZE)) return false;
  TransferState parsed;
  Reader reader(input);
  reader.data(parsed.transaction);
  reader.data(parsed.owner);
  reader.data(parsed.storageGeneration);
  reader.data(parsed.contentHash);
  parsed.length = reader.number(8);
  parsed.durableOffset = reader.number(8);
  parsed.phase = static_cast<TransferPhase>(reader.number(1));
  if (!valid(parsed)) return false;
  record = parsed;
  return true;
}

}  // namespace companion
