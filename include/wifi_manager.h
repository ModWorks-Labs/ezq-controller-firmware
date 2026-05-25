#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wifi_manager {

enum class WifiMode {
  STA,
  PROVISION_AP,
  APSTA_TEST,
};

struct ScanResult {
  std::string ssid;
  int rssi;
  bool secure;
  std::string auth_mode;
};

struct WifiStatus {
  bool initialized;
  bool connected;
  bool mdns_ready;
  bool got_ip;
  bool credentials_stored;
  bool ap_active;
  bool connect_test_active;
  bool last_connect_success;
  int retry_count;
  WifiMode mode;
  std::string hostname;
  std::string ip_address;
  std::string configured_ssid;
  std::string ap_ssid;
  std::string ap_ip_address;
  std::string last_connect_message;
};

void init();
bool provisioning_required();
bool scan_networks(std::vector<ScanResult> &results, std::string &message);
bool test_and_store_credentials(const std::string &ssid,
                                const std::string &password,
                                std::string &message);
bool clear_credentials(std::string &message);
void suspend_for_sleep();
void resume_after_sleep();
WifiStatus get_status();

}  // namespace wifi_manager
