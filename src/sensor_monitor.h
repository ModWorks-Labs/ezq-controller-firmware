#pragma once

#include <cstdint>

#include "config_store.h"
#include "control_runtime.h"

namespace sensor_monitor {

struct Snapshot {
  bool temp_sensor_present;
  bool temp_valid;
  float temp_c;
  bool battery_valid;
  float battery_v;
};

bool initialized();
bool init(const config_store::Settings &settings);
void update(uint32_t now_ms, const config_store::Settings &settings);
Snapshot snapshot();
ControlFaultKind evaluate_fault(const config_store::Settings &settings);

}  // namespace sensor_monitor
