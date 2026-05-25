#pragma once

#include <cstdint>
#include <string>

#include "control_runtime.h"

namespace update_manager {

enum class CheckDecision {
  NONE,
  NO_UPDATE,
  UPDATE_AVAILABLE,
  ERROR,
};

enum class AsyncState {
  IDLE,
  RUNNING,
  COMPLETE,
};

struct UpdateTarget {
  bool valid = false;
  std::string selector;
  std::string version;
  std::string firmware_url;
  std::string sha256;
  int size = 0;
};

struct UpdateStatus {
  std::string current_version;
  std::string available_version;
  std::string manifest_url;
  std::string last_result;
  std::string last_message;
  std::string target_selector;
  bool update_available = false;
  bool ota_in_progress = false;
  bool pending_confirmation = false;
  bool rollback_armed = false;
};

void init();
UpdateStatus get_status();
CheckDecision check_for_update();
bool perform_pending_update();
bool pending_update_available();
bool start_check_async();
bool start_apply_async();
AsyncState async_state();
CheckDecision async_check_decision();
bool async_apply_success();
void clear_async_state();
void tick_post_boot(ControlStateId state, ControlFaultKind fault_kind, uint32_t now_ms);

}  // namespace update_manager
