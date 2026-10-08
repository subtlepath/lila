#pragma once

#include "CompanionContentRemovalRequest.h"
#include "CompanionInventoryPaths.h"
#include "CompanionZipPathValidation.h"

namespace companion {
inline constexpr size_t SINGLE_FILE_REMOVAL_PLAN_PREFIX = 125;
inline constexpr size_t SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE = SINGLE_FILE_REMOVAL_PLAN_PREFIX + INVENTORY_PATH_LIMIT + 4;
// Path borrows the encoded plan or the authoritative inventory record.
struct SingleFileRemovalPlan {
  ContentRemovalRequest request;
  std::string_view path;
};
static_assert(sizeof(SingleFileRemovalPlan) < 256);
inline bool removalPathEqualAscii(std::string_view value, std::string_view lower) {
  if (value.size() != lower.size()) return false;
  for (size_t at = 0; at < value.size(); ++at) {
    const unsigned char byte = value[at];
    if ((byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte) != static_cast<unsigned char>(lower[at])) return false;
  }
  return true;
}
inline bool validSingleFileRemovalPlan(const SingleFileRemovalPlan& plan) {
  if (!validContentRemovalRequest(plan.request) || !validInventoryPath(plan.path)) return false;
  ZipPathValidation grammar;
  ZipPathDetails details;
  if (!grammar.consume(std::span(reinterpret_cast<const uint8_t*>(plan.path.data() + 1), plan.path.size() - 1)) ||
      !grammar.finish(false, details))
    return false;
  static constexpr std::string_view PRIVATE_ROOT = "/.crosspoint";
  if (plan.path.size() >= PRIVATE_ROOT.size() &&
      removalPathEqualAscii(plan.path.substr(0, PRIVATE_ROOT.size()), PRIVATE_ROOT) &&
      (plan.path.size() == PRIVATE_ROOT.size() || plan.path[PRIVATE_ROOT.size()] == '/'))
    return false;
  const auto suffix = [&plan](std::string_view extension) {
    return plan.path.size() > extension.size() &&
           removalPathEqualAscii(plan.path.substr(plan.path.size() - extension.size()), extension);
  };
  if (plan.request.manifest.kind == ContentKind::Epub) return suffix(".epub");
  if (plan.request.manifest.kind != ContentKind::Font) return false;
  const bool fontRoot = (plan.path.size() > 7 && removalPathEqualAscii(plan.path.substr(0, 7), "/fonts/")) ||
                        (plan.path.size() > 8 && removalPathEqualAscii(plan.path.substr(0, 8), "/.fonts/"));
  return fontRoot && (plan.request.manifest.formatVersion == 1 ? suffix(".cpfont")
                                                               : suffix(".ttf") || suffix(".otf") || suffix(".ttc"));
}
inline size_t encodeSingleFileRemovalPlan(const SingleFileRemovalPlan& plan, std::span<uint8_t> output) {
  const size_t length = SINGLE_FILE_REMOVAL_PLAN_PREFIX + plan.path.size() + 4;
  if (!validSingleFileRemovalPlan(plan) || output.size() < length) return 0;
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'S', 'F', 1, 0, 0, 0};
  std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
  if (!encodeContentRemovalRequest(plan.request, output.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE))) return 0;
  inventory_detail::write(output, 123, plan.path.size(), 2);
  std::copy(plan.path.begin(), plan.path.end(), output.begin() + SINGLE_FILE_REMOVAL_PLAN_PREFIX);
  inventory_detail::write(output, length - 4, inventoryIndexCrc(output.first(length - 4)), 4);
  return length;
}
inline bool decodeSingleFileRemovalPlan(std::span<const uint8_t> bytes, SingleFileRemovalPlan& output) {
  static constexpr std::array<uint8_t, 8> PREFIX = {'L', 'R', 'S', 'F', 1, 0, 0, 0};
  if (bytes.size() < SINGLE_FILE_REMOVAL_PLAN_PREFIX + 6 || bytes.size() > SINGLE_FILE_REMOVAL_PLAN_MAX_SIZE ||
      !std::equal(PREFIX.begin(), PREFIX.end(), bytes.begin()) ||
      inventory_detail::read(bytes, bytes.size() - 4, 4) != inventoryIndexCrc(bytes.first(bytes.size() - 4)))
    return false;
  const size_t length = inventory_detail::read(bytes, 123, 2);
  if (bytes.size() != SINGLE_FILE_REMOVAL_PLAN_PREFIX + length + 4) return false;
  SingleFileRemovalPlan parsed;
  if (!decodeContentRemovalRequest(bytes.subspan(8, CONTENT_REMOVAL_REQUEST_SIZE), parsed.request)) return false;
  parsed.path = {reinterpret_cast<const char*>(bytes.data() + SINGLE_FILE_REMOVAL_PLAN_PREFIX), length};
  if (!validSingleFileRemovalPlan(parsed)) return false;
  output = parsed;
  return true;
}
}  // namespace companion
