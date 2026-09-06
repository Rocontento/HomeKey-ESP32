#pragma once
#include <string>
#include <vector>
#include <map>

struct XiaomiDevice {
  std::string name;
  std::string model;
  std::string did;
  std::string token;  // the local miIO token, what we are really after
  std::string ip;
  bool online = false;
};

/**
 * @class XiaomiCloud
 * @brief One-shot Xiaomi account login used only to harvest local device tokens.
 *
 * Same flow the Home Assistant integrations use: serviceLogin -> serviceLoginAuth2 ->
 * follow the returned location to pick up a serviceToken, then an RC4-signed call to
 * /home/device_list. Nothing here is persisted: the password never leaves the stack and
 * the session dies with the object. Only the chosen device's ip/token/did are saved.
 *
 * Blocking and TLS-heavy: call it from a task with >= 12 kB of stack, not from the
 * httpd handler task.
 */
class XiaomiCloud {
public:
  /// @param region "cn", "de", "us", "ru", "sg", "i2" ... (Europe is "de")
  bool loginAndListDevices(const std::string& user, const std::string& pass,
                           const std::string& region,
                           std::vector<XiaomiDevice>& out, std::string& err);

  /// Set when the account demands 2FA: the user must open this URL once, then retry.
  const std::string& twoFactorUrl() const { return m_2faUrl; }

private:
  bool step1Sign(std::string& err);
  bool step2Auth(const std::string& user, const std::string& pass, std::string& err);
  bool step3ServiceToken(std::string& err);
  bool deviceList(const std::string& region, std::vector<XiaomiDevice>& out, std::string& err);

  std::string m_agent;
  std::string m_deviceId;
  std::string m_sign;
  std::string m_ssecurity;
  std::string m_userId;
  std::string m_location;
  std::string m_serviceToken;
  std::string m_2faUrl;
  std::map<std::string, std::string> m_jar;
};
