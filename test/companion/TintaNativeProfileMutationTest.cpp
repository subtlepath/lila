#include <HalClock.h>
#include <gtest/gtest.h>
#include <openssl/sha.h>

#include <vector>

#include "lib/hal/HalTintaProfileMutationContext.h"

HalClock halClock;
namespace tinta::platform {
void log(const char*, ...) {}
}  // namespace tinta::platform
namespace {
class JournalStorage final : public companion::TintaJournalStorage {
 public:
  std::vector<uint8_t> data;
  std::array<std::vector<uint8_t>, 2> headers;
  bool failWrite = false;
  JournalStorage() { data.reserve(4096); }
  bool size(uint32_t& output) override {
    output = data.size();
    return true;
  }
  bool read(uint32_t offset, std::span<uint8_t> output) override {
    if (offset > data.size() || output.size() > data.size() - offset) return false;
    std::copy_n(data.begin() + offset, output.size(), output.begin());
    return true;
  }
  bool write(uint32_t offset, std::span<const uint8_t> input) override {
    if (failWrite) return false;
    data.resize(offset + input.size());
    std::copy(input.begin(), input.end(), data.begin() + offset);
    return true;
  }
  bool truncate(uint32_t bytes) override {
    data.resize(bytes);
    return true;
  }
  bool readHeader(uint8_t slot, std::span<uint8_t> output, size_t& length) override {
    length = headers[slot].size();
    if (length > output.size()) return false;
    std::copy(headers[slot].begin(), headers[slot].end(), output.begin());
    return true;
  }
  bool writeHeader(uint8_t slot, std::span<const uint8_t> input) override {
    headers[slot].assign(input.begin(), input.end());
    return true;
  }
  bool digest(std::span<const uint8_t> input, companion::Digest& output) override {
    return SHA256(input.data(), input.size(), output.data()) != nullptr;
  }
};
class Identities final : public companion::IdentityStorage {
 public:
  std::array<uint8_t, companion::IDENTITY_RECORD_SIZE> binding{};
  companion::Identity marker{};
  bool saved = false;
  uint8_t random = 3;
  bool hardwareIdentity(companion::Identity& output) override {
    output.fill(1);
    return true;
  }
  bool cardIdentity(companion::Identity& output) override {
    output.fill(2);
    return true;
  }
  companion::IdentityRead readBinding(std::span<uint8_t> output) override {
    if (!saved) return companion::IdentityRead::Missing;
    std::copy(binding.begin(), binding.end(), output.begin());
    return companion::IdentityRead::Present;
  }
  bool writeBinding(std::span<const uint8_t> input) override {
    std::copy(input.begin(), input.end(), binding.begin());
    saved = true;
    return true;
  }
  companion::IdentityRead readMarker(companion::Identity& output) override {
    if (!marker[0]) return companion::IdentityRead::Missing;
    output = marker;
    return companion::IdentityRead::Present;
  }
  bool createMarker(const companion::Identity& input) override {
    marker = input;
    return true;
  }
  bool randomIdentity(companion::Identity& output) override {
    output.fill(random++);
    return true;
  }
};
}  // namespace
TEST(TintaNativeProfileMutation, UnconfirmedDateUsesUnknownEvidenceAndDeviceOnlyEditsEmitNothing) {
  using namespace companion;
  JournalStorage storage;
  std::array<uint8_t, 1024> scratch{};
  TintaJournal journal(storage, scratch);
  TintaWriter writer(journal, storage);
  Identities identities;
  ASSERT_EQ(writer.start(identities), TintaJournalResult::Ok);
  tinta::platform::Clock clock;
  clock.begin(false);
  clock.configure(4, 42);
  ASSERT_FALSE(clock.trusted());
  unsigned errors = 0;
  const auto report = [](void* context, TintaJournalResult) { ++*static_cast<unsigned*>(context); };
  HalTintaProfileMutationContext context(writer, clock, 42, &errors, report);
  tinta::core::Profile profile;
  ASSERT_TRUE(context.initialize(profile));
  profile.fullRefreshEvery = 17;
  ASSERT_TRUE(HalTintaProfileMutationContext::callback(&context, profile));
  EXPECT_EQ(journal.count(), 0U);
  profile.newPerDay = 20;
  ASSERT_TRUE(context.persist(profile));
  ASSERT_EQ(journal.read(0), TintaJournalResult::Ok);
  EXPECT_EQ(journal.event().clockQuality, ClockQuality::Unknown);
  EXPECT_EQ(journal.event().studyDay, 42U);
  EXPECT_EQ(journal.event().timestamp, 0U);
  EXPECT_EQ(journal.body()[4], 20);
  ASSERT_TRUE(context.persist(profile));
  EXPECT_EQ(journal.count(), 1U);
  halClock.local.tm_year = 2026 - 1900;
  halClock.local.tm_mon = 9;
  halClock.local.tm_mday = 8;
  halClock.local.tm_hour = 15;
  halClock.localAvailable = true;
  halClock.utc = 1791464400;
  halClock.utcAvailable = true;
  clock.begin(true);
  clock.configure(4, tinta::platform::kFirmwareDay);
  ASSERT_TRUE(clock.trusted());
  profile.retentionPermille = 950;
  ASSERT_TRUE(context.persist(profile));
  ASSERT_EQ(journal.read(1), TintaJournalResult::Ok);
  EXPECT_EQ(journal.event().clockQuality, ClockQuality::Device);
  EXPECT_EQ(journal.event().studyDay, clock.today());
  EXPECT_EQ(journal.event().timestamp, halClock.utc);
  profile.reviewCap = 200;
  storage.failWrite = true;
  EXPECT_FALSE(context.persist(profile));
  EXPECT_FALSE(writer.available());
  EXPECT_EQ(errors, 1U);
}
