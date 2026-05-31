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
  uint8_t sound_volume;
  bool audio_enabled;
  bool post_cycle_reminder_enabled;
  bool automatic_firmware_updates_enabled;
  bool offline_dashboard_ap_enabled;
  bool dashboard_prevents_sleep_enabled;
  uint32_t idle_sleep_timeout_ms;
  uint8_t glow_plug_max_throttle_percent;
  bool glow_plug_ramp_enabled;
  uint8_t glow_plug_initial_throttle_percent;
  uint32_t glow_plug_ramp_time_ms;
  uint32_t glow_plug_pwm_frequency_hz;
  uint8_t fan_min_throttle_percent;
  uint8_t fan_max_throttle_percent;
  uint32_t fan_pwm_frequency_hz;
  uint32_t fan_wake_pulse_ms;
  bool fan_post_cycle_hold_enabled;
  uint8_t fan_post_cycle_hold_throttle_percent;
  uint32_t fan_post_cycle_hold_duration_ms;
  float temp_fault_high_c;
  float temp_fault_low_c;
  float battery_warning_v;
  float battery_fault_low_v;
  bool battery_warning_enabled;
  bool sensor_fault_detection_enabled;
  bool fault_latch_enabled;
  uint32_t abort_blower_duration_ms;
  uint32_t countdown_total_ms;
  uint32_t countdown_step_ms;
  uint32_t cycle_total_duration_ms;
  uint32_t post_cycle_reminder_repeat_ms;
  uint32_t post_cycle_reminder_tone_duration_ms;
};

bool init();
bool prepare_settings_partition_update();
bool finalize_settings_partition_update();
bool update_setting(const std::string &key, const std::string &value, std::string &message);
std::string settings_schema_json();
bool update_settings_schema_json(const std::string &json, std::string &message);
std::string cycle_profile_json();
bool update_cycle_profile_json(const std::string &json, std::string &message);
const Settings &settings();
const IgnitionProfile &active_profile();

}  // namespace config_store
