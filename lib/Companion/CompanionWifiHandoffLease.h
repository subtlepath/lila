#pragma once

#include "CompanionContentExportBinding.h"
#include "CompanionJournalMergeIntent.h"
#include "CompanionWifiHandoffCommands.h"

namespace companion {
enum class WifiHandoffLeasePhase { Empty, Prepared, ActivationRequested, Consuming };
enum class WifiHandoffLeaseResult { Ok, Invalid, Unauthorized, Busy, Expired, NoOffer, SetupFailed };
struct WifiActivationView {
  const WifiHandoffOffer& offer;
  WifiNetworkMode mode;
  const char* ssid;
  const char* password;
  uint64_t receivedAtMilliseconds;
};
struct WifiActivationSink {
  void* context = nullptr;
  bool (*accept)(void*, const WifiActivationView&) = nullptr;
};
// Session-owned outside the task stack; no heap allocation or SD mutation here.
class WifiHandoffLease final {
 public:
  ~WifiHandoffLease() { reset(); }
  WifiHandoffLease() = default;
  WifiHandoffLease(const WifiHandoffLease&) = delete;
  WifiHandoffLease& operator=(const WifiHandoffLease&) = delete;
  WifiHandoffLeasePhase phase() const { return state; }
  void reset() {
    clearWifiHandoffSecrets(offer.session, offer.key);
    volatile char* secret = password.data();
    for (size_t at = 0; at < password.size(); ++at) secret[at] = 0;
    ssid.fill(0);
    offer = {};
    token = receivedAt = 0;
    ssidLength = passwordLength = 0;
    mode = WifiNetworkMode::SavedNetwork;
    state = WifiHandoffLeasePhase::Empty;
  }
  WifiHandoffLeaseResult prepare(const WifiNetworkOffer& source, const WifiHandoffPrepare& request,
                                 const TransferState& transfer, const Identity& reader, const Identity& generation,
                                 const Identity& installation, uint64_t authenticatedToken, uint64_t now) {
    const bool valid = request.transaction == transfer.transaction && transfer.owner == installation &&
                       transfer.storageGeneration == generation && transfer.length &&
                       transfer.durableOffset <= transfer.length &&
                       (transfer.phase == TransferPhase::Receiving || transfer.phase == TransferPhase::Verified) &&
                       (transfer.phase != TransferPhase::Verified || transfer.durableOffset == transfer.length);
    return prepareBound(source, request, transfer.transaction, reader, generation, installation, authenticatedToken,
                        now, valid);
  }
  // Caller supplies the receiver's validated binding, never a declaration from an unaudited request.
  WifiHandoffLeaseResult prepareJournal(const WifiNetworkOffer& source, const WifiHandoffPrepare& request,
                                        const JournalMergeIntent& merge, uint32_t durableCount, const Identity& reader,
                                        const Identity& generation, const Identity& installation,
                                        uint64_t authenticatedToken, uint64_t now) {
    const bool valid = validJournalMergeIntent(merge) && merge.owner == installation &&
                       merge.generation == generation && request.transaction == merge.transaction &&
                       durableCount >= merge.previous.count && durableCount < merge.merged.count;
    return prepareBound(source, request, merge.transaction, reader, generation, installation, authenticatedToken, now,
                        valid);
  }

  WifiHandoffLeaseResult prepareExport(const WifiNetworkOffer& source, const WifiHandoffPrepare& request,
                                       const ContentExportBinding& binding, uint64_t inventoryRevision,
                                       const Identity& reader, const Identity& generation, const Identity& installation,
                                       uint64_t authenticatedToken, uint64_t now) {
    return prepareBound(source, request, request.transaction, reader, generation, installation, authenticatedToken, now,
                        binding.boundTo(request.transaction, installation, generation, inventoryRevision));
  }

 private:
  WifiHandoffLeaseResult prepareBound(const WifiNetworkOffer& source, const WifiHandoffPrepare& request,
                                      const Identity& transaction, const Identity& reader, const Identity& generation,
                                      const Identity& installation, uint64_t authenticatedToken, uint64_t now,
                                      bool bindingValid) {
    if (!authenticatedToken) return WifiHandoffLeaseResult::Unauthorized;
    if (state == WifiHandoffLeasePhase::Consuming) return WifiHandoffLeaseResult::Busy;
    expire(now);
    if (state != WifiHandoffLeasePhase::Empty) return WifiHandoffLeaseResult::Busy;
    if (!bindingValid || !validWifiNetworkDescription(source.network) || source.network.mode != request.mode ||
        !wifiHandoffMatches(source.offer, reader, generation, installation, transaction))
      return WifiHandoffLeaseResult::Invalid;
    offer = source.offer;
    mode = source.network.mode;
    ssidLength = source.network.ssid.size();
    passwordLength = source.network.password.size();
    std::copy(source.network.ssid.begin(), source.network.ssid.end(), ssid.begin());
    std::copy(source.network.password.begin(), source.network.password.end(), password.begin());
    token = authenticatedToken;
    receivedAt = now;
    state = WifiHandoffLeasePhase::Prepared;
    return WifiHandoffLeaseResult::Ok;
  }

 public:
  // Pass actual transport/installation state, not identities read from a command.
  bool reconcile(uint64_t authenticatedToken, const Identity& installation, uint64_t now) {
    if (state == WifiHandoffLeasePhase::Consuming) return true;
    if (expire(now) || state == WifiHandoffLeasePhase::Empty) return false;
    if (!authenticatedToken && state == WifiHandoffLeasePhase::ActivationRequested) return true;
    if (authenticatedToken != token || installation != offer.installation) {
      reset();
      return false;
    }
    return true;
  }
  WifiHandoffLeaseResult apply(const WifiHandoffSessionCommand& command, uint64_t authenticatedToken,
                               const Identity& installation, uint64_t now) {
    if (!authenticatedToken) return WifiHandoffLeaseResult::Unauthorized;
    if (state == WifiHandoffLeasePhase::Consuming) return WifiHandoffLeaseResult::Busy;
    if (expire(now)) return WifiHandoffLeaseResult::Expired;
    if (state == WifiHandoffLeasePhase::Empty) return WifiHandoffLeaseResult::NoOffer;
    if (authenticatedToken != token || installation != offer.installation) return WifiHandoffLeaseResult::Unauthorized;
    if (command.transaction != offer.transaction || command.session != offer.session)
      return WifiHandoffLeaseResult::Invalid;
    if (command.action == WifiHandoffAction::Cancel) {
      reset();
      return WifiHandoffLeaseResult::Ok;
    }
    if (command.action != WifiHandoffAction::Activate) return WifiHandoffLeaseResult::Invalid;
    state = WifiHandoffLeasePhase::ActivationRequested;
    return WifiHandoffLeaseResult::Ok;
  }
  size_t encode(std::span<uint8_t> output, uint64_t now) {
    if (state == WifiHandoffLeasePhase::Consuming || expire(now) || state == WifiHandoffLeasePhase::Empty) return 0;
    struct Scratch {
      WifiNetworkOffer value;
      ~Scratch() { clearWifiHandoffSecrets(value.offer.session, value.offer.key); }
    } scratch{{offer, {mode, {ssid.data(), ssidLength}, {password.data(), passwordLength}}}};
    return encodeWifiNetworkOffer(scratch.value, output);
  }
  // Sink must copy needed values synchronously and never retain view/pointers.
  WifiHandoffLeaseResult consume(uint64_t now, WifiActivationSink sink) {
    if (state == WifiHandoffLeasePhase::Consuming) return WifiHandoffLeaseResult::Busy;
    if (expire(now)) return WifiHandoffLeaseResult::Expired;
    if (state != WifiHandoffLeasePhase::ActivationRequested) return WifiHandoffLeaseResult::NoOffer;
    if (!sink.accept) {
      reset();
      return WifiHandoffLeaseResult::Invalid;
    }
    state = WifiHandoffLeasePhase::Consuming;
    struct Guard {
      WifiHandoffLease& lease;
      ~Guard() { lease.reset(); }
    } guard{*this};
    const WifiActivationView view{offer, mode, ssid.data(), password.data(), receivedAt};
    return sink.accept(sink.context, view) ? WifiHandoffLeaseResult::Ok : WifiHandoffLeaseResult::SetupFailed;
  }

 private:
  bool expire(uint64_t now) {
    if (state == WifiHandoffLeasePhase::Empty) return false;
    if (now >= receivedAt && now - receivedAt < static_cast<uint64_t>(offer.lifetimeSeconds) * 1000) return false;
    reset();
    return true;
  }
  WifiHandoffOffer offer;
  std::array<char, 33> ssid{};
  std::array<char, 64> password{};
  uint64_t token = 0, receivedAt = 0;
  size_t ssidLength = 0, passwordLength = 0;
  WifiNetworkMode mode = WifiNetworkMode::SavedNetwork;
  WifiHandoffLeasePhase state = WifiHandoffLeasePhase::Empty;
};
}  // namespace companion
