#include <gtest/gtest.h>
#include <mdns.h>

#include <string>

#include "lib/hal/HalCompanionWifiDiscovery.h"

unsigned wifiDiscoveryErrors = 0;
namespace {
unsigned starts, stops, hosts, services;
unsigned failure;
std::string hostname, instance;
uint16_t publishedPort;
companion::WifiHandoffOffer offer() {
  companion::WifiHandoffOffer value;
  value.reader.fill(1);
  value.storageGeneration.fill(2);
  value.installation.fill(3);
  value.transaction.fill(4);
  value.session.fill(5);
  value.key.fill(6);
  value.port = 8080;
  value.lifetimeSeconds = 30;
  return value;
}
void reset(unsigned fail = 0) {
  starts = stops = hosts = services = wifiDiscoveryErrors = 0;
  failure = fail;
  hostname.clear();
  instance.clear();
  publishedPort = 0;
}
}  // namespace
esp_err_t mdns_init() {
  ++starts;
  return failure == 1 ? -1 : ESP_OK;
}
void mdns_free() { ++stops; }
esp_err_t mdns_hostname_set(const char* name) {
  ++hosts;
  hostname = name;
  return failure == 2 ? -1 : ESP_OK;
}
esp_err_t mdns_service_add(const char* name, const char* service, const char* proto, uint16_t port,
                           mdns_txt_item_t* txt, size_t count) {
  ++services;
  instance = name;
  publishedPort = port;
  EXPECT_STREQ(service, "_lila-sync");
  EXPECT_STREQ(proto, "_tcp");
  EXPECT_EQ(txt, nullptr);
  EXPECT_EQ(count, 0u);
  return failure == 3 ? -1 : ESP_OK;
}
TEST(HalCompanionWifiDiscoveryTest, PublicationRestartAndDestruction) {
  reset();
  {
    companion::HalCompanionWifiDiscovery discovery;
    ASSERT_TRUE(discovery.begin(offer()));
    EXPECT_EQ(hostname, "lila-05050505050505050505050505050505");
    EXPECT_EQ(instance, hostname);
    EXPECT_EQ(publishedPort, 8080);
    auto next = offer();
    next.session[0] = 0xab;
    ASSERT_TRUE(discovery.begin(next));
    EXPECT_EQ(stops, 1u);
    EXPECT_EQ(hostname, "lila-ab050505050505050505050505050505");
  }
  EXPECT_EQ(starts, 2u);
  EXPECT_EQ(stops, 2u);
  EXPECT_EQ(wifiDiscoveryErrors, 0u);
}
TEST(HalCompanionWifiDiscoveryTest, FailureCleanupAndRetry) {
  for (unsigned fail = 1; fail <= 3; ++fail) {
    reset(fail);
    companion::HalCompanionWifiDiscovery discovery;
    EXPECT_FALSE(discovery.begin(offer()));
    EXPECT_EQ(stops, fail == 1 ? 0u : 1u);
    EXPECT_EQ(wifiDiscoveryErrors, 1u);
    discovery.end();
    EXPECT_EQ(stops, fail == 1 ? 0u : 1u);
    failure = 0;
    ASSERT_TRUE(discovery.begin(offer()));
    auto invalid = offer();
    invalid.session.fill(0);
    EXPECT_FALSE(discovery.begin(invalid));
    EXPECT_EQ(starts, 2u);
    EXPECT_EQ(stops, fail == 1 ? 1u : 2u);
  }
}
