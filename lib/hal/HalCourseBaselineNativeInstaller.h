#pragma once

#if LILA_TINTA

#include "CompanionCourseBaselineTransfer.h"
#include "HalCourseBaselineArchiveSession.h"
#include "HalCourseBaselinePublicationStore.h"
#include "HalCourseBaselineTransferInstaller.h"

namespace companion {
// Retain the admitted owner off stack for serialized transfer/recovery. Caller
// closes activities, proves journal readiness and excludes all other writers.
class HalCourseBaselineNativeInstaller final : public HalCourseBaselineTransferInstaller {
 public:
  using Permission = bool (*)(void*);
  // Checks item compatibility against immutable reviewed evidence. It must not
  // mutate state or require fresh approval during an Installing retry.
  using Compatibility = bool (*)(void*, const CourseBaselinePublicationRecord&, const char*, std::span<uint8_t>);
  HalCourseBaselineNativeInstaller(const Identity& reader, const Identity& generation, const Identity& owner,
                                   std::span<uint8_t> scratch, Permission permitted, void* permissionContext,
                                   Compatibility compatible, void* compatibilityContext)
      : reader(reader),
        generation(generation),
        owner(owner),
        scratch(scratch),
        permitted(permitted),
        permissionContext(permissionContext),
        compatible(compatible),
        compatibilityContext(compatibilityContext),
        consent(reader, generation, owner, scratch, allowed, this),
        archive(reader, generation, owner, scratch, parser, allowed, this),
        publication(reader, generation, owner, scratch, allowed, this),
        metadataReader(allowed, this) {}
  ~HalCourseBaselineNativeInstaller() { close(); }
  HalCourseBaselineNativeInstaller(const HalCourseBaselineNativeInstaller&) = delete;
  HalCourseBaselineNativeInstaller& operator=(const HalCourseBaselineNativeInstaller&) = delete;
  bool prepare(const char* destination, const char* candidate, const ContentManifest& manifest,
               const TransferState& state, std::span<uint8_t> workspace) override {
    if (operating) return failure("reentry");
    if (!candidate || std::strcmp(candidate, TRANSFER_STAGE) != 0 || state.phase != TransferPhase::Receiving ||
        !bind(destination, manifest, state, workspace))
      return failure("preparation context");
    operating = true;
    const bool prepared =
        load() && source(TRANSFER_STAGE) && compatibility(TRANSFER_STAGE) && archive.verifyPrepared(selected, false);
    return finish(prepared, "preparation");
  }
  bool metadata(const char* destination, const ContentManifest& manifest, const TransferState& state,
                std::span<uint8_t> workspace) override {
    if (operating) return failure("reentry");
    if ((state.phase != TransferPhase::Installing && state.phase != TransferPhase::Committed) ||
        !bind(destination, manifest, state, workspace))
      return failure("metadata context");
    operating = true;
    bool installed = load();
    if (installed && current.phase == TransferPhase::Committed) {
      installed = phase(CourseBaselinePublicationPhase::Prepared) && phase(CourseBaselinePublicationPhase::Published) &&
                  archive.verify(selected);
    } else if (installed) {
      installed = source(COURSE_BASELINE_DESTINATION) && compatibility(COURSE_BASELINE_DESTINATION) &&
                  publication.publish(selected, {this, verifyPrepared, publishArchive, verifyPublished}) ==
                      CourseBaselinePublicationResult::Ok &&
                  publication.published();
    }
    return finish(installed, "metadata");
  }
  bool close() {
    parser.close();
    const bool sourceClosed = !sourceReader.isOpen() || sourceReader.close();
    const bool consentClosed = consent.close(), archiveClosed = archive.close();
    const bool publicationClosed = publication.close(), metadataClosed = metadataReader.closeReaders();
    return sourceClosed && consentClosed && archiveClosed && publicationClosed && metadataClosed;
  }

 private:
  Identity reader, generation, owner;
  std::span<uint8_t> scratch;
  Permission permitted;
  void* permissionContext;
  Compatibility compatible;
  void* compatibilityContext;
  tinta::core::pack::Pack parser;
  HalCourseBaselineImportConsentStore consent;
  HalCourseBaselineArchiveSession archive;
  HalCourseBaselinePublicationStore publication;
  HalCourseRemovalMetadata metadataReader;
  HalFile sourceReader;
  ContentManifest expected;
  TransferState current;
  CourseBaselinePublicationRecord selected, observed;
  std::array<char, 96> phasePath{};
  bool operating = false;
  bool guard() const { return permitted && permitted(permissionContext) && admitCompanionHeap(); }
  static bool allowed(void* context) { return static_cast<HalCourseBaselineNativeInstaller*>(context)->guard(); }
  bool bind(const char* destination, const ContentManifest& manifest, const TransferState& state,
            std::span<uint8_t> workspace) {
    if (!destination || std::strcmp(destination, COURSE_BASELINE_DESTINATION) != 0 || !compatible ||
        workspace.data() != scratch.data() || workspace.size() != scratch.size() ||
        scratch.size() < COURSE_BASELINE_REVIEW_MAX_SIZE + 512 ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) || reader == Identity{} ||
        generation == Identity{} || owner == Identity{} || state.transaction == Identity{} || state.owner != owner ||
        state.storageGeneration != generation || state.durableOffset != state.length ||
        !matchesTransferManifest(manifest, state))
      return false;
    current = state;
    expected = manifest;
    return true;
  }
  bool load() {
    selected.reader = reader;
    selected.phase = CourseBaselinePublicationPhase::Prepared;
    return guard() && consent.load(current.transaction, selected.request) == CourseBaselineConsentResult::Ok &&
           selected.request.transaction == current.transaction && selected.request.generation == generation &&
           selected.request.owner == owner && selected.request.manifest == expected && guard();
  }
  bool source(const char* path) {
    uint64_t size = 0, length = 0;
    Digest hash{};
    if (!guard() || metadataReader.stat(path, size) != FileStatus::Present || size != expected.length || !guard() ||
        !Storage.openFileForReadReusing("COMPANION", path, sourceReader))
      return false;
    const bool matched = !sourceReader.isDirectory() &&
                         hashInventoryFile(sourceReader, scratch, length, hash, allowed, this) && sourceReader.sync();
    const bool closed = sourceReader.close();
    if (!matched || !closed || !guard() || length != expected.length || hash != expected.contentHash) return false;
    CourseCandidateDetails details;
    return validateStagedCourse(path, parser, scratch, details) && details.major == expected.formatVersion && guard();
  }
  bool compatibility(const char* path) {
    return guard() && compatible && compatible(compatibilityContext, selected, path, scratch) && guard();
  }
  bool phase(CourseBaselinePublicationPhase role) {
    static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-baseline-";
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::copy(PREFIX.begin(), PREFIX.end(), phasePath.begin());
    size_t at = PREFIX.size();
    for (const auto byte : current.transaction) {
      phasePath[at++] = HEX_DIGITS[byte >> 4];
      phasePath[at++] = HEX_DIGITS[byte & 15];
    }
    const char* suffix = role == CourseBaselinePublicationPhase::Prepared ? ".prepared.tmp" : ".published.tmp";
    std::copy_n(suffix, std::strlen(suffix) + 1, phasePath.begin() + at);
    uint64_t size = 0;
    if (!guard() || metadataReader.stat(phasePath.data(), size) != FileStatus::Missing) return false;
    suffix = role == CourseBaselinePublicationPhase::Prepared ? ".prepared" : ".published";
    std::copy_n(suffix, std::strlen(suffix) + 1, phasePath.begin() + at);
    if (!guard() || metadataReader.stat(phasePath.data(), size) != FileStatus::Present ||
        size != COURSE_BASELINE_PUBLICATION_SIZE || !guard() ||
        !metadataReader.read(phasePath.data(), 0, scratch.first(COURSE_BASELINE_PUBLICATION_SIZE)) || !guard())
      return false;
    return decodeCourseBaselinePublicationRecord(scratch.first(COURSE_BASELINE_PUBLICATION_SIZE), observed) &&
           observed.reader == selected.reader && observed.request == selected.request && observed.phase == role &&
           guard();
  }
  static bool verifyPrepared(void* context, const CourseBaselinePublicationRecord& request, bool recovering) {
    return static_cast<HalCourseBaselineNativeInstaller*>(context)->archive.verifyPrepared(request, recovering);
  }
  static bool publishArchive(void* context, const CourseBaselinePublicationRecord& request) {
    return static_cast<HalCourseBaselineNativeInstaller*>(context)->archive.publishArchive(request,
                                                                                           COURSE_BASELINE_DESTINATION);
  }
  static bool verifyPublished(void* context, const CourseBaselinePublicationRecord& request) {
    return static_cast<HalCourseBaselineNativeInstaller*>(context)->archive.verify(request);
  }
  bool finish(bool success, const char* operation) {
    const bool closed = close();
    operating = false;
    return (success && closed && guard()) || failure(operation);
  }
  static bool failure(const char* operation) {
    LOG_ERR("COMPANION", "Baseline transfer installer %s refused", operation);
    return false;
  }
};
inline std::unique_ptr<HalCourseBaselineTransferInstaller> createHalCourseBaselineNativeInstaller(
    const Identity& reader, const Identity& generation, const Identity& owner, std::span<uint8_t> scratch,
    HalCourseBaselineNativeInstaller::Permission permitted, void* permissionContext,
    HalCourseBaselineNativeInstaller::Compatibility compatible, void* compatibilityContext) {
  if (!permitted || !permitted(permissionContext) || !compatible ||
      !admitCompanionHeap(sizeof(HalCourseBaselineNativeInstaller), sizeof(HalCourseBaselineNativeInstaller))) {
    LOG_ERR("COMPANION", "Native baseline installer admission or compatibility unavailable");
    return nullptr;
  }
  auto installer = makeUniqueNoThrow<HalCourseBaselineNativeInstaller>(
      reader, generation, owner, scratch, permitted, permissionContext, compatible, compatibilityContext);
  if (!installer) LOG_ERR("COMPANION", "OOM: native baseline transfer installer");
  return installer;
}
}  // namespace companion

#endif  // LILA_TINTA
