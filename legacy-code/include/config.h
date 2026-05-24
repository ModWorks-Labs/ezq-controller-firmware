#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

namespace config {

constexpr int kMaxIgnitionProfileSegments = 16;
constexpr int kMaxIgnitionProfiles = 16;

struct IgnitionProfileSegment {
  bool is_ramp;
  unsigned long duration_ms;
  float throttle_percent;
};

struct IgnitionProfile {
  bool valid;
  unsigned int gp_segment_count;
  unsigned int fan_segment_count;
  unsigned long total_duration_ms;
  IgnitionProfileSegment gp_segments[kMaxIgnitionProfileSegments];
  IgnitionProfileSegment fan_segments[kMaxIgnitionProfileSegments];
};

struct IgnitionProfileFileInfo {
  String filename;
  String exported_at;
  unsigned int gp_segment_count;
  unsigned int fan_segment_count;
  unsigned long total_duration_ms;
  bool is_active;
  bool is_default;
};

struct Settings {
  String firmware_version;
  String board_id;
  String serial_number;
  String wifi_device_name;
  unsigned long idle_sleep_timeout_ms;
  String active_profile_filename;
  float temp_fault_high_c;
  float temp_fault_low_c;
  unsigned long abort_blower_duration_ms;
  unsigned int sound_volume;
  bool button_press_beep_enabled;
};

void setupConfig();
const Settings &getSettings();
const IgnitionProfile &getIgnitionProfile();
const String &getLoadedIgnitionProfileFilename();
const String &getWifiDeviceName();
void setIdleSleepTimeoutMs(unsigned long duration_ms);
void setActiveProfileFilename(const String &filename);
const String &getActiveProfileFilename();
void setWifiDeviceName(const String &name);
void setTempFaultHighC(float temperature_c);
void setTempFaultLowC(float temperature_c);
void setAbortBlowerDurationMs(unsigned long duration_ms);
void setSoundVolume(unsigned int volume);
void setButtonPressBeepEnabled(bool enabled);
bool reloadIgnitionProfile();
size_t listIgnitionProfiles(IgnitionProfileFileInfo *entries, size_t max_entries);
bool saveIgnitionProfileFile(const String &filename,
                             const String &json_text,
                             String &error_message,
                             String *stored_filename = nullptr);
bool deleteIgnitionProfileFile(const String &filename, String &error_message);
bool profileFileExists(const String &filename);
bool saveConfig();
bool saveDevice();

}

#endif
