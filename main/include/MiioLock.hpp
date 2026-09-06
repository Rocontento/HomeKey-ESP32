#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include "app_event_loop.hpp"

/**
 * @class MiioLock
 * @brief Sends an unlatch command to a Xiaomi lock over the local miIO protocol
 *        (UDP 54321) when a HomeKey tap is authenticated.
 *
 * No cloud, no Home Assistant: the lock is reached directly on the LAN. The
 * credentials (token/did) come from the Xiaomi account, which is what the web
 * portal login provides.
 */
class MiioLock {
public:
  MiioLock() = default;

  /// Subscribes to NFC_EVENT/NFC_TAP_EVENT. A successful HomeKey tap unlatches.
  void begin();

  /// Runtime credentials (from NVS, or from the web portal after a cloud login).
  void setCredentials(std::string host, std::string token, std::string did);

  bool isConfigured() const { return !m_host.empty() && m_token.size() == 32 && !m_did.empty(); }

  /// Blocking miIO round-trip, ~2 s worst case. True on {"code":0}.
  bool callAction(int siid, int aiid);

  /// Known-answer test of the packet packer (vector generated with openssl).
  static bool selftest();

  // siid 18 = lock-unlock. aiid 4 = emergency-unlock, the motor pull (unlatch);
  // aiid 9 = ble-unlock. Both take no parameters, unlike remote-unlock-e which
  // needs a cloud-issued encrypted secret.
  int siid = 18;
  int unlatch_aiid = 4;

private:
  std::string m_host, m_token, m_did;
  int64_t m_lastTapUs = 0;
  AppEventLoop::SubscriptionHandle m_nfc_event{};
};
