#pragma once

#include <string>

namespace wifi_manager {

struct WifiStatus {
  bool initialized;
  bool connected;
  bool mdns_ready;
  bool got_ip;
  int retry_count;
  std::string hostname;
  std::string ip_address;
};

void init();
void suspend_for_sleep();
void resume_after_sleep();
WifiStatus get_status();

}  // namespace wifi_manager
