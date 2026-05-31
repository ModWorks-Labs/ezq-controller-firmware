#pragma once

#include <string>

namespace ota_service {

struct OtaStatus {
  bool server_running;
  bool reboot_pending;
  std::string running_partition;
  std::string boot_partition;
  std::string next_update_partition;
};

void init();
void poll();
OtaStatus get_status();
bool maintenance_active();
bool dashboard_session_active();

}  // namespace ota_service
