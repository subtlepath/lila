#include "HalCompanionBluetooth.h"

#include <Logging.h>
#include <Memory.h>

#include "CompanionWifiSecrets.h"

#if LILA_COMPANION
#include <Esp.h>
#include <HalMemory.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>

#include "CompanionCommandQueue.h"
#include "CompanionControlWorkspaceLease.h"
#include "CompanionFrameAssembler.h"
#include "CompanionSession.h"

namespace {
static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS == 1);
static_assert(CONFIG_BT_NIMBLE_MAX_BONDS == companion::MAX_PAIRINGS);
constexpr size_t RX_SIZE = companion::FRAME_HEADER_SIZE + companion::MAX_CONTROL_PAYLOAD;
constexpr size_t INPUT_WORKSPACE_SIZE = companion::COMMAND_QUEUE_STORAGE_SIZE + RX_SIZE;
static_assert(INPUT_WORKSPACE_SIZE + 2 * RX_SIZE <= companion::SESSION_WORKSPACE_SIZE);
constexpr size_t REQUIRED_POST_START_HEAP = 50 * 1024;
// A conservative startup gate, not a claim about measured NimBLE consumption.
constexpr size_t MINIMUM_START_HEAP = 128 * 1024;
constexpr uint16_t NO_CONNECTION = 0xffff;
}  // namespace

struct HalCompanionBluetooth::Impl {
  struct Guard {
    SemaphoreHandle_t mutex;
    explicit Guard(SemaphoreHandle_t mutex) : mutex(mutex) { xSemaphoreTake(mutex, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(mutex); }
  };
  struct ServerCallbacks final : NimBLEServerCallbacks {
    Impl& owner;
    explicit ServerCallbacks(Impl& owner) : owner(owner) {}
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
      bool rejected;
      {
        Guard lock(owner.mutex);
        rejected = owner.workspaceLease.active() || !owner.session.connect();
        if (!rejected) {
          owner.connection = info.getConnHandle();
          owner.status = {State::Connected, 0, false};
          owner.queue.clear();
          owner.assembler.reset();
        }
      }
      if (rejected) server->disconnect(info.getConnHandle());
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
      bool restart = false;
      {
        Guard lock(owner.mutex);
        if (owner.connection == info.getConnHandle()) {
          owner.connection = NO_CONNECTION;
          owner.session.disconnect();
          owner.queue.clear();
          owner.assembler.reset();
          owner.status = {owner.running ? State::Advertising : State::Off, 0, false};
          restart = owner.running;
        }
      }
      if (restart) NimBLEDevice::startAdvertising();
    }
    uint32_t onPassKeyDisplay() override {
      const uint32_t code = esp_random() % 1000000;
      Guard lock(owner.mutex);
      owner.status.passkey = code;
      owner.status.showPasskey = true;
      return code;
    }
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
      const bool authorized = info.isEncrypted() && info.isAuthenticated() && info.isBonded();
      {
        Guard lock(owner.mutex);
        if (owner.connection != info.getConnHandle()) return;
        owner.session.authenticate(authorized);
        owner.status = {authorized ? State::Authenticated : State::Connected, 0, false};
        if (authorized) owner.peer = info.getIdAddress();
      }
      LOG_DBG("COMPANION", "NimBLE authentication callback stack watermark: %u",
              static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
      if (!authorized) NimBLEDevice::getServer()->disconnect(info.getConnHandle());
    }
  } callbacks;
  struct InputCallbacks final : NimBLECharacteristicCallbacks {
    Impl& owner;
    explicit InputCallbacks(Impl& owner) : owner(owner) {}
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
      bool rejected = false;
      {
        Guard lock(owner.mutex);
        if (!owner.running || owner.connection != info.getConnHandle() || owner.status.state != State::Authenticated ||
            !info.isEncrypted() || !info.isAuthenticated())
          return;
        if (owner.workspaceLease.active()) {
          rejected = true;
        } else {
          companion::FrameView frame;
          const auto result =
              owner.assembler.append({characteristic->getValueData(), characteristic->getLength()}, true, frame);
          if (result == companion::FrameAssembler::Result::Ready) {
            const size_t count = companion::FRAME_HEADER_SIZE + frame.payload.size();
            rejected = owner.queue.push(owner.receiveBuffer.first(count), true) !=
                       companion::CommandQueue::PushResult::Accepted;
            owner.assembler.reset();
          } else if (result == companion::FrameAssembler::Result::Rejected)
            rejected = true;
        }
      }
      if (rejected) {
        LOG_ERR("COMPANION", "BLE control input rejected");
        NimBLEDevice::getServer()->disconnect(info.getConnHandle());
      }
    }
  } inputCallbacks;
  StaticSemaphore_t mutexStorage{};
  SemaphoreHandle_t mutex;
  companion::CommandQueue queue;
  std::span<uint8_t> receiveBuffer;
  companion::FrameAssembler assembler;
  NimBLEServer* server = nullptr;
  NimBLECharacteristic* output = nullptr;
  NimBLEAddress peer;
  uint16_t connection = NO_CONNECTION;
  Snapshot status;
  companion::Session session;
  companion::ControlWorkspaceLease workspaceLease;
  bool running = false;

  explicit Impl(std::span<uint8_t> workspace)
      : callbacks(*this),
        inputCallbacks(*this),
        mutex(xSemaphoreCreateMutexStatic(&mutexStorage)),
        queue(workspace.first(companion::COMMAND_QUEUE_STORAGE_SIZE)),
        receiveBuffer(workspace.subspan(companion::COMMAND_QUEUE_STORAGE_SIZE, RX_SIZE)),
        assembler(receiveBuffer) {}
};

HalCompanionBluetooth::HalCompanionBluetooth() = default;
HalCompanionBluetooth::~HalCompanionBluetooth() { stop(); }

bool HalCompanionBluetooth::begin(std::span<uint8_t> workspace) {
  if (impl || workspace.size() < companion::SESSION_WORKSPACE_SIZE || NimBLEDevice::isInitialized()) {
    LOG_ERR("COMPANION", "BLE start requires an unused controller and 8 KiB workspace");
    return false;
  }
  if (HalMemory::getInternalHeap().freeBytes < MINIMUM_START_HEAP) {
    LOG_ERR("COMPANION", "Insufficient internal heap for BLE startup gate");
    return false;
  }
  // Callback/lock state lives for the session and exceeds the local stack budget.
  impl = makeUniqueNoThrow<Impl>(workspace);
  if (!impl || !impl->mutex) {
    LOG_ERR("COMPANION", "OOM: BLE callback state");
    impl.reset();
    return false;
  }
  if (!NimBLEDevice::init("lila")) {
    stop();
    LOG_ERR("COMPANION", "BLE init failed");
    return false;
  }
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setMTU(247);
  impl->server = NimBLEDevice::createServer();
  if (!impl->server) {
    stop();
    LOG_ERR("COMPANION", "BLE server allocation failed");
    return false;
  }
  impl->server->setCallbacks(&impl->callbacks, false);
  impl->server->advertiseOnDisconnect(false);
  auto* service = impl->server->createService(SERVICE_UUID);
  if (!service) {
    stop();
    LOG_ERR("COMPANION", "BLE service allocation failed");
    return false;
  }
  auto* input = service->createCharacteristic(
      INPUT_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, 244);
  impl->output = service->createCharacteristic(OUTPUT_UUID, NIMBLE_PROPERTY::NOTIFY, 244);
  if (!input || !impl->output) {
    stop();
    LOG_ERR("COMPANION", "BLE characteristics unavailable");
    return false;
  }
  input->setCallbacks(&impl->inputCallbacks);
  if (!service->start()) {
    stop();
    LOG_ERR("COMPANION", "BLE service start failed");
    return false;
  }
  auto* advertising = NimBLEDevice::getAdvertising();
  advertising->setName("lila");
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->enableScanResponse(true);
  impl->running = true;
  impl->status.state = State::Advertising;
  if (!advertising->start() || HalMemory::getInternalHeap().freeBytes <= REQUIRED_POST_START_HEAP) {
    stop();
    LOG_ERR("COMPANION", "BLE advertising failed or heap below 50 KiB");
    return false;
  }
  LOG_INF("COMPANION", "BLE heap: free=%u largest=%u", static_cast<unsigned>(HalMemory::getInternalHeap().freeBytes),
          static_cast<unsigned>(HalMemory::getInternalHeap().largestBlockBytes));
  return true;
}

void HalCompanionBluetooth::stop() {
  if (!impl) return;
  {
    Impl::Guard lock(impl->mutex);
    impl->running = false;
    impl->queue.clear();
  }
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::stopAdvertising();
    if (!NimBLEDevice::deinit(true)) {
      LOG_ERR("COMPANION", "BLE shutdown failed; rebooting before callback storage is released");
      ESP.restart();
      for (;;) vTaskDelay(portMAX_DELAY);
    }
  }
  vSemaphoreDelete(impl->mutex);
  impl.reset();
}
HalCompanionBluetooth::Snapshot HalCompanionBluetooth::snapshot() const {
  if (!impl) return {};
  Impl::Guard lock(impl->mutex);
  return impl->status;
}
size_t HalCompanionBluetooth::receive(std::span<uint8_t> destination, uint64_t& session) {
  if (!impl) return 0;
  Impl::Guard lock(impl->mutex);
  if (impl->workspaceLease.active()) return 0;
  companion::FrameView frame;
  if (!impl->queue.peek(frame)) return 0;
  const size_t size = companion::encodeFrame(frame, destination);
  if (size) {
    session = impl->session.token();
    impl->queue.pop();
  }
  return size;
}
bool HalCompanionBluetooth::send(std::span<const uint8_t> frame, uint64_t session) {
  if (!impl) return false;
  Impl::Guard lock(impl->mutex);
  if (impl->workspaceLease.active() || !impl->session.accepts(session)) return false;
  const uint16_t connection = impl->connection;
  const uint16_t mtu = impl->server->getPeerMTU(connection);
  if (mtu <= 3) return false;
  while (!frame.empty()) {
    const size_t count = std::min<size_t>(frame.size(), mtu - 3);
    if (!impl->output->notify(frame.data(), count, connection)) return false;
    frame = frame.subspan(count);
  }
  return true;
}
bool HalCompanionBluetooth::acquireWorkspace(uint64_t session) {
  if (!impl) return false;
  Impl::Guard lock(impl->mutex);
  return impl->workspaceLease.acquire(session, impl->session.accepts(session), impl->queue.size(),
                                      impl->assembler.idle());
}
bool HalCompanionBluetooth::workspaceOwned(uint64_t session) const {
  if (!impl) return false;
  Impl::Guard lock(impl->mutex);
  return impl->workspaceLease.valid(session, impl->session.accepts(session));
}
bool HalCompanionBluetooth::releaseWorkspace(uint64_t session) {
  if (!impl) return false;
  Impl::Guard lock(impl->mutex);
  if (!impl->workspaceLease.release(session)) return false;
  impl->queue.clear();
  impl->assembler.reset();
  return true;
}
bool HalCompanionBluetooth::peer(uint64_t session, companion::PairingPeer& output) const {
  if (!impl) return false;
  Impl::Guard lock(impl->mutex);
  if (!impl->session.accepts(session)) return false;
  output[0] = impl->peer.getType();
  std::copy_n(impl->peer.getVal(), 6, output.begin() + 1);
  return true;
}
uint64_t HalCompanionBluetooth::authenticatedSession() const {
  if (!impl) return 0;
  Impl::Guard lock(impl->mutex);
  return impl->session.token();
}
bool HalCompanionBluetooth::generateWifiHandoffSecrets(uint64_t session, companion::Identity& identity,
                                                       companion::Digest& key) {
  companion::clearWifiHandoffSecrets(identity, key);
  if (!impl) {
    LOG_ERR("COMPANION", "BLE entropy unavailable");
    return false;
  }
  Impl::Guard lock(impl->mutex);
  if (!impl->running || !impl->session.accepts(session) || impl->status.state != State::Authenticated) {
    LOG_ERR("COMPANION", "Unauthenticated entropy request");
    return false;
  }
  const companion::WifiRandomSource source{nullptr, [](void*, std::span<uint8_t> bytes) {
                                             esp_fill_random(bytes.data(), bytes.size());
                                             return true;
                                           }};
  if (!companion::generateWifiHandoffSecrets(identity, key, source)) {
    LOG_ERR("COMPANION", "Handoff entropy failed validation");
    return false;
  }
  return true;
}
bool HalCompanionBluetooth::generateWifiHotspotPassword(uint64_t session, companion::WifiHotspotPassword& password) {
  companion::clearWifiHotspotPassword(password);
  if (!impl) {
    LOG_ERR("COMPANION", "BLE entropy unavailable");
    return false;
  }
  Impl::Guard lock(impl->mutex);
  if (!impl->running || !impl->session.accepts(session) || impl->status.state != State::Authenticated) {
    LOG_ERR("COMPANION", "Unauthenticated entropy request");
    return false;
  }
  const companion::WifiRandomSource source{nullptr, [](void*, std::span<uint8_t> bytes) {
                                             esp_fill_random(bytes.data(), bytes.size());
                                             return true;
                                           }};
  if (!companion::generateWifiHotspotPassword(password, source)) {
    LOG_ERR("COMPANION", "Hotspot entropy failed validation");
    return false;
  }
  return true;
}
bool HalCompanionBluetooth::unpairConnected(uint64_t session) {
  if (!impl) return false;
  uint16_t connection;
  NimBLEAddress peer;
  {
    Impl::Guard lock(impl->mutex);
    if (!impl->session.accepts(session)) return false;
    connection = impl->connection;
    peer = impl->peer;
    impl->queue.clear();
    impl->status.state = State::Connected;
    impl->session.authenticate(false);
  }
  const bool disconnected = impl->server->disconnect(connection);
  const bool removed = NimBLEDevice::deleteBond(peer);
  if (!disconnected || !removed) LOG_ERR("COMPANION", "BLE unpair failed");
  return disconnected && removed;
}
#else
struct HalCompanionBluetooth::Impl {};
HalCompanionBluetooth::HalCompanionBluetooth() = default;
HalCompanionBluetooth::~HalCompanionBluetooth() = default;
bool HalCompanionBluetooth::begin(std::span<uint8_t>) { return false; }
void HalCompanionBluetooth::stop() {}
HalCompanionBluetooth::Snapshot HalCompanionBluetooth::snapshot() const { return {}; }
size_t HalCompanionBluetooth::receive(std::span<uint8_t>, uint64_t&) { return 0; }
bool HalCompanionBluetooth::send(std::span<const uint8_t>, uint64_t) { return false; }
bool HalCompanionBluetooth::peer(uint64_t, companion::PairingPeer&) const { return false; }
uint64_t HalCompanionBluetooth::authenticatedSession() const { return 0; }
bool HalCompanionBluetooth::generateWifiHandoffSecrets(uint64_t, companion::Identity& identity,
                                                       companion::Digest& key) {
  companion::clearWifiHandoffSecrets(identity, key);
  LOG_ERR("COMPANION", "BLE entropy unavailable");
  return false;
}
bool HalCompanionBluetooth::generateWifiHotspotPassword(uint64_t, companion::WifiHotspotPassword& password) {
  companion::clearWifiHotspotPassword(password);
  LOG_ERR("COMPANION", "BLE entropy unavailable");
  return false;
}
bool HalCompanionBluetooth::unpairConnected(uint64_t) { return false; }
bool HalCompanionBluetooth::acquireWorkspace(uint64_t) { return false; }
bool HalCompanionBluetooth::workspaceOwned(uint64_t) const { return false; }
bool HalCompanionBluetooth::releaseWorkspace(uint64_t) { return false; }
#endif
