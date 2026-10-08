#include "HalCompanionWifiDiscovery.h"

#include <Logging.h>
#include <mdns.h>

namespace companion {
HalCompanionWifiDiscovery::~HalCompanionWifiDiscovery() { end(); }
void HalCompanionWifiDiscovery::end() {
  if (active) mdns_free();
  active = false;
}
bool HalCompanionWifiDiscovery::begin(const WifiHandoffOffer& offer) {
  end();
  char hostname[WIFI_HANDOFF_HOSTNAME_SIZE];
  if (!validWifiHandoffOffer(offer) || !wifiHandoffHostname(offer.session, hostname)) {
    LOG_ERR("CWIFI", "Invalid discovery offer");
    return false;
  }
  // SDK owns its task and record allocations; end() releases them as one session.
  if (mdns_init() != ESP_OK) {
    LOG_ERR("CWIFI", "Discovery initialization failed");
    return false;
  }
  active = true;
  if (mdns_hostname_set(hostname) != ESP_OK ||
      mdns_service_add(hostname, "_lila-sync", "_tcp", offer.port, nullptr, 0) != ESP_OK) {
    LOG_ERR("CWIFI", "Discovery publication failed");
    end();
    return false;
  }
  return true;
}
}  // namespace companion
