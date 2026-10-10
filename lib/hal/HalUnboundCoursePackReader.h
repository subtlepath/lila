#pragma once

#include "CompanionCourseValidation.h"
#include "CompanionUnboundCourseMigrationIntent.h"
#include "HalCompanionHeapAdmission.h"
#include "HalCourseRemovalMetadata.h"
#include "HalInventoryFileHash.h"

namespace companion {
enum class UnboundPackRole { Original, Installed };
struct UnboundPackLoan {
  tinta::core::pack::Pack* pack = nullptr;
  tinta::core::pack::PackSource* source = nullptr;
  explicit operator bool() const { return pack && source; }
};
// Admit off stack. Caller excludes pack writers and owns parser/workspace until
// close; every loan is scoped to one intent and role and expires on the next open.
class HalUnboundCoursePackReader final {
 public:
  using Permission = bool (*)(void*);
  HalUnboundCoursePackReader(std::span<uint8_t> scratch, tinta::core::pack::Pack& parser, Permission permitted,
                             void* context)
      : scratch(scratch),
        parser(parser),
        permitted(permitted),
        context(context),
        metadata(allowed, this),
        source(*this) {}
  ~HalUnboundCoursePackReader() { closeReaders(); }
  HalUnboundCoursePackReader(const HalUnboundCoursePackReader&) = delete;
  HalUnboundCoursePackReader& operator=(const HalUnboundCoursePackReader&) = delete;
  bool open(const UnboundCourseMigrationIntent& intent, UnboundPackRole role, std::string_view originalPath = {}) {
    if (operating) return false;
    ready = false;
    if (!validUnboundCourseMigrationIntent(intent) ||
        (role != UnboundPackRole::Original && role != UnboundPackRole::Installed) || scratch.size() < 512 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &parser, sizeof(parser)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), &intent, sizeof(intent)))
      return failure();
    const auto selectedPath = role == UnboundPackRole::Original ? originalPath : std::string_view(ACTIVE_COURSE_PATH);
    if (selectedPath.empty() || selectedPath.size() >= path.size() ||
        selectedPath.find('\0') != std::string_view::npos ||
        course_baseline_detail::overlaps(selectedPath.data(), selectedPath.size(), this, sizeof(*this)))
      return failure();
    selected = intent;
    selectedRole = role;
    std::copy(selectedPath.begin(), selectedPath.end(), path.begin());
    path[selectedPath.size()] = 0;
    operating = true;
    const bool closed = closeReaders();
    cancelled = false;
    bool valid = closed && guard() && hash();
    source.length = manifest().length;
    source.failed = false;
    source.reads = 0;
    if (valid)
      valid = validate() && hash() && !source.failed && guard() && !cancelled && parser.isOpen() &&
              source.length == manifest().length;
    if (!valid) closeReaders();
    ready = valid;
    operating = false;
    return ready || failure();
  }
  UnboundPackLoan borrowed(const UnboundCourseMigrationIntent& intent, UnboundPackRole role) {
    if (operating || selected != intent || selectedRole != role) return {};
    operating = true;
    if (!guard() || source.failed || !parser.isOpen()) ready = false;
    operating = false;
    return ready ? UnboundPackLoan{&parser, &source} : UnboundPackLoan{};
  }
  bool closeReaders() {
    if (operating) cancelled = true;
    ready = false;
    parser.close();
    source.length = 0;
    const bool fileClosed = !reader.isOpen() || reader.close();
    const bool metadataClosed = metadata.closeReaders();
    return fileClosed && metadataClosed;
  }

 private:
  class Source final : public tinta::core::pack::PackSource {
   public:
    explicit Source(HalUnboundCoursePackReader& owner) : owner(owner) {}
    uint32_t size() const override { return length; }
    bool read(uint32_t offset, void* output, uint32_t count) override {
      if (reading || (owner.operating && !owner.validating)) return false;
      reading = true;
      const bool previousOperating = owner.operating;
      owner.operating = true;
      const auto expectedLength = length;
      const bool valid = !failed && !owner.cancelled && (previousOperating || owner.ready) && owner.guard() &&
                         offset <= length && count <= length - offset && (!count || output) &&
                         owner.metadata.read(owner.path.data(), offset, {static_cast<uint8_t*>(output), count}) &&
                         owner.guard() && length == expectedLength && (previousOperating || owner.ready);
      if (!valid) {
        failed = true;
        owner.ready = false;
      }
      if (valid && ++reads == 32) {
        reads = 0;
        vTaskDelay(1);
      }
      owner.operating = previousOperating;
      reading = false;
      return valid;
    }
    uint32_t length = 0;
    uint8_t reads = 0;
    bool failed = false;
    bool reading = false;

   private:
    HalUnboundCoursePackReader& owner;
  };
  std::span<uint8_t> scratch;
  tinta::core::pack::Pack& parser;
  Permission permitted;
  void* context;
  HalCourseRemovalMetadata metadata;
  HalFile reader;
  Source source;
  UnboundCourseMigrationIntent selected;
  UnboundPackRole selectedRole = UnboundPackRole::Original;
  std::array<char, INVENTORY_PATH_LIMIT + 1> path{};
  bool operating = false, ready = false, validating = false, cancelled = false;
  const ContentManifest& manifest() const {
    return selectedRole == UnboundPackRole::Original ? selected.request.original.manifest : selected.activePack;
  }
  bool guard() const { return permitted && permitted(context) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalUnboundCoursePackReader*>(context)->guard(); }
  [[gnu::noinline]] bool validate() {
    validating = true;
    const bool valid = validateCourseCandidate(parser, source, scratch) == CourseValidationResult::Ok &&
                       parser.formatMajor() == manifest().formatVersion;
    validating = false;
    return valid && guard();
  }
  [[gnu::noinline]] bool hash() {
    uint64_t length = 0, actualLength = 0;
    Digest actual{};
    if (!metadata.closeReaders() || !guard() || metadata.stat(path.data(), length) != FileStatus::Present ||
        length != manifest().length || !Storage.openFileForReadReusing("COMPANION", path.data(), reader))
      return false;
    const bool valid = !reader.isDirectory() &&
                       hashInventoryFile(reader, scratch, actualLength, actual, allowed, this) && reader.sync();
    const bool closed = reader.close();
    return valid && closed && actualLength == manifest().length && actual == manifest().contentHash && guard();
  }
  static bool failure() {
    LOG_ERR("COMPANION", "Unbound course pack loan refused");
    return false;
  }
};
}  // namespace companion
