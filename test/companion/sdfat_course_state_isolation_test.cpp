#include <cassert>

#include "lib/hal/HalCourseStateIsolation.h"
using namespace companion;
namespace {
struct Metadata final : TransferStorage {
  unsigned mutations = 0;
  bool prepare() override {
    ++mutations;
    return false;
  }
  FileStatus stat(const char* path, uint64_t& length) override {
    const auto& state = inventory_hal_test::state;
    if (path == state.statErrorPath) return FileStatus::Error;
    const auto it = state.files.find(path);
    if (it == state.files.end()) return FileStatus::Missing;
    length = it->second.size();
    return FileStatus::Present;
  }
  bool read(const char* path, uint64_t offset, std::span<uint8_t> bytes) override {
    const auto& state = inventory_hal_test::state;
    if (path == state.readErrorPath) return false;
    const auto it = state.files.find(path);
    if (it == state.files.end() || offset > it->second.size() || bytes.size() > it->second.size() - offset)
      return false;
    std::copy_n(it->second.begin() + offset, bytes.size(), bytes.begin());
    return true;
  }
  bool write(const char*, uint64_t, std::span<const uint8_t>, bool) override {
    ++mutations;
    return false;
  }
  bool resize(const char*, uint64_t) override {
    ++mutations;
    return false;
  }
  bool rename(const char*, const char*) override {
    ++mutations;
    return false;
  }
  bool remove(const char*) override {
    ++mutations;
    return false;
  }
  bool verify(const char*, uint64_t, const Digest&, std::span<uint8_t>) override { return false; }
};
struct Permission {
  unsigned calls = 0, revoke = 0;
  static bool check(void* context) {
    auto& self = *static_cast<Permission*>(context);
    return ++self.calls != self.revoke;
  }
};
void receipt(const CourseMigrationPaths& paths, const Identity& origin) {
  for (const auto* path : {paths.intent, paths.done}) {
    const bool intent = path == paths.intent;
    auto& bytes = inventory_hal_test::state.files[path];
    bytes.resize(intent ? 28 : 24);
    std::memcpy(bytes.data(), intent ? "CLSM" : "CLSD", 4);
    std::copy(origin.begin(), origin.end(), bytes.begin() + 4);
    const auto end = bytes.size() - 4;
    const auto crc = courseBindingCrc(std::span(bytes).first(end));
    for (unsigned i = 0; i < 4; ++i) bytes[end + i] = static_cast<uint8_t>(crc >> (8 * i));
  }
}
}  // namespace
int main() {
  Identity original{}, current{};
  original.fill(1);
  current.fill(0xab);
  std::array<char, COURSE_STATE_DIRECTORY_SIZE> path{};
  assert(courseStateDirectory(current, path));
  const std::string directory(path.data()), name(path.data() + COURSE_STATE_ROOT.size());
  auto& state = inventory_hal_test::state;
  state = {};
  state.directories["/tinta/courses"] = {{name, true}};
  state.directories[directory] = {};
  state.files[directory + "/reviews.log"] = {7, 8, 9};
  receipt(COURSE_STATE_MIGRATION_PATHS, original);
  receipt(COURSE_MARK_MIGRATION_PATHS, original);
  const auto clean = state;
  Metadata metadata;
  std::array<uint8_t, 28> scratch{};
  Permission permission;
  HalCourseStateIsolation isolation(metadata, scratch, &Permission::check, &permission);
  auto assertClosed = [&] {
    const auto closes = state.closes;
    assert(isolation.closeReaders());
    assert(state.closes == closes);
  };
  assert(isolation.verify(current));
  const auto permissionCalls = permission.calls;
  assert(state.files == clean.files && metadata.mutations == 0);
  assertClosed();
  // Every permission check refuses a revoked owner, including after matching.
  for (unsigned revoke = 1; revoke <= permissionCalls; ++revoke) {
    state = clean;
    permission = {0, revoke};
    assert(!isolation.verify(current));
    assert(state.files == clean.files && metadata.mutations == 0);
    assertClosed();
  }
  for (unsigned fault = 0; fault < 15; ++fault) {
    state = clean;
    permission = {};
    switch (fault) {
      case 0:
        state.directories.erase(directory);
        break;
      case 1:
        state.directories["/tinta/courses"].clear();
        break;
      case 2:
        state.directories["/tinta/courses"].push_back({name, true});
        break;
      case 3:
        state.directoryErrorPath = "/tinta/courses";
        break;
      case 4:
        state.failClosePath = directory;
        break;
      case 5:
        state.failClosePath = "/tinta/courses";
        break;
      case 6:
        state.failShortName = true;
        break;
      case 7:
        state.failOpen = true;
        break;
      case 8:
        state.directories["/tinta/courses"].push_back({std::string(1, '\xff'), false});
        break;
      case 9:
        state.files["/tinta/items.bin"] = {1};
        break;
      case 10:
        state.files[COURSE_MARK_MIGRATION_PATHS.doneStage] = {1};
        break;
      case 11:
        state.files.erase(COURSE_STATE_MIGRATION_DONE);
        break;
      case 12:
        receipt(COURSE_MARK_MIGRATION_PATHS, current);
        break;
      case 13:
        state.statErrorPath = COURSE_STATE_MIGRATION;
        break;
      case 14:
        state.readErrorPath = COURSE_MARK_MIGRATION_PATHS.done;
        break;
    }
    const auto before = state.files;
    assert(!isolation.verify(current));
    assert(state.files == before && metadata.mutations == 0);
    assertClosed();
    state = clean;
    permission = {};
    assert(isolation.verify(current));
  }
  state = clean;
  permission = {};
  std::string upper = name;
  std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return c >= 'a' && c <= 'f' ? c - 32 : c; });
  state.directories["/tinta/courses"] = {{upper, true}};
  state.directories[directory].clear();
  state.directories[std::string("/tinta/courses/") + upper] = {};
  assert(isolation.verify(current));
  state.directories["/tinta/courses"].push_back({name, true});
  assert(!isolation.verify(current));
  state = clean;
  for (unsigned i = 0; i < 64; ++i)
    state.directories["/tinta/courses"].push_back({"unrelated-" + std::to_string(i), false});
  permission = {};
  assert(isolation.verify(current));
  assert(state.yields == 2);
  assert(!isolation.verify(Identity{}));
  HalCourseStateIsolation unauthorized(metadata, scratch, nullptr, nullptr);
  assert(!unauthorized.verify(current));
  HalCourseStateIsolation undersized(metadata, std::span(scratch).first(27), &Permission::check, &permission);
  assert(!undersized.verify(current));
  assert(metadata.mutations == 0);
}
