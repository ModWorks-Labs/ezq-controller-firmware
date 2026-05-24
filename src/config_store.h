#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

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
  uint32_t abort_cooldown_ms;
  float temp_fault_high_c;
  float battery_fault_low_v;
  std::array<char, 16> active_profile_name;
};

bool init();
const Settings &settings();
const IgnitionProfile &active_profile();

}  // namespace config_store
