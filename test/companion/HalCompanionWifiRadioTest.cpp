#include <esp_event.h>
#include <esp_wifi_default.h>
#include <gtest/gtest.h>

#include <cstring>

#include "HalCompanionWifiRadio.h"

using namespace companion;
namespace {
enum Step {
  None,
  NetInit,
  Loop,
  Allocate,
  Attach,
  Handlers,
  Init,
  Storage,
  Mode,
  Config,
  Start,
  Connect,
  Stop,
  Deinit,
  Detach,
  DeleteLoop
};
Step failed;
bool foreign, sharedLoop;
unsigned netifs, loops, drivers, stops, connections, publications, detaches;
wifi_config_t saved;
wifi_interface_t selected;
esp_netif_t interface;
std::array<uint8_t, 4> ip;
int result(Step step) { return failed == step ? -77 : ESP_OK; }
void reset() {
  failed = None;
  foreign = sharedLoop = false;
  netifs = loops = drivers = stops = connections = publications = detaches = 0;
  saved = {};
  ip = {192, 168, 4, 1};
}
}  // namespace
esp_err_t esp_wifi_get_mode(wifi_mode_t*) { return foreign ? ESP_OK : ESP_ERR_WIFI_NOT_INIT; }
esp_err_t esp_netif_init() { return result(NetInit); }
esp_err_t esp_event_loop_create_default() {
  if (sharedLoop) return ESP_ERR_INVALID_STATE;
  if (result(Loop) == ESP_OK) ++loops;
  return result(Loop);
}
esp_err_t esp_event_loop_delete_default() {
  if (result(DeleteLoop) == ESP_OK) --loops;
  return result(DeleteLoop);
}
esp_netif_t* esp_netif_new(const esp_netif_config_t*) {
  if (failed == Allocate) return nullptr;
  ++netifs;
  return &interface;
}
void esp_netif_destroy(esp_netif_t*) { --netifs; }
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t*) { return result(Attach); }
esp_err_t esp_netif_attach_wifi_station(esp_netif_t*) { return result(Attach); }
esp_err_t esp_wifi_set_default_wifi_ap_handlers() { return result(Handlers); }
esp_err_t esp_wifi_set_default_wifi_sta_handlers() { return result(Handlers); }
esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(void*) {
  ++detaches;
  return result(Detach);
}
esp_err_t esp_wifi_init(const wifi_init_config_t* config) {
  EXPECT_FALSE(config->nvs_enable);
  if (result(Init) == ESP_OK) ++drivers;
  return result(Init);
}
esp_err_t esp_wifi_deinit() {
  if (result(Deinit) == ESP_OK) --drivers;
  return result(Deinit);
}
esp_err_t esp_wifi_stop() {
  ++stops;
  return result(Stop);
}
esp_err_t esp_wifi_start() { return result(Start); }
esp_err_t esp_wifi_connect() {
  ++connections;
  return result(Connect);
}
esp_err_t esp_wifi_set_storage(int storage) {
  EXPECT_EQ(storage, WIFI_STORAGE_RAM);
  return result(Storage);
}
esp_err_t esp_wifi_set_mode(wifi_mode_t) { return result(Mode); }
esp_err_t esp_wifi_set_config(wifi_interface_t type, const wifi_config_t* config) {
  ++publications;
  selected = type;
  saved = *config;
  return result(Config);
}
esp_err_t esp_netif_get_ip_info(esp_netif_t*, esp_netif_ip_info_t* info) {
  std::memcpy(&info->ip.addr, ip.data(), ip.size());
  return ESP_OK;
}
TEST(HalCompanionWifiRadioTest, HotspotUsesRamWpa2SingleClientAndReleasesAllResources) {
  reset();
  HalCompanionWifiRadio radio;
  ASSERT_TRUE(radio.begin(WifiNetworkMode::Hotspot, "lila-test", "0123456789abcdef"));
  EXPECT_EQ(selected, WIFI_IF_AP);
  EXPECT_EQ(saved.ap.ssid_len, 9);
  EXPECT_EQ(saved.ap.authmode, WIFI_AUTH_WPA2_PSK);
  EXPECT_EQ(saved.ap.max_connection, 1);
  EXPECT_EQ(connections, 0u);
  std::array<uint8_t, 4> address{};
  EXPECT_TRUE(radio.address(address));
  EXPECT_EQ(address, ip);
  EXPECT_FALSE(radio.begin(WifiNetworkMode::Hotspot, "other", "0123456789abcdef"));
  ASSERT_TRUE(radio.end());
  EXPECT_FALSE(radio.ownsResources());
  EXPECT_EQ(netifs + loops + drivers, 0u);
  EXPECT_TRUE(radio.end());
  EXPECT_EQ(stops, 1u);
}
TEST(HalCompanionWifiRadioTest, SavedNetworkAcceptsFullSsidAndRawPskWithoutTerminatorOverrun) {
  reset();
  HalCompanionWifiRadio radio;
  const std::string ssid(32, 's'), password(64, 'a');
  ASSERT_TRUE(radio.begin(WifiNetworkMode::SavedNetwork, ssid, password));
  EXPECT_EQ(selected, WIFI_IF_STA);
  EXPECT_EQ(connections, 1u);
  EXPECT_EQ(saved.sta.scan_method, WIFI_ALL_CHANNEL_SCAN);
  EXPECT_EQ(saved.sta.sort_method, WIFI_CONNECT_AP_BY_SIGNAL);
  EXPECT_EQ(std::memcmp(saved.sta.ssid, ssid.data(), 32), 0);
  EXPECT_EQ(std::memcmp(saved.sta.password, password.data(), 64), 0);
  ip.fill(0);
  std::array<uint8_t, 4> address{9, 9, 9, 9};
  EXPECT_FALSE(radio.address(address));
  EXPECT_EQ(address, (std::array<uint8_t, 4>{9, 9, 9, 9}));
}
TEST(HalCompanionWifiRadioTest, StartupFailuresReleaseOwnedResources) {
  for (int step = NetInit; step <= Connect; ++step) {
    reset();
    failed = static_cast<Step>(step);
    HalCompanionWifiRadio radio;
    EXPECT_FALSE(radio.begin(WifiNetworkMode::SavedNetwork, "network", "password")) << step;
    EXPECT_FALSE(radio.ownsResources()) << step;
    EXPECT_EQ(netifs + loops + drivers, 0u) << step;
  }
}
TEST(HalCompanionWifiRadioTest, TeardownFailuresRetainOwnershipForRetry) {
  for (Step step : {Stop, Deinit, Detach, DeleteLoop}) {
    reset();
    HalCompanionWifiRadio radio;
    ASSERT_TRUE(radio.begin(WifiNetworkMode::Hotspot, "lila", "0123456789abcdef"));
    failed = step;
    EXPECT_FALSE(radio.end());
    EXPECT_TRUE(radio.ownsResources());
    EXPECT_FALSE(radio.begin(WifiNetworkMode::Hotspot, "lila", "0123456789abcdef"));
    failed = None;
    EXPECT_TRUE(radio.end());
    EXPECT_EQ(netifs + loops + drivers, 0u);
    EXPECT_EQ(detaches, 1u);
  }
}
TEST(HalCompanionWifiRadioTest, SharedLoopAndForeignWifiAreNeverReleased) {
  reset();
  foreign = true;
  HalCompanionWifiRadio radio;
  EXPECT_FALSE(radio.begin(WifiNetworkMode::SavedNetwork, "network", "password"));
  EXPECT_EQ(netifs + loops + drivers, 0u);
  foreign = false;
  sharedLoop = true;
  ASSERT_TRUE(radio.begin(WifiNetworkMode::SavedNetwork, "network", ""));
  EXPECT_TRUE(radio.end());
  EXPECT_EQ(loops, 0u);
}

TEST(HalCompanionWifiRadioTest, InvalidCredentialsNeverAllocateAndDestructionReleasesOwnership) {
  reset();
  {
    HalCompanionWifiRadio radio;
    EXPECT_FALSE(radio.begin(WifiNetworkMode::Hotspot, "lila", ""));
    EXPECT_FALSE(radio.begin(WifiNetworkMode::Hotspot, "lila", "short"));
    EXPECT_FALSE(radio.begin(WifiNetworkMode::SavedNetwork, std::string_view("a\0b", 3), "password"));
    EXPECT_FALSE(radio.begin(WifiNetworkMode::SavedNetwork, "lila", std::string_view("a\0b", 3)));
    EXPECT_EQ(netifs + loops + drivers, 0u);
    ASSERT_TRUE(radio.begin(WifiNetworkMode::Hotspot, "lila", "0123456789abcdef"));
  }
  EXPECT_EQ(netifs + loops + drivers, 0u);
}
