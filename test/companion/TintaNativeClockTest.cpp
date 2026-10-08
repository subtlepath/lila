#include <HalClock.h>
#include <gtest/gtest.h>

#include "lib/hal/HalTintaMutationClock.h"
#include "lib/hal/HalTintaProgressMutationContext.h"
#include "platform/Clock.h"
#include "platform/Log.h"

HalClock halClock;
namespace tinta::platform {
void log(const char*, ...) {}
}  // namespace tinta::platform

TEST(TintaNativeClock, AbsoluteUtcRequiresRtcTimeAndPreservesUnknownOutput) {
  tinta::platform::Clock clock;
  uint64_t timestamp = 42;
  clock.begin(false);
  uint32_t day = 123;
  companion::ClockQuality quality = companion::ClockQuality::Trusted;
  EXPECT_FALSE(companion::readTintaMutationClock(clock, day, timestamp, quality));
  EXPECT_EQ(day, 123U);
  EXPECT_EQ(timestamp, 42U);
  EXPECT_EQ(quality, companion::ClockQuality::Trusted);
  EXPECT_FALSE(clock.unixUtc(timestamp));
  EXPECT_EQ(timestamp, 42U);
  halClock.local.tm_year = 2026 - 1900;
  halClock.local.tm_mon = 9;
  halClock.local.tm_mday = 8;
  halClock.local.tm_hour = 15;
  halClock.localAvailable = true;
  // This UTC snapshot differs from local wall time: do not reinterpret it.
  halClock.utc = 1791464400;
  halClock.utcAvailable = true;
  clock.begin(true);
  clock.configure(4, tinta::platform::kFirmwareDay);
  ASSERT_TRUE(clock.unixUtc(timestamp));
  EXPECT_EQ(timestamp, halClock.utc);
  ASSERT_TRUE(companion::readTintaMutationClock(clock, day, timestamp, quality));
  EXPECT_EQ(day, clock.today());
  EXPECT_EQ(timestamp, halClock.utc);
  EXPECT_EQ(quality, companion::ClockQuality::Device);
  // A snapshot's recorded study day must also reject an older RTC date.
  clock.configure(4, static_cast<tinta::core::DayNumber>(clock.today() + 1));
  EXPECT_FALSE(clock.trusted());
  EXPECT_FALSE(companion::readTintaMutationClock(clock, day, timestamp, quality));
  clock.configure(4, tinta::platform::kFirmwareDay);
  ASSERT_TRUE(clock.trusted());
  tinta::core::Profile profile;
  companion::HalTintaProgressMutationContext context(profile, clock);
  companion::TintaSchedulerConfiguration configuration;
  ASSERT_TRUE(context.capture(configuration, quality));
  tinta::core::JournalEntry entry;
  entry.day = clock.today();
  ASSERT_TRUE(context.timestamp(entry, quality, timestamp));
  EXPECT_EQ(timestamp, halClock.utc);
  timestamp = 42;
  EXPECT_FALSE(context.timestamp(entry, quality, timestamp));
  EXPECT_EQ(timestamp, 42U);
  ASSERT_TRUE(context.capture(configuration, quality));
  ++entry.day;
  EXPECT_FALSE(context.timestamp(entry, quality, timestamp));
  EXPECT_EQ(timestamp, 42U);
  entry.day = clock.today();
  profile.retentionPermille = 950;
  ASSERT_TRUE(context.capture(configuration, quality));
  EXPECT_EQ(configuration.retentionBasisPoints, 9500U);
  ASSERT_TRUE(context.timestamp(entry, quality, timestamp));
  halClock.utcAvailable = false;
  timestamp = 42;
  const auto previousDay = day;
  const auto previousQuality = quality;
  EXPECT_FALSE(companion::readTintaMutationClock(clock, day, timestamp, quality));
  EXPECT_EQ(day, previousDay);
  EXPECT_EQ(timestamp, 42U);
  EXPECT_EQ(quality, previousQuality);
  EXPECT_FALSE(clock.unixUtc(timestamp));
  EXPECT_EQ(timestamp, 42U);
  halClock.utcAvailable = true;
  halClock.local.tm_year = 2020 - 1900;
  EXPECT_FALSE(clock.unixUtc(timestamp));
  EXPECT_EQ(timestamp, 42U);
  halClock.local.tm_year = 2026 - 1900;
  clock.confirmDay(tinta::platform::kFirmwareDay);
  EXPECT_TRUE(clock.trusted());
  EXPECT_FALSE(clock.hasTimeOfDay());
  ASSERT_TRUE(companion::readTintaMutationClock(clock, day, timestamp, quality));
  EXPECT_EQ(day, tinta::platform::kFirmwareDay);
  EXPECT_EQ(timestamp, 0U);
  EXPECT_EQ(quality, companion::ClockQuality::Unknown);
  timestamp = 42;
  EXPECT_FALSE(clock.unixUtc(timestamp));
  EXPECT_EQ(timestamp, 42U);
}
