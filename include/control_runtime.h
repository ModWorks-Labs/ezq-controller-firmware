#pragma once

#include <cstdint>

enum class ControlStateId {
  BOOT,
  READY_IDLE,
  BLOWER_MODE,
  IGNITION_CYCLE,
  ABORT_COOLDOWN,
  SLEEP_IDLE,
  FAULT,
};

enum class ControlFaultKind {
  NONE,
  INIT_FAILURE,
  SENSOR_FAILURE,
  OVERTEMPERATURE,
  BATTERY_UNDERVOLTAGE,
};

struct ControlRuntimeStatus {
  ControlStateId state;
  ControlFaultKind fault_kind;
  const char *state_name;
  const char *detail_name;
  uint32_t elapsed_ms;
  uint32_t total_ms;
  bool has_total_ms;
  bool can_start_cycle;
  bool can_start_blower;
  bool can_abort_cycle;
  bool can_sleep;
  bool glow_plug_active;
  bool fan_active;
  bool fault_latched;
  bool temp_valid;
  bool battery_valid;
  float temp_c;
  float battery_v;
};

namespace control_app {

void init();
void tick();
bool initialized();
ControlRuntimeStatus get_status();

}  // namespace control_app
