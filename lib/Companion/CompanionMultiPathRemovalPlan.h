#pragma once

#include "CompanionSingleFileRemovalPlan.h"

namespace companion {
inline constexpr size_t MULTI_PATH_REMOVAL_HEADER_SIZE = 155;
inline constexpr size_t MULTI_PATH_REMOVAL_RECORD_MAX = INVENTORY_PATH_LIMIT + 6;
inline constexpr size_t MULTI_PATH_REMOVAL_RECORD_MIN = 8;
struct MultiPathRemovalPlanHeader {
  ContentRemovalRequest request;
  uint64_t inventoryRevision = 0, count = 0, recordsLength = 0;
  uint32_t payloadCrc = 0;
  bool operator==(const MultiPathRemovalPlanHeader&) const = default;
};
static_assert(sizeof(MultiPathRemovalPlanHeader) < 256);
inline bool validMultiPathRemovalPlanHeader(const MultiPathRemovalPlanHeader& header) {
  return validContentRemovalRequest(header.request) &&
         (header.request.manifest.kind == ContentKind::Epub || header.request.manifest.kind == ContentKind::Font) &&
         header.inventoryRevision != 0 && header.count != 0 &&
         header.recordsLength <= UINT64_MAX - MULTI_PATH_REMOVAL_HEADER_SIZE &&
         header.count <= header.recordsLength / MULTI_PATH_REMOVAL_RECORD_MIN &&
         header.count >= (header.recordsLength - 1) / MULTI_PATH_REMOVAL_RECORD_MAX + 1;
}
inline size_t encodeMultiPathRemovalPlanHeader(const MultiPathRemovalPlanHeader& header, std::span<uint8_t> output) {
  if (output.size() < MULTI_PATH_REMOVAL_HEADER_SIZE || !validMultiPathRemovalPlanHeader(header)) return 0;
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'M', 'P', 1, 0, 0, 0};
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  if (!encodeContentRemovalRequest(header.request, output.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE))) return 0;
  inventory_detail::write(output, 123, header.inventoryRevision, 8);
  inventory_detail::write(output, 131, header.count, 8);
  inventory_detail::write(output, 139, header.recordsLength, 8);
  inventory_detail::write(output, 147, header.payloadCrc, 4);
  inventory_detail::write(output, 151, inventoryIndexCrc(output.first(151)), 4);
  return MULTI_PATH_REMOVAL_HEADER_SIZE;
}
namespace multi_path_removal_detail {
// Keep nested request decoding out of the header-validation frame.
[[gnu::noinline]] inline bool decodeRequest(std::span<const uint8_t> bytes, ContentRemovalRequest& output) {
  return decodeContentRemovalRequest(bytes, output);
}
}  // namespace multi_path_removal_detail
inline bool decodeMultiPathRemovalPlanHeader(std::span<const uint8_t> bytes, MultiPathRemovalPlanHeader& output) {
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'M', 'P', 1, 0, 0, 0};
  if (bytes.size() != MULTI_PATH_REMOVAL_HEADER_SIZE || !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
      inventory_detail::read(bytes, 151, 4) != inventoryIndexCrc(bytes.first(151)))
    return false;
  MultiPathRemovalPlanHeader parsed;
  if (!multi_path_removal_detail::decodeRequest(bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE), parsed.request))
    return false;
  parsed.inventoryRevision = inventory_detail::read(bytes, 123, 8);
  parsed.count = inventory_detail::read(bytes, 131, 8);
  parsed.recordsLength = inventory_detail::read(bytes, 139, 8);
  parsed.payloadCrc = inventory_detail::read(bytes, 147, 4);
  if (!validMultiPathRemovalPlanHeader(parsed)) return false;
  output = parsed;
  return true;
}
// Path and encoding output must not overlap.
inline size_t encodeMultiPathRemovalRecord(const ContentRemovalRequest& request, std::string_view path,
                                           std::span<uint8_t> output) {
  const auto size = path.size() + 6;
  if (!validSingleFileRemovalPlan({request, path}) || output.size() < size) return 0;
  inventory_detail::write(output, 0, path.size(), 2);
  memcpy(output.data() + 2, path.data(), path.size());
  inventory_detail::write(output, size - 4, inventoryIndexCrc(output.first(size - 4)), 4);
  return size;
}
// Returned view borrows bytes; the streaming reader exposes copied paths instead.
inline bool decodeMultiPathRemovalRecord(const ContentRemovalRequest& request, std::span<const uint8_t> bytes,
                                         std::string_view& output) {
  if (bytes.size() < MULTI_PATH_REMOVAL_RECORD_MIN || bytes.size() > MULTI_PATH_REMOVAL_RECORD_MAX ||
      inventory_detail::read(bytes, 0, 2) != bytes.size() - 6 ||
      inventory_detail::read(bytes, bytes.size() - 4, 4) != inventoryIndexCrc(bytes.first(bytes.size() - 4)))
    return false;
  const std::string_view path(reinterpret_cast<const char*>(bytes.data() + 2), bytes.size() - 6);
  if (!validSingleFileRemovalPlan({request, path})) return false;
  output = path;
  return true;
}
// The owner excludes plan writers and verifies the entire file's journal-bound
// SHA-256 before arming participant mutations. CRCs alone do not authorize IO.
class MultiPathRemovalPlanReader final {
 public:
  MultiPathRemovalPlanReader(InventoryIndexStorage& storage, std::span<uint8_t> scratch)
      : storage(storage), scratch(scratch) {}
  const MultiPathRemovalPlanHeader* current() const { return ready ? &header : nullptr; }
  bool open(const ContentRemovalRequest& request) {
    ready = false;
    if (scratch.size() < MULTI_PATH_REMOVAL_RECORD_MAX || !storage.size(length) ||
        !storage.read(0, scratch.first(MULTI_PATH_REMOVAL_HEADER_SIZE)) ||
        !decodeMultiPathRemovalPlanHeader(scratch.first(MULTI_PATH_REMOVAL_HEADER_SIZE), checked) ||
        checked.request != request || length != MULTI_PATH_REMOVAL_HEADER_SIZE + checked.recordsLength)
      return false;
    header = checked;
    rewind();
    for (; cursor < header.count; ++cursor) {
      std::string_view path;
      size_t size = 0;
      if (!read(path, size)) return false;
      crc = inventoryIndexCrcUpdate(crc, scratch.first(size - 4));
      offset += size;
    }
    if (!finish()) return false;
    ready = true;
    rewind();
    return true;
  }
  void rewind() {
    cursor = 0;
    offset = MULTI_PATH_REMOVAL_HEADER_SIZE;
    crc = 0xffffffff;
  }
  InventoryPathRecordResult next(std::span<char> output) {
    if (!ready) return InventoryPathRecordResult::Error;
    const auto a = reinterpret_cast<uintptr_t>(output.data());
    const auto b = reinterpret_cast<uintptr_t>(scratch.data());
    if (!(a <= b ? output.size() <= b - a : scratch.size() <= a - b)) return error();
    if (cursor == header.count) return finish() ? InventoryPathRecordResult::End : error();
    std::string_view path;
    size_t size = 0;
    if (!read(path, size) || output.size() <= path.size()) return error();
    memcpy(output.data(), path.data(), path.size());
    output[path.size()] = 0;
    crc = inventoryIndexCrcUpdate(crc, scratch.first(size - 4));
    offset += size;
    ++cursor;
    return InventoryPathRecordResult::Entry;
  }

 private:
  InventoryIndexStorage& storage;
  std::span<uint8_t> scratch;
  MultiPathRemovalPlanHeader header, checked;
  uint64_t length = 0, offset = 0, cursor = 0;
  uint32_t crc = 0xffffffff;
  bool ready = false;
  bool read(std::string_view& path, size_t& size) {
    if (offset > length || length - offset < 2 || !storage.read(offset, scratch.first(2))) return false;
    const auto pathLength = inventory_detail::read(scratch, 0, 2);
    if (pathLength < 2 || pathLength > INVENTORY_PATH_LIMIT) return false;
    size = static_cast<size_t>(pathLength) + 6;
    return size <= length - offset && storage.read(offset, scratch.first(size)) &&
           decodeMultiPathRemovalRecord(header.request, scratch.first(size), path);
  }
  bool finish() {
    uint64_t actual = 0;
    return offset == length && ~crc == header.payloadCrc && storage.size(actual) && actual == length &&
           storage.read(0, scratch.first(MULTI_PATH_REMOVAL_HEADER_SIZE)) &&
           decodeMultiPathRemovalPlanHeader(scratch.first(MULTI_PATH_REMOVAL_HEADER_SIZE), checked) &&
           checked == header;
  }
  InventoryPathRecordResult error() {
    ready = false;
    return InventoryPathRecordResult::Error;
  }
};
}  // namespace companion
