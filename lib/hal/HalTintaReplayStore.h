#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../Serialization/BinaryRecordBytes.h"
#include "CompanionCourseStatePaths.h"
#include "CompanionTintaReplayReducer.h"

namespace companion {
// Disposable replay workspace; caller excludes other replay/publication writers.
class HalTintaReplayStore final : public TintaReplayStore {
 public:
  ~HalTintaReplayStore() { close(); }
  bool begin(const Identity& course) {
    ready = false;
    if (!close() || !courseStateDirectory(course, root) || !courseStatePath(course, "replay-work", path) ||
        !Storage.ensureDirectoryExists(root.data()) || !Storage.openFileForWriteReusing("COMPANION", path.data(), file))
      return fail("begin");
    bytes.fill(0);
    bytes[0] = 'T';
    bytes[1] = 'R';
    bytes[2] = 'W';
    bytes[3] = 1;
    std::copy(course.begin(), course.end(), bytes.begin() + 4);
    headerChecksum = binary_record::crc32(bytes.data(), 20);
    tinta_body_detail::write(bytes, 20, headerChecksum, 4);
    if (file.write(bytes.data(), HEADER_SIZE) != HEADER_SIZE || !file.sync()) return fail("header");
    records = operations = 0;
    ready = true;
    return true;
  }
  bool item(uint32_t uid, tinta::core::ItemState& value) override {
    bool found = false;
    uint32_t slot = 0;
    if (!find(1, uid, slot, found)) return false;
    tinta::core::ItemState decoded = tinta::core::ItemState::fresh(uid);
    if (found && (!tinta::core::ItemState::decode(bytes.data() + 8, decoded) || decoded.uid != uid))
      return fail("item");
    value = decoded;
    return true;
  }
  bool putItem(const tinta::core::ItemState& value) override {
    data.fill(0);
    value.encode(data.data());
    tinta::core::ItemState checked;
    if (!tinta::core::ItemState::decode(data.data(), checked)) return fail("item encoding");
    return put(1, value.uid);
  }
  bool day(uint16_t day, TintaReplayDay& value) override {
    bool found = false;
    uint32_t slot = 0;
    if (!find(2, day, slot, found)) return false;
    TintaReplayDay decoded;
    if (found) {
      decoded.newItems = tinta_body_detail::read(bytes, 8, 4);
      decoded.reviews = tinta_body_detail::read(bytes, 12, 4);
      decoded.gradedReviews = tinta_body_detail::read(bytes, 16, 4);
      decoded.correctReviews = tinta_body_detail::read(bytes, 20, 4);
      decoded.responseMilliseconds = tinta_body_detail::read(bytes, 24, 8);
      if (decoded.newItems > decoded.gradedReviews || decoded.correctReviews > decoded.gradedReviews)
        return fail("day counters");
    }
    value = decoded;
    return true;
  }
  bool putDay(uint16_t day, const TintaReplayDay& value) override {
    if (value.newItems > value.gradedReviews || value.correctReviews > value.gradedReviews) return fail("day counters");
    tinta_body_detail::write(data, 0, value.newItems, 4);
    tinta_body_detail::write(data, 4, value.reviews, 4);
    tinta_body_detail::write(data, 8, value.gradedReviews, 4);
    tinta_body_detail::write(data, 12, value.correctReviews, 4);
    tinta_body_detail::write(data, 16, value.responseMilliseconds, 8);
    return put(2, day);
  }
  bool completion(EventKind kind, uint32_t uid, bool enabled) override {
    if (kind != EventKind::LessonComplete && kind != EventKind::ReadingComplete) return fail("completion kind");
    data.fill(0);
    data[0] = enabled;
    return put(kind == EventKind::LessonComplete ? 3 : 4, uid);
  }
  bool completed(EventKind kind, uint32_t uid, bool& enabled) {
    if (kind != EventKind::LessonComplete && kind != EventKind::ReadingComplete) return fail("completion kind");
    bool found = false;
    uint32_t slot = 0;
    if (!find(kind == EventKind::LessonComplete ? 3 : 4, uid, slot, found)) return false;
    if (found && bytes[8] > 1) return fail("completion value");
    enabled = found && bytes[8] != 0;
    return true;
  }
  bool matchesCourse(const Identity& course) {
    return checkHeader() && std::equal(course.begin(), course.end(), bytes.begin() + 4);
  }
  enum class Kind : uint8_t { Item = 1, Day = 2, Lesson = 3, Reading = 4 };
  // Caller freezes writes while enumerating. No match leaves the output key unchanged.
  bool nextKey(Kind kind, bool hasPrevious, uint32_t previous, uint32_t& key, bool& found) {
    const auto type = static_cast<uint8_t>(kind);
    if (type < 1 || type > 4 || !checkHeader()) return fail("enumeration arguments");
    bool selected = false;
    uint32_t candidate = 0;
    for (uint32_t slot = 0; slot < records; ++slot) {
      if (!readRecord(slot)) return false;
      const auto value = static_cast<uint32_t>(tinta_body_detail::read(bytes, 4, 4));
      if (bytes[0] != type || (hasPrevious && value <= previous)) continue;
      if (!selected || value < candidate) {
        candidate = value;
        selected = true;
      }
    }
    if (selected) key = candidate;
    found = selected;
    return true;
  }
  bool close() {
    ready = false;
    const bool ok = !file || file.close();
    if (!ok) LOG_ERR("COMPANION", "Replay store close failed");
    return ok;
  }

 private:
  bool checkHeader() {
    if (!ready || file.fileSize64() != HEADER_SIZE + uint64_t{records} * RECORD_SIZE) return fail("working extent");
    if (!file.seek64(0) || file.read(bytes.data(), HEADER_SIZE) != HEADER_SIZE || bytes[0] != 'T' || bytes[1] != 'R' ||
        bytes[2] != 'W' || bytes[3] != 1 || tinta_body_detail::read(bytes, 20, 4) != headerChecksum ||
        binary_record::crc32(bytes.data(), 20) != headerChecksum)
      return fail("header read");
    return true;
  }
  bool readRecord(uint32_t slot) {
    if (slot >= records || !file.seek64(HEADER_SIZE + uint64_t{slot} * RECORD_SIZE) ||
        file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size()) || bytes[0] < 1 || bytes[0] > 4 ||
        bytes[1] || bytes[2] || bytes[3] ||
        tinta_body_detail::read(bytes, 32, 4) != binary_record::crc32(bytes.data(), 32))
      return fail("record read");
    const auto key = tinta_body_detail::read(bytes, 4, 4);
    if ((bytes[0] == 2 && key > UINT16_MAX) || (bytes[0] != 2 && (!key || key == UINT32_MAX)))
      return fail("record key");
    yield();
    return true;
  }
  bool find(uint8_t kind, uint32_t key, uint32_t& slot, bool& found) {
    if ((kind != 2 && (!key || key == UINT32_MAX)) || !checkHeader()) return fail("lookup arguments");
    for (slot = 0; slot < records; ++slot) {
      if (!readRecord(slot)) return false;
      if (bytes[0] == kind && tinta_body_detail::read(bytes, 4, 4) == key) {
        found = true;
        return true;
      }
    }
    found = false;
    return true;
  }
  bool put(uint8_t kind, uint32_t key) {
    bool found = false;
    uint32_t slot = 0;
    if (!find(kind, key, slot, found) || (!found && records == UINT32_MAX)) return fail("write lookup");
    bytes.fill(0);
    bytes[0] = kind;
    tinta_body_detail::write(bytes, 4, key, 4);
    std::copy(data.begin(), data.end(), bytes.begin() + 8);
    tinta_body_detail::write(bytes, 32, binary_record::crc32(bytes.data(), 32), 4);
    if (!file.seek64(HEADER_SIZE + uint64_t{slot} * RECORD_SIZE) ||
        file.write(bytes.data(), bytes.size()) != bytes.size() || !file.sync())
      return fail("record write");
    if (!found) ++records;
    yield();
    return true;
  }
  bool fail(const char* stage) {
    ready = false;
    LOG_ERR("COMPANION", "Replay store %s failed", stage);
    return false;
  }
  void yield() {
    if (++operations == 32) {
      operations = 0;
      vTaskDelay(1);
    }
  }
  static constexpr uint32_t HEADER_SIZE = 24, RECORD_SIZE = 36;
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> root{};
  std::array<char, COURSE_STATE_PATH_SIZE> path{};
  std::array<uint8_t, RECORD_SIZE> bytes{};
  std::array<uint8_t, 24> data{};
  HalFile file;
  uint32_t records = 0, headerChecksum = 0;
  unsigned operations = 0;
  bool ready = false;
};
}  // namespace companion
