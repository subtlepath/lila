#if LILA_TINTA

#include "HalCourseValidation.h"

#include <Logging.h>

#include <cstring>

#include "CompanionCourseSource.h"
#include "CompanionCourseValidation.h"
#include "HalInventoryIndexStorage.h"

namespace companion {
bool validateStagedCourse(const char* path, tinta::core::pack::Pack& pack, std::span<uint8_t> scratch,
                          CourseCandidateDetails& details) {
  struct Close {
    tinta::core::pack::Pack& pack;
    ~Close() { pack.close(); }
  };
  pack.close();
  if (!path || !path[0] || scratch.size() < 512) {
    LOG_ERR("COMPANION", "Invalid course validation arguments");
    return false;
  }
  HalInventoryIndexStorage storage;
  if (!storage.open(path)) return false;
  StoredCourseSource source(storage);
  Close close{pack};
  if (!source.attach()) {
    LOG_ERR("COMPANION", "Invalid course candidate size");
    return false;
  }
  const auto result = validateCourseCandidate(pack, source, scratch);
  if (result != CourseValidationResult::Ok) {
    LOG_ERR("COMPANION", "Invalid course candidate: %u", static_cast<unsigned>(result));
    return false;
  }
  CourseCandidateDetails verified{};
  verified.major = pack.formatMajor();
  verified.minor = pack.formatMinor();
  verified.edition = pack.contentVersion();
  verified.items = pack.itemCount();
  verified.lessons = pack.count(tinta::core::pack::Section::Less);
  verified.stories = pack.count(tinta::core::pack::Section::Stor);
  std::memcpy(verified.locale, pack.locale(), sizeof(verified.locale));
  if (verified.locale[0] == 0) {
    LOG_ERR("COMPANION", "Empty course locale");
    return false;
  }
  pack.close();
  details = verified;
  return true;
}
}  // namespace companion

#endif  // LILA_TINTA
