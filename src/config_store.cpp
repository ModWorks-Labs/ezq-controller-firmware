#include "config_store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "debug_console.h"
#include "esp_spiffs.h"
#include "settings_defaults.h"

namespace config_store {
namespace {

constexpr char kTag[] = "config_store";
constexpr char kSettingsPartitionLabel[] = "settings";
constexpr char kSettingsBasePath[] = "/settings";
constexpr char kSettingsFilePath[] = "/settings/settings.json";
constexpr char kCycleProfileFilePath[] = "/settings/cycle_profile.json";
constexpr std::size_t kSettingsFileMaxBytes = 16384;
constexpr std::size_t kCycleProfileFileMaxBytes = 4096;

struct FanStep {
  uint32_t start_sec;
  uint8_t target_throttle_percent;
  uint32_t ramp_duration_sec;
};

struct CycleProfileData {
  uint32_t glow_plug_duration_sec = 0;
  std::vector<FanStep> fan_steps;
};

Settings g_settings = {};
IgnitionProfile g_profile = {};
std::string g_settings_schema_json;
std::string g_cycle_profile_json;
bool g_initialized = false;
bool g_settings_fs_mounted = false;

bool mount_settings_partition() {
  if (g_settings_fs_mounted) {
    return true;
  }

  const esp_vfs_spiffs_conf_t conf = {
      .base_path = kSettingsBasePath,
      .partition_label = kSettingsPartitionLabel,
      .max_files = 6,
      .format_if_mount_failed = false,
  };

  const esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Failed to mount settings partition: %s", esp_err_to_name(err));
    return false;
  }

  g_settings_fs_mounted = true;
  return true;
}

void unmount_settings_partition() {
  if (!g_settings_fs_mounted) {
    return;
  }

  esp_vfs_spiffs_unregister(kSettingsPartitionLabel);
  g_settings_fs_mounted = false;
}

bool parse_bool_text(const std::string &value, bool &parsed) {
  if (value == "true" || value == "1" || value == "on" || value == "yes") {
    parsed = true;
    return true;
  }
  if (value == "false" || value == "0" || value == "off" || value == "no") {
    parsed = false;
    return true;
  }
  return false;
}

std::string print_json(cJSON *root) {
  char *printed = cJSON_Print(root);
  if (printed == nullptr) {
    return {};
  }
  std::string json(printed);
  cJSON_free(printed);
  return json;
}

bool read_text_file(const char *path, std::size_t max_bytes, std::string &content) {
  FILE *file = fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }

  const long size = ftell(file);
  if (size <= 0 || static_cast<std::size_t>(size) > max_bytes) {
    fclose(file);
    return false;
  }

  rewind(file);
  content.assign(static_cast<std::size_t>(size), '\0');
  const std::size_t read = fread(content.data(), 1, content.size(), file);
  fclose(file);
  return read == content.size();
}

bool write_text_file(const char *path, const std::string &content) {
  FILE *file = fopen(path, "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = fwrite(content.data(), 1, content.size(), file);
  fclose(file);
  return written == content.size();
}

bool setting_bool_value(cJSON *setting, bool &value) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(setting, "value");
  if (!cJSON_IsBool(item)) {
    return false;
  }
  value = cJSON_IsTrue(item);
  return true;
}

bool setting_u32_value(cJSON *setting, uint32_t &value) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(setting, "value");
  if (!cJSON_IsNumber(item) || item->valuedouble < 0.0) {
    return false;
  }
  value = static_cast<uint32_t>(item->valuedouble);
  return true;
}

bool setting_u8_value(cJSON *setting, uint8_t &value) {
  uint32_t parsed = 0;
  if (!setting_u32_value(setting, parsed) || parsed > 255U) {
    return false;
  }
  value = static_cast<uint8_t>(parsed);
  return true;
}

bool setting_float_value(cJSON *setting, float &value) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(setting, "value");
  if (!cJSON_IsNumber(item)) {
    return false;
  }
  value = static_cast<float>(item->valuedouble);
  return true;
}

bool apply_setting_from_object(cJSON *setting, Settings &settings_out) {
  cJSON *key = cJSON_GetObjectItemCaseSensitive(setting, "key");
  if (!cJSON_IsString(key) || key->valuestring == nullptr) {
    return true;
  }

  const std::string key_name = key->valuestring;
  if (key_name == "sound_volume") {
    return setting_u8_value(setting, settings_out.sound_volume);
  }
  if (key_name == "audio_enabled") {
    return setting_bool_value(setting, settings_out.audio_enabled);
  }
  if (key_name == "post_cycle_reminder_enabled") {
    return setting_bool_value(setting, settings_out.post_cycle_reminder_enabled);
  }
  if (key_name == "automatic_firmware_updates_enabled") {
    return setting_bool_value(setting, settings_out.automatic_firmware_updates_enabled);
  }
  if (key_name == "offline_dashboard_ap_enabled") {
    return setting_bool_value(setting, settings_out.offline_dashboard_ap_enabled);
  }
  if (key_name == "dashboard_prevents_sleep_enabled") {
    return setting_bool_value(setting, settings_out.dashboard_prevents_sleep_enabled);
  }
  if (key_name == "idle_sleep_timeout_ms") {
    return setting_u32_value(setting, settings_out.idle_sleep_timeout_ms);
  }
  if (key_name == "glow_plug_max_throttle_percent") {
    return setting_u8_value(setting, settings_out.glow_plug_max_throttle_percent);
  }
  if (key_name == "glow_plug_ramp_enabled") {
    return setting_bool_value(setting, settings_out.glow_plug_ramp_enabled);
  }
  if (key_name == "glow_plug_initial_throttle_percent") {
    return setting_u8_value(setting, settings_out.glow_plug_initial_throttle_percent);
  }
  if (key_name == "glow_plug_ramp_time_ms") {
    return setting_u32_value(setting, settings_out.glow_plug_ramp_time_ms);
  }
  if (key_name == "glow_plug_pwm_frequency_hz") {
    return setting_u32_value(setting, settings_out.glow_plug_pwm_frequency_hz);
  }
  if (key_name == "fan_min_throttle_percent") {
    return setting_u8_value(setting, settings_out.fan_min_throttle_percent);
  }
  if (key_name == "fan_max_throttle_percent") {
    return setting_u8_value(setting, settings_out.fan_max_throttle_percent);
  }
  if (key_name == "fan_pwm_frequency_hz") {
    return setting_u32_value(setting, settings_out.fan_pwm_frequency_hz);
  }
  if (key_name == "fan_wake_pulse_ms") {
    return setting_u32_value(setting, settings_out.fan_wake_pulse_ms);
  }
  if (key_name == "fan_post_cycle_hold_enabled") {
    return setting_bool_value(setting, settings_out.fan_post_cycle_hold_enabled);
  }
  if (key_name == "fan_post_cycle_hold_throttle_percent") {
    return setting_u8_value(setting, settings_out.fan_post_cycle_hold_throttle_percent);
  }
  if (key_name == "fan_post_cycle_hold_duration_ms") {
    return setting_u32_value(setting, settings_out.fan_post_cycle_hold_duration_ms);
  }
  if (key_name == "temp_fault_high_c") {
    return setting_float_value(setting, settings_out.temp_fault_high_c);
  }
  if (key_name == "temp_fault_low_c") {
    return setting_float_value(setting, settings_out.temp_fault_low_c);
  }
  if (key_name == "battery_warning_v") {
    return setting_float_value(setting, settings_out.battery_warning_v);
  }
  if (key_name == "battery_fault_v") {
    return setting_float_value(setting, settings_out.battery_fault_low_v);
  }
  if (key_name == "battery_warning_enabled") {
    return setting_bool_value(setting, settings_out.battery_warning_enabled);
  }
  if (key_name == "sensor_fault_detection_enabled") {
    return setting_bool_value(setting, settings_out.sensor_fault_detection_enabled);
  }
  if (key_name == "fault_latch_enabled") {
    return setting_bool_value(setting, settings_out.fault_latch_enabled);
  }
  if (key_name == "abort_blower_duration_ms") {
    return setting_u32_value(setting, settings_out.abort_blower_duration_ms);
  }
  if (key_name == "countdown_total_ms") {
    return setting_u32_value(setting, settings_out.countdown_total_ms);
  }
  if (key_name == "countdown_step_ms") {
    return setting_u32_value(setting, settings_out.countdown_step_ms);
  }
  if (key_name == "cycle_total_duration_ms") {
    return setting_u32_value(setting, settings_out.cycle_total_duration_ms);
  }
  if (key_name == "post_cycle_reminder_repeat_ms") {
    return setting_u32_value(setting, settings_out.post_cycle_reminder_repeat_ms);
  }
  if (key_name == "post_cycle_reminder_tone_duration_ms") {
    return setting_u32_value(setting, settings_out.post_cycle_reminder_tone_duration_ms);
  }

  return true;
}

bool parse_settings_schema_json_impl(const std::string &json,
                                     Settings &settings_out,
                                     std::string &message) {
  cJSON *root = cJSON_Parse(json.c_str());
  if (root == nullptr) {
    message = "Invalid settings schema JSON";
    return false;
  }

  cJSON *categories = cJSON_GetObjectItemCaseSensitive(root, "categories");
  if (!cJSON_IsArray(categories)) {
    cJSON_Delete(root);
    message = "settings.json categories array missing";
    return false;
  }

  for (cJSON *category = categories->child; category != nullptr; category = category->next) {
    cJSON *settings = cJSON_GetObjectItemCaseSensitive(category, "settings");
    if (!cJSON_IsArray(settings)) {
      continue;
    }
    for (cJSON *setting = settings->child; setting != nullptr; setting = setting->next) {
      if (!apply_setting_from_object(setting, settings_out)) {
        cJSON_Delete(root);
        message = "Invalid value in settings schema";
        return false;
      }
    }
  }

  cJSON_Delete(root);
  return true;
}

cJSON *make_range(double min_value, double max_value) {
  cJSON *range = cJSON_CreateObject();
  cJSON_AddNumberToObject(range, "min", min_value);
  cJSON_AddNumberToObject(range, "max", max_value);
  return range;
}

void add_setting(cJSON *settings_array,
                 const char *key,
                 const char *menu_name,
                 const char *input_type,
                 const char *unit,
                 cJSON *value,
                 cJSON *default_value,
                 cJSON *accepted_range) {
  cJSON *setting = cJSON_CreateObject();
  cJSON_AddStringToObject(setting, "key", key);
  cJSON_AddStringToObject(setting, "menu_name", menu_name);
  cJSON_AddStringToObject(setting, "input_type", input_type);
  if (unit != nullptr) {
    cJSON_AddStringToObject(setting, "unit", unit);
  }
  cJSON_AddItemToObject(setting, "value", value);
  cJSON_AddItemToObject(setting, "default_value", default_value);
  if (accepted_range != nullptr) {
    cJSON_AddItemToObject(setting, "accepted_range", accepted_range);
  }
  cJSON_AddItemToArray(settings_array, setting);
}

void add_category(cJSON *categories,
                  const char *id,
                  const char *menu_name,
                  const std::function<void(cJSON *)> &populate) {
  cJSON *category = cJSON_CreateObject();
  cJSON_AddStringToObject(category, "id", id);
  cJSON_AddStringToObject(category, "menu_name", menu_name);
  cJSON *settings = cJSON_AddArrayToObject(category, "settings");
  populate(settings);
  cJSON_AddItemToArray(categories, category);
}

std::string default_settings_schema_json() {
  const auto &defaults = settings_defaults::settings();
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "schema_version", 1);
  cJSON *categories = cJSON_AddArrayToObject(root, "categories");

  add_category(categories, "audio_alerts", "Audio Alerts", [&](cJSON *settings) {
    add_setting(settings,
                "sound_volume",
                "Sound Volume",
                "uint8",
                "level",
                cJSON_CreateNumber(defaults.sound_volume),
                cJSON_CreateNumber(defaults.sound_volume),
                make_range(0, 3));
    add_setting(settings,
                "audio_enabled",
                "Audio Enabled",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.audio_enabled),
                cJSON_CreateBool(defaults.audio_enabled),
                nullptr);
    add_setting(settings,
                "post_cycle_reminder_enabled",
                "Post Cycle Reminder Enabled",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.post_cycle_reminder_enabled),
                cJSON_CreateBool(defaults.post_cycle_reminder_enabled),
                nullptr);
  });

  add_category(categories, "connectivity", "Connectivity", [&](cJSON *settings) {
    add_setting(settings,
                "automatic_firmware_updates_enabled",
                "Automatic Firmware Updates",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.automatic_firmware_updates_enabled),
                cJSON_CreateBool(defaults.automatic_firmware_updates_enabled),
                nullptr);
    add_setting(settings,
                "offline_dashboard_ap_enabled",
                "Offline Dashboard AP",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.offline_dashboard_ap_enabled),
                cJSON_CreateBool(defaults.offline_dashboard_ap_enabled),
                nullptr);
    add_setting(settings,
                "dashboard_prevents_sleep_enabled",
                "Dashboard Prevents Sleep",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.dashboard_prevents_sleep_enabled),
                cJSON_CreateBool(defaults.dashboard_prevents_sleep_enabled),
                nullptr);
  });

  add_category(categories, "state_handling", "State Handling", [&](cJSON *settings) {
    add_setting(settings,
                "idle_sleep_timeout_ms",
                "Idle Sleep Timeout",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.idle_sleep_timeout_ms),
                cJSON_CreateNumber(defaults.idle_sleep_timeout_ms),
                make_range(0, 86400000));
  });

  add_category(categories, "glow_plug", "Glow Plug", [&](cJSON *settings) {
    add_setting(settings,
                "glow_plug_max_throttle_percent",
                "Maximum Throttle",
                "uint8",
                "%",
                cJSON_CreateNumber(defaults.glow_plug_max_throttle_percent),
                cJSON_CreateNumber(defaults.glow_plug_max_throttle_percent),
                make_range(0, 100));
    add_setting(settings,
                "glow_plug_ramp_enabled",
                "Ramp Enabled",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.glow_plug_ramp_enabled),
                cJSON_CreateBool(defaults.glow_plug_ramp_enabled),
                nullptr);
    add_setting(settings,
                "glow_plug_initial_throttle_percent",
                "Initial Throttle",
                "uint8",
                "%",
                cJSON_CreateNumber(defaults.glow_plug_initial_throttle_percent),
                cJSON_CreateNumber(defaults.glow_plug_initial_throttle_percent),
                make_range(0, 100));
    add_setting(settings,
                "glow_plug_ramp_time_ms",
                "Ramp Time",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.glow_plug_ramp_time_ms),
                cJSON_CreateNumber(defaults.glow_plug_ramp_time_ms),
                make_range(0, 30000));
    add_setting(settings,
                "glow_plug_pwm_frequency_hz",
                "PWM Frequency",
                "uint32",
                "Hz",
                cJSON_CreateNumber(defaults.glow_plug_pwm_frequency_hz),
                cJSON_CreateNumber(defaults.glow_plug_pwm_frequency_hz),
                make_range(100, 80000));
  });

  add_category(categories, "fan", "Fan", [&](cJSON *settings) {
    add_setting(settings,
                "fan_min_throttle_percent",
                "Minimum Throttle",
                "uint8",
                "%",
                cJSON_CreateNumber(defaults.fan_min_throttle_percent),
                cJSON_CreateNumber(defaults.fan_min_throttle_percent),
                make_range(0, 100));
    add_setting(settings,
                "fan_max_throttle_percent",
                "Maximum Throttle",
                "uint8",
                "%",
                cJSON_CreateNumber(defaults.fan_max_throttle_percent),
                cJSON_CreateNumber(defaults.fan_max_throttle_percent),
                make_range(0, 100));
    add_setting(settings,
                "fan_pwm_frequency_hz",
                "PWM Frequency",
                "uint32",
                "Hz",
                cJSON_CreateNumber(defaults.fan_pwm_frequency_hz),
                cJSON_CreateNumber(defaults.fan_pwm_frequency_hz),
                make_range(100, 80000));
    add_setting(settings,
                "fan_wake_pulse_ms",
                "Wake Pulse",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.fan_wake_pulse_ms),
                cJSON_CreateNumber(defaults.fan_wake_pulse_ms),
                make_range(0, 500));
    add_setting(settings,
                "fan_post_cycle_hold_enabled",
                "Post Cycle Hold Enabled",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.fan_post_cycle_hold_enabled),
                cJSON_CreateBool(defaults.fan_post_cycle_hold_enabled),
                nullptr);
    add_setting(settings,
                "fan_post_cycle_hold_throttle_percent",
                "Post Cycle Hold Throttle",
                "uint8",
                "%",
                cJSON_CreateNumber(defaults.fan_post_cycle_hold_throttle_percent),
                cJSON_CreateNumber(defaults.fan_post_cycle_hold_throttle_percent),
                make_range(0, 100));
    add_setting(settings,
                "fan_post_cycle_hold_duration_ms",
                "Post Cycle Hold Duration",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.fan_post_cycle_hold_duration_ms),
                cJSON_CreateNumber(defaults.fan_post_cycle_hold_duration_ms),
                make_range(0, 900000));
  });

  add_category(categories, "fault_detection", "Fault Detection", [&](cJSON *settings) {
    add_setting(settings,
                "temp_fault_high_c",
                "Upper Fault Temp",
                "float",
                "C",
                cJSON_CreateNumber(defaults.temp_fault_high_c),
                cJSON_CreateNumber(defaults.temp_fault_high_c),
                make_range(0.0, 150.0));
    add_setting(settings,
                "temp_fault_low_c",
                "Lower Fault Temp",
                "float",
                "C",
                cJSON_CreateNumber(defaults.temp_fault_low_c),
                cJSON_CreateNumber(defaults.temp_fault_low_c),
                make_range(0.0, 150.0));
    add_setting(settings,
                "battery_warning_v",
                "Battery Warning Voltage",
                "float",
                "V",
                cJSON_CreateNumber(defaults.battery_warning_v),
                cJSON_CreateNumber(defaults.battery_warning_v),
                make_range(0.0, 24.0));
    add_setting(settings,
                "battery_fault_v",
                "Battery Fault Voltage",
                "float",
                "V",
                cJSON_CreateNumber(defaults.battery_fault_low_v),
                cJSON_CreateNumber(defaults.battery_fault_low_v),
                make_range(0.0, 24.0));
    add_setting(settings,
                "battery_warning_enabled",
                "Battery Warning Enabled",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.battery_warning_enabled),
                cJSON_CreateBool(defaults.battery_warning_enabled),
                nullptr);
    add_setting(settings,
                "sensor_fault_detection_enabled",
                "Sensor Fault Detection Enabled",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.sensor_fault_detection_enabled),
                cJSON_CreateBool(defaults.sensor_fault_detection_enabled),
                nullptr);
    add_setting(settings,
                "fault_latch_enabled",
                "Fault Latch Enabled",
                "bool",
                nullptr,
                cJSON_CreateBool(defaults.fault_latch_enabled),
                cJSON_CreateBool(defaults.fault_latch_enabled),
                nullptr);
  });

  add_category(categories, "cycle_runtime", "Cycle Runtime", [&](cJSON *settings) {
    add_setting(settings,
                "abort_blower_duration_ms",
                "Abort Blower Duration",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.abort_blower_duration_ms),
                cJSON_CreateNumber(defaults.abort_blower_duration_ms),
                make_range(0, 600000));
    add_setting(settings,
                "countdown_total_ms",
                "Countdown Total",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.countdown_total_ms),
                cJSON_CreateNumber(defaults.countdown_total_ms),
                make_range(0, 30000));
    add_setting(settings,
                "countdown_step_ms",
                "Countdown Step",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.countdown_step_ms),
                cJSON_CreateNumber(defaults.countdown_step_ms),
                make_range(100, 10000));
    add_setting(settings,
                "cycle_total_duration_ms",
                "Cycle Total Duration",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.cycle_total_duration_ms),
                cJSON_CreateNumber(defaults.cycle_total_duration_ms),
                make_range(0, 3600000));
    add_setting(settings,
                "post_cycle_reminder_repeat_ms",
                "Reminder Repeat",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.post_cycle_reminder_repeat_ms),
                cJSON_CreateNumber(defaults.post_cycle_reminder_repeat_ms),
                make_range(1000, 600000));
    add_setting(settings,
                "post_cycle_reminder_tone_duration_ms",
                "Reminder Tone Duration",
                "uint32",
                "ms",
                cJSON_CreateNumber(defaults.post_cycle_reminder_tone_duration_ms),
                cJSON_CreateNumber(defaults.post_cycle_reminder_tone_duration_ms),
                make_range(50, 10000));
  });

  std::string json = print_json(root);
  cJSON_Delete(root);
  return json;
}

std::string default_cycle_profile_json() {
  return "{\n"
         "  \"schema_version\": 1,\n"
         "  \"glow_plug_duration_sec\": 150,\n"
         "  \"fan_steps\": [\n"
         "    {\n"
         "      \"start_sec\": 150,\n"
         "      \"target_throttle_percent\": 50,\n"
         "      \"ramp_duration_sec\": 5\n"
         "    }\n"
         "  ]\n"
         "}\n";
}

bool parse_cycle_profile_json_impl(const std::string &json,
                                   CycleProfileData &profile_out,
                                   std::string &message) {
  cJSON *root = cJSON_Parse(json.c_str());
  if (root == nullptr) {
    message = "Invalid cycle profile JSON";
    return false;
  }

  cJSON *glow_duration = cJSON_GetObjectItemCaseSensitive(root, "glow_plug_duration_sec");
  cJSON *fan_steps = cJSON_GetObjectItemCaseSensitive(root, "fan_steps");
  if (!cJSON_IsNumber(glow_duration) || glow_duration->valuedouble < 0.0 ||
      !cJSON_IsArray(fan_steps)) {
    cJSON_Delete(root);
    message = "cycle_profile.json missing required fields";
    return false;
  }

  profile_out.glow_plug_duration_sec = static_cast<uint32_t>(glow_duration->valuedouble);
  profile_out.fan_steps.clear();

  for (cJSON *step = fan_steps->child; step != nullptr; step = step->next) {
    cJSON *start_sec = cJSON_GetObjectItemCaseSensitive(step, "start_sec");
    cJSON *target = cJSON_GetObjectItemCaseSensitive(step, "target_throttle_percent");
    cJSON *ramp = cJSON_GetObjectItemCaseSensitive(step, "ramp_duration_sec");
    if (!cJSON_IsNumber(start_sec) || start_sec->valuedouble < 0.0 ||
        !cJSON_IsNumber(target) || target->valuedouble < 0.0 || target->valuedouble > 100.0 ||
        !cJSON_IsNumber(ramp) || ramp->valuedouble < 0.0) {
      cJSON_Delete(root);
      message = "cycle_profile.json contains an invalid fan step";
      return false;
    }

    profile_out.fan_steps.push_back({
        static_cast<uint32_t>(start_sec->valuedouble),
        static_cast<uint8_t>(target->valuedouble),
        static_cast<uint32_t>(ramp->valuedouble),
    });
  }

  cJSON_Delete(root);
  return true;
}

bool add_segment(std::array<IgnitionProfileSegment, kMaxIgnitionProfileSegments> &segments,
                 std::size_t &segment_count,
                 bool is_ramp,
                 uint32_t duration_ms,
                 float throttle_percent,
                 const char *label,
                 std::string &message) {
  if (segment_count >= segments.size()) {
    message = std::string("Too many ") + label + " segments";
    return false;
  }

  segments[segment_count++] = {is_ramp, duration_ms, throttle_percent};
  return true;
}

bool build_profile(const Settings &settings,
                   const CycleProfileData &cycle_profile,
                   IgnitionProfile &profile_out,
                   std::string &message) {
  profile_out = {};
  profile_out.valid = true;
  profile_out.total_duration_ms = settings.cycle_total_duration_ms;

  const uint32_t glow_duration_ms = cycle_profile.glow_plug_duration_sec * 1000U;
  if (settings.cycle_total_duration_ms < glow_duration_ms) {
    message = "cycle_total_duration_ms cannot be shorter than glow_plug_duration_sec";
    return false;
  }

  if (settings.glow_plug_ramp_enabled && glow_duration_ms > 0U) {
    if (settings.glow_plug_initial_throttle_percent > 0U) {
      if (!add_segment(profile_out.gp_segments,
                       profile_out.gp_segment_count,
                       false,
                       0,
                       static_cast<float>(settings.glow_plug_initial_throttle_percent),
                       "glow",
                       message)) {
        return false;
      }
    }

    const uint32_t ramp_duration_ms = std::min(settings.glow_plug_ramp_time_ms, glow_duration_ms);
    if (ramp_duration_ms > 0U &&
        settings.glow_plug_initial_throttle_percent != settings.glow_plug_max_throttle_percent) {
      if (!add_segment(profile_out.gp_segments,
                       profile_out.gp_segment_count,
                       true,
                       ramp_duration_ms,
                       0.0f,
                       "glow",
                       message) ||
          !add_segment(profile_out.gp_segments,
                       profile_out.gp_segment_count,
                       false,
                       0,
                       static_cast<float>(settings.glow_plug_max_throttle_percent),
                       "glow",
                       message)) {
        return false;
      }
    }

    if (glow_duration_ms > ramp_duration_ms) {
      if (!add_segment(profile_out.gp_segments,
                       profile_out.gp_segment_count,
                       false,
                       glow_duration_ms - ramp_duration_ms,
                       static_cast<float>(settings.glow_plug_max_throttle_percent),
                       "glow",
                       message)) {
        return false;
      }
    }
  } else if (glow_duration_ms > 0U) {
    if (!add_segment(profile_out.gp_segments,
                     profile_out.gp_segment_count,
                     false,
                     glow_duration_ms,
                     static_cast<float>(settings.glow_plug_max_throttle_percent),
                     "glow",
                     message)) {
      return false;
    }
  }

  uint32_t current_time_ms = 0;
  float current_throttle = 0.0f;
  for (const auto &step : cycle_profile.fan_steps) {
    const uint32_t start_ms = step.start_sec * 1000U;
    const uint32_t ramp_ms = step.ramp_duration_sec * 1000U;
    const uint32_t end_ms = start_ms + ramp_ms;
    if (start_ms < current_time_ms) {
      message = "Fan steps overlap or are out of order";
      return false;
    }
    if (end_ms > settings.cycle_total_duration_ms) {
      message = "cycle_total_duration_ms must be at or after the final fan ramp";
      return false;
    }

    if (start_ms > current_time_ms) {
      if (!add_segment(profile_out.fan_segments,
                       profile_out.fan_segment_count,
                       false,
                       start_ms - current_time_ms,
                       current_throttle,
                       "fan",
                       message)) {
        return false;
      }
      current_time_ms = start_ms;
    }

    if (ramp_ms > 0U && std::fabs(current_throttle - step.target_throttle_percent) > 0.01f) {
      if (!add_segment(profile_out.fan_segments,
                       profile_out.fan_segment_count,
                       true,
                       ramp_ms,
                       0.0f,
                       "fan",
                       message) ||
          !add_segment(profile_out.fan_segments,
                       profile_out.fan_segment_count,
                       false,
                       0,
                       static_cast<float>(step.target_throttle_percent),
                       "fan",
                       message)) {
        return false;
      }
      current_time_ms = end_ms;
      current_throttle = static_cast<float>(step.target_throttle_percent);
      continue;
    }

    if (std::fabs(current_throttle - step.target_throttle_percent) > 0.01f) {
      if (!add_segment(profile_out.fan_segments,
                       profile_out.fan_segment_count,
                       false,
                       0,
                       static_cast<float>(step.target_throttle_percent),
                       "fan",
                       message)) {
        return false;
      }
      current_throttle = static_cast<float>(step.target_throttle_percent);
    }
  }

  if (settings.cycle_total_duration_ms > current_time_ms) {
    if (!add_segment(profile_out.fan_segments,
                     profile_out.fan_segment_count,
                     false,
                     settings.cycle_total_duration_ms - current_time_ms,
                     current_throttle,
                     "fan",
                     message)) {
      return false;
    }
  }

  return true;
}

cJSON *find_setting_object(cJSON *root, const std::string &key) {
  cJSON *categories = cJSON_GetObjectItemCaseSensitive(root, "categories");
  if (!cJSON_IsArray(categories)) {
    return nullptr;
  }

  for (cJSON *category = categories->child; category != nullptr; category = category->next) {
    cJSON *settings = cJSON_GetObjectItemCaseSensitive(category, "settings");
    if (!cJSON_IsArray(settings)) {
      continue;
    }
    for (cJSON *setting = settings->child; setting != nullptr; setting = setting->next) {
      cJSON *candidate = cJSON_GetObjectItemCaseSensitive(setting, "key");
      if (cJSON_IsString(candidate) && candidate->valuestring != nullptr &&
          key == candidate->valuestring) {
        return setting;
      }
    }
  }

  return nullptr;
}

bool replace_setting_value(cJSON *setting, const std::string &value, std::string &message) {
  cJSON *input_type = cJSON_GetObjectItemCaseSensitive(setting, "input_type");
  if (!cJSON_IsString(input_type) || input_type->valuestring == nullptr) {
    message = "Setting input_type missing";
    return false;
  }

  cJSON *replacement = nullptr;
  char *end = nullptr;
  const std::string type = input_type->valuestring;
  if (type == "bool") {
    bool parsed = false;
    if (!parse_bool_text(value, parsed)) {
      message = "Invalid boolean value";
      return false;
    }
    replacement = cJSON_CreateBool(parsed);
  } else if (type == "uint8" || type == "uint32") {
    const unsigned long parsed = strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
      message = "Invalid integer value";
      return false;
    }
    replacement = cJSON_CreateNumber(static_cast<double>(parsed));
  } else if (type == "float") {
    const float parsed = strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
      message = "Invalid float value";
      return false;
    }
    replacement = cJSON_CreateNumber(static_cast<double>(parsed));
  } else {
    message = "Unsupported setting input_type";
    return false;
  }

  cJSON_ReplaceItemInObjectCaseSensitive(setting, "value", replacement);
  return true;
}

void log_effective_settings() {
  DEV_LOGI(kTag,
           "Settings loaded cycle_total_duration_ms=%lu glow_max=%u fan_max=%u abort_ms=%lu "
           "battery_fault=%.2f",
           static_cast<unsigned long>(g_settings.cycle_total_duration_ms),
           static_cast<unsigned>(g_settings.glow_plug_max_throttle_percent),
           static_cast<unsigned>(g_settings.fan_max_throttle_percent),
           static_cast<unsigned long>(g_settings.abort_blower_duration_ms),
           static_cast<double>(g_settings.battery_fault_low_v));
}

bool load_effective_settings() {
  g_settings = settings_defaults::settings();
  g_settings_schema_json = default_settings_schema_json();
  g_cycle_profile_json = default_cycle_profile_json();

  if (!mount_settings_partition()) {
    std::string message;
    CycleProfileData cycle_profile;
    parse_cycle_profile_json_impl(g_cycle_profile_json, cycle_profile, message);
    return build_profile(g_settings, cycle_profile, g_profile, message);
  }

  std::string candidate_settings_json;
  if (read_text_file(kSettingsFilePath, kSettingsFileMaxBytes, candidate_settings_json)) {
    Settings candidate_settings = settings_defaults::settings();
    std::string message;
    if (parse_settings_schema_json_impl(candidate_settings_json, candidate_settings, message)) {
      g_settings = candidate_settings;
      g_settings_schema_json = candidate_settings_json;
    } else {
      DEV_LOGW(kTag, "Failed parsing settings.json, rewriting defaults: %s", message.c_str());
      write_text_file(kSettingsFilePath, g_settings_schema_json);
    }
  } else {
    write_text_file(kSettingsFilePath, g_settings_schema_json);
  }

  CycleProfileData cycle_profile;
  std::string candidate_cycle_json;
  std::string cycle_message;
  if (read_text_file(kCycleProfileFilePath, kCycleProfileFileMaxBytes, candidate_cycle_json)) {
    if (parse_cycle_profile_json_impl(candidate_cycle_json, cycle_profile, cycle_message)) {
      g_cycle_profile_json = candidate_cycle_json;
    } else {
      DEV_LOGW(kTag,
               "Failed parsing cycle_profile.json, rewriting defaults: %s",
               cycle_message.c_str());
      g_cycle_profile_json = default_cycle_profile_json();
      parse_cycle_profile_json_impl(g_cycle_profile_json, cycle_profile, cycle_message);
      write_text_file(kCycleProfileFilePath, g_cycle_profile_json);
    }
  } else {
    g_cycle_profile_json = default_cycle_profile_json();
    parse_cycle_profile_json_impl(g_cycle_profile_json, cycle_profile, cycle_message);
    write_text_file(kCycleProfileFilePath, g_cycle_profile_json);
  }

  std::string profile_message;
  if (!build_profile(g_settings, cycle_profile, g_profile, profile_message)) {
    DEV_LOGW(kTag, "Invalid effective cycle profile, falling back to defaults: %s",
             profile_message.c_str());
    g_settings = settings_defaults::settings();
    g_settings_schema_json = default_settings_schema_json();
    g_cycle_profile_json = default_cycle_profile_json();
    parse_cycle_profile_json_impl(g_cycle_profile_json, cycle_profile, cycle_message);
    profile_message.clear();
    if (!build_profile(g_settings, cycle_profile, g_profile, profile_message)) {
      DEV_LOGE(kTag, "Failed building default ignition profile: %s", profile_message.c_str());
      return false;
    }
    write_text_file(kSettingsFilePath, g_settings_schema_json);
    write_text_file(kCycleProfileFilePath, g_cycle_profile_json);
  }

  log_effective_settings();
  return true;
}

}  // namespace

bool init() {
  if (g_initialized) {
    return true;
  }

  g_initialized = load_effective_settings();
  return g_initialized;
}

bool prepare_settings_partition_update() {
  unmount_settings_partition();
  return true;
}

bool finalize_settings_partition_update() {
  return load_effective_settings();
}

bool update_setting(const std::string &key, const std::string &value, std::string &message) {
  if (!mount_settings_partition()) {
    message = "Settings partition is not mounted";
    return false;
  }

  cJSON *root = cJSON_Parse(g_settings_schema_json.c_str());
  if (root == nullptr) {
    message = "Current settings schema is invalid";
    return false;
  }

  cJSON *setting = find_setting_object(root, key);
  if (setting == nullptr) {
    cJSON_Delete(root);
    message = "Unknown setting key";
    return false;
  }

  if (!replace_setting_value(setting, value, message)) {
    cJSON_Delete(root);
    return false;
  }

  const std::string updated_json = print_json(root);
  cJSON_Delete(root);
  if (updated_json.empty()) {
    message = "Failed serializing updated settings";
    return false;
  }

  return update_settings_schema_json(updated_json, message);
}

std::string settings_schema_json() {
  return g_settings_schema_json;
}

bool update_settings_schema_json(const std::string &json, std::string &message) {
  Settings candidate_settings = settings_defaults::settings();
  if (!parse_settings_schema_json_impl(json, candidate_settings, message)) {
    return false;
  }

  CycleProfileData cycle_profile;
  if (!parse_cycle_profile_json_impl(g_cycle_profile_json, cycle_profile, message)) {
    return false;
  }

  IgnitionProfile candidate_profile = {};
  if (!build_profile(candidate_settings, cycle_profile, candidate_profile, message)) {
    return false;
  }

  if (!mount_settings_partition()) {
    message = "Settings partition is not mounted";
    return false;
  }
  if (!write_text_file(kSettingsFilePath, json)) {
    message = "Failed writing settings.json";
    return false;
  }

  g_settings = candidate_settings;
  g_profile = candidate_profile;
  g_settings_schema_json = json;
  return true;
}

std::string cycle_profile_json() {
  return g_cycle_profile_json;
}

bool update_cycle_profile_json(const std::string &json, std::string &message) {
  CycleProfileData cycle_profile;
  if (!parse_cycle_profile_json_impl(json, cycle_profile, message)) {
    return false;
  }

  IgnitionProfile candidate_profile = {};
  if (!build_profile(g_settings, cycle_profile, candidate_profile, message)) {
    return false;
  }

  if (!mount_settings_partition()) {
    message = "Settings partition is not mounted";
    return false;
  }
  if (!write_text_file(kCycleProfileFilePath, json)) {
    message = "Failed writing cycle_profile.json";
    return false;
  }

  g_profile = candidate_profile;
  g_cycle_profile_json = json;
  return true;
}

const Settings &settings() {
  return g_settings;
}

const IgnitionProfile &active_profile() {
  return g_profile;
}

}  // namespace config_store
