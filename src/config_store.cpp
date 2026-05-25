#include "config_store.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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
constexpr std::size_t kSettingsFileMaxBytes = 2048;

Settings g_settings = {};

IgnitionProfile g_profile = {
    true,
    3,
    4,
    375000,
    {{
        {true, 5000, 0.0f},
        {false, 150000, 100.0f},
        {true, 2500, 0.0f},
    }},
    {{
        {false, 60000, 0.0f},
        {false, 90000, 5.0f},
        {true, 15000, 0.0f},
        {false, 210000, 100.0f},
    }},
};

bool g_initialized = false;
bool g_settings_fs_mounted = false;

bool mount_settings_partition() {
  if (g_settings_fs_mounted) {
    return true;
  }

  const esp_vfs_spiffs_conf_t conf = {
      .base_path = kSettingsBasePath,
      .partition_label = kSettingsPartitionLabel,
      .max_files = 4,
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

void log_effective_settings(const char *source_label) {
  DEV_LOGI(kTag,
           "Settings source=%s idle_sleep_ms=%lu abort_blower_duration_ms=%lu temp_high=%.1fC "
           "temp_low=%.1fC batt_warn=%.2fV batt_fault=%.2fV sound_volume=%u button_beep=%s "
           "profile=%s",
           source_label,
           static_cast<unsigned long>(g_settings.idle_sleep_timeout_ms),
           static_cast<unsigned long>(g_settings.abort_blower_duration_ms),
           static_cast<double>(g_settings.temp_fault_high_c),
           static_cast<double>(g_settings.temp_fault_low_c),
           static_cast<double>(g_settings.battery_warning_v),
           static_cast<double>(g_settings.battery_fault_low_v),
           static_cast<unsigned>(g_settings.sound_volume),
           g_settings.button_press_beep_enabled ? "true" : "false",
           g_settings.active_profile_name.data());
}

std::string serialize_settings_json(const Settings &settings) {
  char buffer[512] = {};
  const int written = snprintf(
      buffer,
      sizeof(buffer),
      "{\n"
      "  \"state_handling\": {\n"
      "    \"idle_sleep_timeout_ms\": %lu\n"
      "  },\n"
      "  \"safety\": {\n"
      "    \"temp_fault_high_c\": %.1f,\n"
      "    \"temp_fault_low_c\": %.1f,\n"
      "    \"battery_warning_v\": %.1f,\n"
      "    \"battery_fault_v\": %.1f,\n"
      "    \"abort_blower_duration_ms\": %lu\n"
      "  },\n"
      "  \"ui\": {\n"
      "    \"sound_volume\": %u,\n"
      "    \"button_press_beep_enabled\": %s\n"
      "  }\n"
      "}\n",
      static_cast<unsigned long>(settings.idle_sleep_timeout_ms),
      static_cast<double>(settings.temp_fault_high_c),
      static_cast<double>(settings.temp_fault_low_c),
      static_cast<double>(settings.battery_warning_v),
      static_cast<double>(settings.battery_fault_low_v),
      static_cast<unsigned long>(settings.abort_blower_duration_ms),
      static_cast<unsigned>(settings.sound_volume),
      settings.button_press_beep_enabled ? "true" : "false");

  return written > 0 ? std::string(buffer, static_cast<std::size_t>(written)) : std::string();
}

bool write_settings_file(const Settings &settings) {
  FILE *file = fopen(kSettingsFilePath, "wb");
  if (file == nullptr) {
    DEV_LOGW(kTag, "Failed to open settings file at %s", kSettingsFilePath);
    return false;
  }

  const std::string json = serialize_settings_json(settings);
  if (json.empty()) {
    fclose(file);
    DEV_LOGW(kTag, "Failed serializing settings JSON");
    return false;
  }

  fwrite(json.data(), 1, json.size(), file);
  fclose(file);
  return true;
}

void write_default_settings_file() {
  if (!write_settings_file(g_settings)) {
    DEV_LOGW(kTag, "Failed to create default settings file at %s", kSettingsFilePath);
  }
}

void overlay_number_u32(cJSON *object, const char *key, uint32_t &target) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item) && item->valuedouble >= 0.0) {
    target = static_cast<uint32_t>(item->valuedouble);
  }
}

void overlay_number_float(cJSON *object, const char *key, float &target) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item)) {
    target = static_cast<float>(item->valuedouble);
  }
}

void overlay_bool(cJSON *object, const char *key, bool &target) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsBool(item)) {
    target = cJSON_IsTrue(item);
  }
}

void overlay_sound_volume(cJSON *object, const char *key, uint8_t &target) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= 3.0) {
    target = static_cast<uint8_t>(item->valuedouble);
  }
}

bool load_settings_from_json(const std::string &json_text) {
  cJSON *root = cJSON_Parse(json_text.c_str());
  if (root == nullptr) {
    DEV_LOGW(kTag, "settings.json parse failed");
    return false;
  }

  cJSON *state_handling = cJSON_GetObjectItemCaseSensitive(root, "state_handling");
  if (cJSON_IsObject(state_handling)) {
    overlay_number_u32(state_handling, "idle_sleep_timeout_ms", g_settings.idle_sleep_timeout_ms);
  }

  cJSON *safety = cJSON_GetObjectItemCaseSensitive(root, "safety");
  if (cJSON_IsObject(safety)) {
    overlay_number_float(safety, "temp_fault_high_c", g_settings.temp_fault_high_c);
    overlay_number_float(safety, "temp_fault_low_c", g_settings.temp_fault_low_c);
    overlay_number_float(safety, "battery_warning_v", g_settings.battery_warning_v);
    overlay_number_float(safety, "battery_fault_v", g_settings.battery_fault_low_v);
    overlay_number_u32(safety,
                       "abort_blower_duration_ms",
                       g_settings.abort_blower_duration_ms);
  }

  cJSON *ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
  if (cJSON_IsObject(ui)) {
    overlay_sound_volume(ui, "sound_volume", g_settings.sound_volume);
    overlay_bool(ui, "button_press_beep_enabled", g_settings.button_press_beep_enabled);
  }

  cJSON_Delete(root);
  return true;
}

bool load_settings_file() {
  FILE *file = fopen(kSettingsFilePath, "rb");
  if (file == nullptr) {
    DEV_LOGW(kTag, "No settings file found at %s; writing defaults", kSettingsFilePath);
    write_default_settings_file();
    return false;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    DEV_LOGW(kTag, "Failed seeking settings file");
    return false;
  }

  const long size = ftell(file);
  if (size <= 0 || static_cast<std::size_t>(size) > kSettingsFileMaxBytes) {
    fclose(file);
    DEV_LOGW(kTag, "Settings file size invalid: %ld", size);
    return false;
  }

  rewind(file);
  std::string json_text(static_cast<std::size_t>(size), '\0');
  const std::size_t read = fread(json_text.data(), 1, json_text.size(), file);
  fclose(file);

  if (read != json_text.size()) {
    DEV_LOGW(kTag, "Settings file read incomplete");
    return false;
  }

  return load_settings_from_json(json_text);
}

bool load_effective_settings() {
  g_settings = settings_defaults::settings();

  const bool mounted = mount_settings_partition();
  bool loaded_from_file = false;
  if (mounted) {
    loaded_from_file = load_settings_file();
  }

  log_effective_settings(loaded_from_file ? "settings.json" : "compiled_defaults");
  return true;
}

bool parse_bool(const std::string &value, bool &parsed) {
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

}  // namespace

bool init() {
  if (g_initialized) {
    return true;
  }

  load_effective_settings();
  g_initialized = true;
  return true;
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

  Settings updated = g_settings;

  char *end = nullptr;
  if (key == "idle_sleep_timeout_ms") {
    const auto parsed = strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
      message = "Invalid integer value";
      return false;
    }
    updated.idle_sleep_timeout_ms = static_cast<uint32_t>(parsed);
  } else if (key == "abort_blower_duration_ms") {
    const auto parsed = strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
      message = "Invalid integer value";
      return false;
    }
    updated.abort_blower_duration_ms = static_cast<uint32_t>(parsed);
  } else if (key == "temp_fault_high_c") {
    const auto parsed = strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
      message = "Invalid float value";
      return false;
    }
    updated.temp_fault_high_c = parsed;
  } else if (key == "temp_fault_low_c") {
    const auto parsed = strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
      message = "Invalid float value";
      return false;
    }
    updated.temp_fault_low_c = parsed;
  } else if (key == "battery_warning_v") {
    const auto parsed = strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
      message = "Invalid float value";
      return false;
    }
    updated.battery_warning_v = parsed;
  } else if (key == "battery_fault_v") {
    const auto parsed = strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
      message = "Invalid float value";
      return false;
    }
    updated.battery_fault_low_v = parsed;
  } else if (key == "sound_volume") {
    const auto parsed = strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed > 3) {
      message = "Invalid sound volume (expected 0-3)";
      return false;
    }
    updated.sound_volume = static_cast<uint8_t>(parsed);
  } else if (key == "button_press_beep_enabled") {
    bool parsed = false;
    if (!parse_bool(value, parsed)) {
      message = "Invalid boolean value";
      return false;
    }
    updated.button_press_beep_enabled = parsed;
  } else {
    message = "Unknown setting key";
    return false;
  }

  if (!write_settings_file(updated)) {
    message = "Failed writing settings.json";
    return false;
  }

  g_settings = updated;
  message = "Updated " + key + "=" + value;
  return true;
}

const Settings &settings() {
  return g_settings;
}

const IgnitionProfile &active_profile() {
  return g_profile;
}

}  // namespace config_store
