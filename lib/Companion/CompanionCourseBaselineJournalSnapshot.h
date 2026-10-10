#pragma once

#include "CompanionCourseBaselineReview.h"
#include "CompanionTintaJournal.h"
#include "CompanionTransfer.h"

namespace companion {
enum class CourseBaselineJournalSnapshotResult { Ok, Missing, Invalid, Busy, IoError };
// Admit off stack. Caller excludes backup writers and lends scratch only during
// open/reopen. Reads use verified immutable copies; native recovery cannot mutate them.
class CourseBaselineJournalSnapshot final : public TintaJournalStorage {
 public:
  using Permission = bool (*)(void*);
  using Hash = bool (*)(void*, std::span<const uint8_t>, Digest&);
  CourseBaselineJournalSnapshot(TransferStorage& storage, Permission permitted, void* context, Hash hash,
                                void* hashContext)
      : storage(storage), permitted(permitted), context(context), hash(hash), hashContext(hashContext) {}
  CourseBaselineJournalSnapshotResult open(std::span<const uint8_t> review, const Digest& expected,
                                           std::span<uint8_t> scratch) {
    if (operating) return CourseBaselineJournalSnapshotResult::Busy;
    operating = true;
    struct Operation {
      bool& operating;
      ~Operation() { operating = false; }
    } operation{operating};
    ready = captured = false;
    CourseBaselineReviewView view;
    if (!guard()) return CourseBaselineJournalSnapshotResult::Busy;
    if (scratch.empty() || !hash ||
        course_baseline_detail::overlaps(review.data(), review.size(), this, sizeof(*this)) ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)) || !view.decode(review))
      return CourseBaselineJournalSnapshotResult::Invalid;
    if (!hash(hashContext, review, computed) || computed != expected)
      return CourseBaselineJournalSnapshotResult::Invalid;
    const bool present = review[5] != 0;
    unsigned copied = 0;
    for (size_t index = 0; index < view.count(); ++index) {
      const auto entry = view.entry(index);
      if (entry[0] != uint8_t(CourseBaselineReviewDomain::Journal)) continue;
      auto& file = files[copied++];
      file.present = entry[1] != 0;
      file.length = static_cast<uint32_t>(course_review_detail::number(entry, 28, 8));
      std::copy_n(entry.begin() + 36, file.hash.size(), file.hash.begin());
      path(file.path, expected, index);
    }
    if (copied != files.size()) return CourseBaselineJournalSnapshotResult::Invalid;
    // All borrowed review fields are copied before verification can reuse scratch.
    const auto verified = verify(scratch);
    if (verified != CourseBaselineJournalSnapshotResult::Ok) return verified;
    ready = captured = present;
    return present ? CourseBaselineJournalSnapshotResult::Ok : CourseBaselineJournalSnapshotResult::Missing;
  }
  CourseBaselineJournalSnapshotResult reopen(std::span<uint8_t> scratch) {
    if (operating) return CourseBaselineJournalSnapshotResult::Busy;
    ready = false;
    if (!guard()) return CourseBaselineJournalSnapshotResult::Busy;
    if (!captured || scratch.empty() ||
        course_baseline_detail::overlaps(scratch.data(), scratch.size(), this, sizeof(*this)))
      return CourseBaselineJournalSnapshotResult::Invalid;
    operating = true;
    const auto result = verify(scratch);
    operating = false;
    ready = result == CourseBaselineJournalSnapshotResult::Ok;
    return result;
  }
  void close() { ready = false; }
  bool size(uint32_t& bytes) override {
    if (!checked(files[0])) return false;
    bytes = files[0].length;
    return true;
  }
  bool read(uint32_t offset, std::span<uint8_t> bytes) override {
    const auto& file = files[0];
    return checked(file) && offset <= file.length && bytes.size() <= file.length - offset &&
           storage.read(file.path.data(), offset, bytes) && guard();
  }
  bool readHeader(uint8_t slot, std::span<uint8_t> bytes, size_t& length) override {
    if (slot >= 2 || !ready || !guard()) return false;
    const auto& file = files[slot + 1];
    if (!file.present) {
      uint64_t ignored = 0;
      if (storage.stat(file.path.data(), ignored) != FileStatus::Missing || !guard()) return false;
      length = 0;
      return true;
    }
    if (!checked(file) ||
        !storage.read(file.path.data(), 0, bytes.first(std::min<size_t>(bytes.size(), file.length))) || !guard())
      return false;
    length = file.length;
    return true;
  }
  bool write(uint32_t, std::span<const uint8_t>) override { return false; }
  bool truncate(uint32_t) override { return false; }
  bool writeHeader(uint8_t, std::span<const uint8_t>) override { return false; }
  bool digest(std::span<const uint8_t> bytes, Digest& output) override {
    return ready && guard() && hash && hash(hashContext, bytes, output) && guard();
  }

 private:
  CourseBaselineJournalSnapshotResult verify(std::span<uint8_t> scratch) {
    for (const auto& file : files) {
      uint64_t length = 0;
      if (!guard()) return CourseBaselineJournalSnapshotResult::Busy;
      const auto status = storage.stat(file.path.data(), length);
      if (file.present ? status != FileStatus::Present || length != file.length ||
                             !storage.verify(file.path.data(), file.length, file.hash, scratch)
                       : status != FileStatus::Missing)
        return CourseBaselineJournalSnapshotResult::IoError;
    }
    if (!guard()) return CourseBaselineJournalSnapshotResult::Busy;
    return CourseBaselineJournalSnapshotResult::Ok;
  }
  struct File {
    std::array<char, 112> path{};
    Digest hash{};
    uint32_t length = 0;
    bool present = false;
  };
  TransferStorage& storage;
  Permission permitted;
  void* context;
  Hash hash;
  void* hashContext;
  std::array<File, 3> files{};
  Digest computed{};
  bool ready = false, operating = false, captured = false;
  bool guard() const { return permitted && permitted(context); }
  bool checked(const File& file) {
    uint64_t length = 0;
    return ready && file.present && guard() && storage.stat(file.path.data(), length) == FileStatus::Present &&
           length == file.length && guard();
  }
  static void path(std::array<char, 112>& output, const Digest& hash, size_t index) {
    static constexpr std::string_view PREFIX = "/.crosspoint/companion/course-review-state-";
    static constexpr char DIGITS[] = "0123456789abcdef";
    static_assert(PREFIX.size() + 64 + 4 <= 112);
    std::copy(PREFIX.begin(), PREFIX.end(), output.begin());
    size_t at = PREFIX.size();
    for (const auto byte : hash) {
      output[at++] = DIGITS[byte >> 4];
      output[at++] = DIGITS[byte & 15];
    }
    output[at++] = '-';
    output[at++] = DIGITS[(index >> 4) & 15];
    output[at++] = DIGITS[index & 15];
    output[at] = 0;
  }
};
}  // namespace companion
