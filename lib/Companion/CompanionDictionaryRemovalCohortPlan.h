#pragma once

#include "CompanionDictionaryRemovalPlan.h"

namespace companion {
inline constexpr size_t DICTIONARY_REMOVAL_COHORT_HEADER_SIZE = 147;
struct DictionaryRemovalCohortHeader {
  ContentRemovalRequest request;
  uint64_t inventoryRevision = 0, count = 0;
  uint32_t payloadCrc = 0;
  bool operator==(const DictionaryRemovalCohortHeader&) const = default;
};
static_assert(sizeof(DictionaryRemovalCohortHeader) < 256);
inline bool validDictionaryRemovalCohortHeader(const DictionaryRemovalCohortHeader& header) {
  return validContentRemovalRequest(header.request) && header.request.manifest.kind == ContentKind::Dictionary &&
         header.inventoryRevision && header.count &&
         header.count <= (UINT64_MAX - DICTIONARY_REMOVAL_COHORT_HEADER_SIZE) / DICTIONARY_REMOVAL_PLAN_SIZE;
}
inline size_t encodeDictionaryRemovalCohortHeader(const DictionaryRemovalCohortHeader& header,
                                                  std::span<uint8_t> output) {
  if (output.size() < DICTIONARY_REMOVAL_COHORT_HEADER_SIZE || !validDictionaryRemovalCohortHeader(header)) return 0;
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'D', 'R', 'M', 1, 0, 0, 0};
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  if (!encodeContentRemovalRequest(header.request, output.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE))) return 0;
  inventory_detail::write(output, 123, header.inventoryRevision, 8);
  inventory_detail::write(output, 131, header.count, 8);
  inventory_detail::write(output, 139, header.payloadCrc, 4);
  inventory_detail::write(output, 143, inventoryIndexCrc(output.first(143)), 4);
  return DICTIONARY_REMOVAL_COHORT_HEADER_SIZE;
}
inline bool decodeDictionaryRemovalCohortHeader(std::span<const uint8_t> bytes, DictionaryRemovalCohortHeader& output) {
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'D', 'R', 'M', 1, 0, 0, 0};
  if (bytes.size() != DICTIONARY_REMOVAL_COHORT_HEADER_SIZE ||
      !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
      inventory_detail::read(bytes, 143, 4) != inventoryIndexCrc(bytes.first(143)))
    return false;
  DictionaryRemovalCohortHeader parsed;
  if (!decodeContentRemovalRequest(bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE), parsed.request)) return false;
  parsed.inventoryRevision = inventory_detail::read(bytes, 123, 8);
  parsed.count = inventory_detail::read(bytes, 131, 8);
  parsed.payloadCrc = inventory_detail::read(bytes, 139, 4);
  if (!validDictionaryRemovalCohortHeader(parsed)) return false;
  output = parsed;
  return true;
}
// The native owner verifies the entire file's journal-bound SHA before mutation.
// Retain this fixed decoder outside the task stack; scratch and output are disjoint.
// Records use strictly increasing base paths, excluding duplicate exact paths.
class DictionaryRemovalCohortReader final {
 public:
  DictionaryRemovalCohortReader(InventoryIndexStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  const DictionaryRemovalCohortHeader* current() const { return ready ? &header : nullptr; }
  bool open(const ContentRemovalRequest& request) {
    ready = false;
    if (scratch.size() < DICTIONARY_REMOVAL_PLAN_SIZE || !storage.size(length) ||
        !storage.read(0, scratch.first(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE)) ||
        !decodeDictionaryRemovalCohortHeader(scratch.first(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE), checked) ||
        checked.request != request ||
        length != DICTIONARY_REMOVAL_COHORT_HEADER_SIZE + checked.count * DICTIONARY_REMOVAL_PLAN_SIZE)
      return false;
    header = checked;
    rewind();
    while (cursor < header.count)
      if (!readRecord()) return false;
    if (!finish()) return false;
    ready = true;
    rewind();
    return true;
  }
  void rewind() {
    cursor = 0;
    offset = DICTIONARY_REMOVAL_COHORT_HEADER_SIZE;
    crc = 0xffffffff;
    previous[0] = 0;
  }
  InventoryPathRecordResult next(DictionaryRemovalPlan& output) {
    if (!ready) return InventoryPathRecordResult::Error;
    const auto a = reinterpret_cast<uintptr_t>(&output), b = reinterpret_cast<uintptr_t>(scratch.data());
    if (!(a <= b ? sizeof(output) <= b - a : scratch.size() <= a - b)) return error();
    if (cursor == header.count) return finish() ? InventoryPathRecordResult::End : error();
    if (!readRecord()) return error();
    output = candidate;
    return InventoryPathRecordResult::Entry;
  }

 private:
  InventoryIndexStorage& storage;
  std::span<uint8_t> scratch;
  DictionaryRemovalCohortHeader header, checked;
  DictionaryRemovalPlanCodec codec;
  DictionaryRemovalPlan candidate;
  std::array<char, 128> previous{};
  uint64_t length = 0, offset = 0, cursor = 0;
  uint32_t crc = 0xffffffff;
  bool ready = false;
  bool readRecord() {
    auto bytes = scratch.first(DICTIONARY_REMOVAL_PLAN_SIZE);
    if (!storage.read(offset, bytes) || !codec.decode(bytes, candidate) || candidate.request != header.request ||
        (cursor && std::strcmp(previous.data(), candidate.installed.base.data()) >= 0))
      return false;
    previous = candidate.installed.base;
    crc = inventoryIndexCrcUpdate(crc, bytes);
    offset += bytes.size();
    ++cursor;
    return true;
  }
  bool finish() {
    uint64_t actual = 0;
    return offset == length && ~crc == header.payloadCrc && storage.size(actual) && actual == length &&
           storage.read(0, scratch.first(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE)) &&
           decodeDictionaryRemovalCohortHeader(scratch.first(DICTIONARY_REMOVAL_COHORT_HEADER_SIZE), checked) &&
           checked == header;
  }
  InventoryPathRecordResult error() {
    ready = false;
    return InventoryPathRecordResult::Error;
  }
};
}  // namespace companion
