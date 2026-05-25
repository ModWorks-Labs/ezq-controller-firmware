#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace config_store {

constexpr std::size_t kMaxIgnitionProfileSegments = 16;

struct IgnitionProfileSegment {
  bool is_ramp;
  uint32_t duration_ms;
  float throttle_percent;
};

struct IgnitionProfile {
  bool valid;
  std::size_t gp_segment_count;
  std::size_t fan_segment_count;
  uint32_t total_duration_ms;
  std::array<IgnitionProfileSegment, kMaxIgnitionProfileSegments> gp_segments;
  std::array<IgnitionProfileSegment, kMaxIgnitionProfileSegments> fan_segments;
};

struct Settings {
  uint32_t idle_sleep_timeout_ms;
  uint32_t abort_blower_duration_ms;
  float temp_fault_high_c;
  float temp_fault_low_c;
  float battery_warning_v;
  float battery_fault_low_v;
  uint8_t sound_volume;
  bool button_press_beep_enabled;
  std::array<char, 16> active_profile_name;
};

bool init();
bool prepare_settings_partition_update();
bool finalize_settings_partition_update();
bool update_setting(const std::string &key, const std::string &value, std::string &message);
const Settings &settings();
const IgnitionProfile &active_profile();

}  // namespace config_store
