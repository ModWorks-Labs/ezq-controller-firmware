#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "file_manager.h"
#include "serial_log.h"

namespace config {
namespace {

constexpr char kProfileSchema[] = "ezq-profile/v1";
constexpr char kProfilesDirectory[] = "/ignition-cycle-profiles";
constexpr char kDefaultProfileFilename[] = "default_profile.json";
constexpr char kLegacyRootDefaultProfilePath[] = "/default_profile.json";
constexpr char kBuiltInProfileLabel[] = "built-in fallback";
constexpr char kDevicePath[] = "/device.json";
constexpr size_t kMaxProfileStoragePathLength = 63;

IgnitionProfile g_ignition_profile = {
    true,
    3,
    4,
    375000,
    {
        {true, 5000, 0.0f},
        {false, 150000, 100.0f},
        {true, 2500, 0.0f},
    },
    {
        {false, 60000, 0.0f},
        {false, 90000, 5.0f},
        {true, 15000, 0.0f},
        {false, 210000, 100.0f},
    },
};

Settings g_settings = {
    "ezq-ctlr-a-v0.4.0-alpha",
    "EZQ-CTLR-A",
    "UNASSIGNED",
    "EZQ-CTLR-A",
    60000,
    kDefaultProfileFilename,
    90.0f,
    85.0f,
    30000,
    4,
    true,
};

String g_loaded_profile_filename = kBuiltInProfileLabel;

bool parseIgnitionProfileJsonText(const String &json_text,
                                  IgnitionProfile &profile,
                                  String *exported_at);

unsigned long readUnsignedLong(JsonVariantConst value, unsigned long fallback) {
  return value.is<unsigned long>() || value.is<int>() ? value.as<unsigned long>() : fallback;
}

String sanitizeWifiDeviceName(const String &name) {
  String sanitized = name;
  sanitized.trim();

  String filtered;
  filtered.reserve(sanitized.length());
  for (size_t i = 0; i < sanitized.length(); ++i) {
    const char c = sanitized.charAt(i);
    if (c >= 32 && c <= 126) {
      filtered += c;
    }
  }

  filtered.trim();
  if (filtered.length() == 0) {
    filtered = g_settings.board_id;
  }

  if (filtered.length() > 27) {
    filtered.remove(27);
    filtered.trim();
  }

  if (filtered.length() == 0) {
    filtered = "EZQ-CTLR-A";
  }

  return filtered;
}

unsigned int readUnsignedInt(JsonVariantConst value, unsigned int fallback) {
  return value.is<unsigned int>() || value.is<int>() ? value.as<unsigned int>() : fallback;
}

float readFloat(JsonVariantConst value, float fallback) {
  return value.is<float>() || value.is<double>() || value.is<int>()
             ? value.as<float>()
             : fallback;
}

unsigned int clampSoundVolume(unsigned int volume) {
  return volume > 4 ? 4 : volume;
}

float clampPercent(float percent) {
  if (percent < 0.0f) {
    return 0.0f;
  }

  if (percent > 100.0f) {
    return 100.0f;
  }

  return percent;
}

unsigned long secondsToMilliseconds(JsonVariantConst value, unsigned long fallback) {
  if (!(value.is<float>() || value.is<double>() || value.is<int>() || value.is<unsigned int>())) {
    return fallback;
  }

  const float seconds = value.as<float>();
  if (seconds <= 0.0f) {
    return 0;
  }

  return static_cast<unsigned long>(seconds * 1000.0f + 0.5f);
}

bool isSafeProfileFilename(const String &filename) {
  if (filename.length() == 0 || filename.length() > 63 || !filename.endsWith(".json")) {
    return false;
  }

  if (filename.indexOf("..") >= 0 || filename.indexOf('/') >= 0 || filename.indexOf('\\') >= 0) {
    return false;
  }

  for (size_t i = 0; i < filename.length(); ++i) {
    const char c = filename.charAt(i);
    const bool allowed =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.';
    if (!allowed) {
      return false;
    }
  }

  return true;
}

String sanitizeProfileFilename(const String &raw_filename) {
  String filename = raw_filename;
  filename.trim();

  if (filename.length() == 0) {
    filename = "profile.json";
  }

  int slash_index = filename.lastIndexOf('/');
  int backslash_index = filename.lastIndexOf('\\');
  const int split_index = slash_index > backslash_index ? slash_index : backslash_index;
  if (split_index >= 0) {
    filename = filename.substring(split_index + 1);
  }

  String sanitized;
  sanitized.reserve(filename.length());
  for (size_t i = 0; i < filename.length(); ++i) {
    const char c = filename.charAt(i);
    const bool allowed =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.';
    sanitized += allowed ? c : '-';
  }

  while (sanitized.indexOf("--") >= 0) {
    sanitized.replace("--", "-");
  }

  while (sanitized.startsWith("-") || sanitized.startsWith("_") || sanitized.startsWith(".")) {
    sanitized.remove(0, 1);
  }

  while (sanitized.endsWith("-") || sanitized.endsWith("_")) {
    sanitized.remove(sanitized.length() - 1);
  }

  if (!sanitized.endsWith(".json")) {
    const int extension_index = sanitized.lastIndexOf('.');
    if (extension_index >= 0) {
      sanitized.remove(extension_index);
    }
    if (sanitized.length() == 0) {
      sanitized = "profile";
    }
    sanitized += ".json";
  }

  if (sanitized.length() == 0) {
    sanitized = "profile.json";
  }

  return sanitized;
}

String buildProfilePath(const String &filename) {
  return String(kProfilesDirectory) + "/" + filename;
}

uint32_t hashString(const String &value) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < value.length(); ++i) {
    hash ^= static_cast<uint8_t>(value.charAt(i));
    hash *= 16777619UL;
  }
  return hash;
}

String normalizeListedProfilePath(const String &path) {
  if (path.startsWith("/")) {
    return path;
  }

  return String(kProfilesDirectory) + "/" + path;
}

String shrinkProfileFilenameForStorage(const String &filename) {
  const String full_path = buildProfilePath(filename);
  if (full_path.length() <= kMaxProfileStoragePathLength) {
    return filename;
  }

  const int extension_index = filename.lastIndexOf('.');
  const String extension = extension_index >= 0 ? filename.substring(extension_index) : ".json";
  String base = extension_index >= 0 ? filename.substring(0, extension_index) : filename;

  char hash_suffix[10] = {};
  snprintf(hash_suffix, sizeof(hash_suffix), "%08lx",
           static_cast<unsigned long>(hashString(filename)));

  const size_t directory_prefix_length = String(kProfilesDirectory).length() + 1;
  const size_t suffix_length = 1 + 8 + extension.length();
  size_t max_base_length = 1;
  if (kMaxProfileStoragePathLength > directory_prefix_length + suffix_length) {
    max_base_length = kMaxProfileStoragePathLength - directory_prefix_length - suffix_length;
  }

  if (base.length() > max_base_length) {
    base.remove(max_base_length);
  }
  if (base.length() == 0) {
    base = "profile";
  }

  return base + "-" + hash_suffix + extension;
}

bool migrateLegacyDefaultProfileIfNeeded() {
  const String target_path = buildProfilePath(kDefaultProfileFilename);
  if (pathExists(target_path.c_str())) {
    return true;
  }

  if (!pathExists(kLegacyRootDefaultProfilePath)) {
    return false;
  }

  if (!ensureDirectory(kProfilesDirectory)) {
    return false;
  }

  String json_text;
  if (!readFileText(kLegacyRootDefaultProfilePath, json_text)) {
    return false;
  }

  IgnitionProfile profile = {};
  if (!parseIgnitionProfileJsonText(json_text, profile, nullptr)) {
    return false;
  }

  return writeFileText(target_path.c_str(), json_text);
}

String profileFilenameFromPath(const String &path) {
  const String prefix = String(kProfilesDirectory) + "/";
  if (path.startsWith(prefix)) {
    return path.substring(prefix.length());
  }

  return path;
}

bool loadIgnitionSegments(JsonArrayConst segments_json,
                          IgnitionProfileSegment *segments,
                          unsigned int &segment_count,
                          unsigned long &total_duration_ms) {
  segment_count = 0;
  total_duration_ms = 0;

  for (JsonObjectConst segment_json : segments_json) {
    if (segment_count >= kMaxIgnitionProfileSegments) {
      return false;
    }

    const char *type = segment_json["type"] | "";
    const bool is_ramp = strcmp(type, "RAMP") == 0;
    const bool is_hold = strcmp(type, "HOLD") == 0;
    if (!is_ramp && !is_hold) {
      return false;
    }

    IgnitionProfileSegment &segment = segments[segment_count++];
    segment.is_ramp = is_ramp;
    segment.duration_ms = secondsToMilliseconds(segment_json["duration_s"], 0);
    segment.throttle_percent =
        is_hold ? clampPercent(readFloat(segment_json["throttle_pct"], 0.0f)) : 0.0f;

    total_duration_ms += segment.duration_ms;
  }

  return true;
}

bool parseIgnitionProfileJsonText(const String &json_text,
                                  IgnitionProfile &profile,
                                  String *exported_at) {
  DynamicJsonDocument doc(4096);
  const DeserializationError error = deserializeJson(doc, json_text);
  if (error) {
    return false;
  }

  const char *schema = doc["schema"] | "";
  if (strcmp(schema, kProfileSchema) != 0) {
    return false;
  }

  JsonArrayConst gp_segments_json = doc["gpSegments"].as<JsonArrayConst>();
  JsonArrayConst fan_segments_json = doc["fanSegments"].as<JsonArrayConst>();
  if (gp_segments_json.isNull() || fan_segments_json.isNull()) {
    return false;
  }

  profile = {};
  profile.valid = true;

  unsigned long gp_total_duration_ms = 0;
  unsigned long fan_total_duration_ms = 0;
  if (!loadIgnitionSegments(
          gp_segments_json, profile.gp_segments, profile.gp_segment_count, gp_total_duration_ms) ||
      !loadIgnitionSegments(
          fan_segments_json, profile.fan_segments, profile.fan_segment_count, fan_total_duration_ms)) {
    return false;
  }

  if (profile.gp_segment_count == 0 && profile.fan_segment_count == 0) {
    return false;
  }

  profile.total_duration_ms = gp_total_duration_ms > fan_total_duration_ms ? gp_total_duration_ms
                                                                            : fan_total_duration_ms;

  if (exported_at != nullptr) {
    *exported_at = String(doc["exported_at"] | "");
  }

  return true;
}

bool loadIgnitionProfileFromPath(const String &path, const String &filename_label) {
  String json_text;
  if (!readFileText(path.c_str(), json_text)) {
    return false;
  }

  IgnitionProfile profile = {};
  if (!parseIgnitionProfileJsonText(json_text, profile, nullptr)) {
    return false;
  }

  g_ignition_profile = profile;
  g_loaded_profile_filename = filename_label;
  return true;
}

void printIgnitionSegments(const char *label,
                           const IgnitionProfileSegment *segments,
                           unsigned int segment_count) {
  DebugSerial.print(label);
  DebugSerial.print(" segments: ");
  DebugSerial.println(segment_count);

  for (unsigned int i = 0; i < segment_count; ++i) {
    const IgnitionProfileSegment &segment = segments[i];
    DebugSerial.print("  [");
    DebugSerial.print(i);
    DebugSerial.print("] ");
    DebugSerial.print(segment.is_ramp ? "RAMP" : "HOLD");
    DebugSerial.print(" for ");
    DebugSerial.print(segment.duration_ms);
    DebugSerial.print(" ms");
    if (!segment.is_ramp) {
      DebugSerial.print(" @ ");
      DebugSerial.print(segment.throttle_percent, 1);
      DebugSerial.print("%");
    }
    DebugSerial.println();
  }
}

void printIgnitionProfileSummary() {
  DebugSerial.println();
  DebugSerial.println("Ignition profile summary");
  DebugSerial.print("Source: ");
  DebugSerial.println(g_loaded_profile_filename);
  DebugSerial.print("Total duration: ");
  DebugSerial.print(g_ignition_profile.total_duration_ms);
  DebugSerial.println(" ms");
  printIgnitionSegments("GP", g_ignition_profile.gp_segments, g_ignition_profile.gp_segment_count);
  printIgnitionSegments(
      "Fan", g_ignition_profile.fan_segments, g_ignition_profile.fan_segment_count);
  DebugSerial.println();
}

bool loadDeviceFromFile() {
  String json_text;
  if (!readFileText(kDevicePath, json_text)) {
    return false;
  }

  DynamicJsonDocument doc(1024);
  const DeserializationError error = deserializeJson(doc, json_text);
  if (error) {
    return false;
  }

  if (!doc["firmware_version"].isNull()) {
    g_settings.firmware_version = doc["firmware_version"].as<const char *>();
  }
  if (!doc["board_id"].isNull()) {
    g_settings.board_id = doc["board_id"].as<const char *>();
  }
  if (!doc["serial_number"].isNull()) {
    g_settings.serial_number = doc["serial_number"].as<const char *>();
  }
  if (!doc["wifi_device_name"].isNull()) {
    g_settings.wifi_device_name = sanitizeWifiDeviceName(doc["wifi_device_name"].as<const char *>());
  } else {
    g_settings.wifi_device_name = sanitizeWifiDeviceName(g_settings.board_id);
  }

  return true;
}

bool loadConfigFromFile(bool allow_legacy_identity_fallback) {
  String json_text;
  if (!readFileText("/config.json", json_text)) {
    return false;
  }

  DynamicJsonDocument doc(4096);
  const DeserializationError error = deserializeJson(doc, json_text);
  if (error) {
    return false;
  }

  JsonObjectConst state_handling =
      doc["state_handling"].isNull() ? doc["cycle"] : doc["state_handling"];
  JsonObjectConst safety = doc["safety"];
  JsonObjectConst ui = doc["ui"];

  if (allow_legacy_identity_fallback) {
    JsonObjectConst identity = doc["identity"];
    if (!identity["firmware_version"].isNull()) {
      g_settings.firmware_version = identity["firmware_version"].as<const char *>();
    }
    if (!identity["board_id"].isNull()) {
      g_settings.board_id = identity["board_id"].as<const char *>();
    }
    if (!identity["serial_number"].isNull()) {
      g_settings.serial_number = identity["serial_number"].as<const char *>();
    }
    if (!identity["wifi_device_name"].isNull()) {
      g_settings.wifi_device_name =
          sanitizeWifiDeviceName(identity["wifi_device_name"].as<const char *>());
    } else {
      g_settings.wifi_device_name = sanitizeWifiDeviceName(g_settings.board_id);
    }
  }

  g_settings.idle_sleep_timeout_ms = readUnsignedLong(
      state_handling["idle_sleep_timeout_ms"], g_settings.idle_sleep_timeout_ms);
  if (!state_handling["active_profile_filename"].isNull()) {
    g_settings.active_profile_filename =
        state_handling["active_profile_filename"].as<const char *>();
  }

  g_settings.temp_fault_high_c =
      readFloat(safety["temp_fault_high_c"], g_settings.temp_fault_high_c);
  g_settings.temp_fault_low_c =
      readFloat(safety["temp_fault_low_c"], g_settings.temp_fault_low_c);
  g_settings.abort_blower_duration_ms =
      readUnsignedLong(safety["abort_blower_duration_ms"], g_settings.abort_blower_duration_ms);

  g_settings.sound_volume =
      clampSoundVolume(readUnsignedInt(ui["sound_volume"], g_settings.sound_volume));
  if (!ui["button_press_beep_enabled"].isNull()) {
    g_settings.button_press_beep_enabled = ui["button_press_beep_enabled"].as<bool>();
  }

  return true;
}

bool saveConfigToFile() {
  DynamicJsonDocument doc(2048);

  JsonObject state_handling = doc.createNestedObject("state_handling");
  state_handling["idle_sleep_timeout_ms"] = g_settings.idle_sleep_timeout_ms;
  state_handling["active_profile_filename"] = g_settings.active_profile_filename;

  JsonObject safety = doc.createNestedObject("safety");
  safety["temp_fault_high_c"] = g_settings.temp_fault_high_c;
  safety["temp_fault_low_c"] = g_settings.temp_fault_low_c;
  safety["abort_blower_duration_ms"] = g_settings.abort_blower_duration_ms;

  JsonObject ui = doc.createNestedObject("ui");
  ui["sound_volume"] = clampSoundVolume(g_settings.sound_volume);
  ui["button_press_beep_enabled"] = g_settings.button_press_beep_enabled;

  String json_text;
  serializeJsonPretty(doc, json_text);
  return writeFileText("/config.json", json_text);
}

bool saveDeviceToFile() {
  DynamicJsonDocument doc(1024);
  doc["firmware_version"] = g_settings.firmware_version;
  doc["board_id"] = g_settings.board_id;
  doc["serial_number"] = g_settings.serial_number;
  doc["wifi_device_name"] = sanitizeWifiDeviceName(g_settings.wifi_device_name);

  String json_text;
  serializeJsonPretty(doc, json_text);
  return writeFileText(kDevicePath, json_text);
}

void sortProfileInfos(IgnitionProfileFileInfo *entries, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = i + 1; j < count; ++j) {
      if (entries[j].filename < entries[i].filename) {
        const IgnitionProfileFileInfo temp = entries[i];
        entries[i] = entries[j];
        entries[j] = temp;
      }
    }
  }
}

}  // namespace

void setupConfig() {
  ensureDirectory(kProfilesDirectory);
  migrateLegacyDefaultProfileIfNeeded();
  const bool loaded_device = loadDeviceFromFile();
  loadConfigFromFile(!loaded_device);
  g_settings.wifi_device_name = sanitizeWifiDeviceName(g_settings.wifi_device_name);
  if (!loaded_device) {
    saveDeviceToFile();
  }
  reloadIgnitionProfile();
  printIgnitionProfileSummary();
}

const Settings &getSettings() {
  return g_settings;
}

const IgnitionProfile &getIgnitionProfile() {
  return g_ignition_profile;
}

const String &getLoadedIgnitionProfileFilename() {
  return g_loaded_profile_filename;
}

const String &getWifiDeviceName() {
  return g_settings.wifi_device_name;
}

void setIdleSleepTimeoutMs(unsigned long duration_ms) {
  g_settings.idle_sleep_timeout_ms = duration_ms;
}

void setActiveProfileFilename(const String &filename) {
  if (isSafeProfileFilename(filename)) {
    g_settings.active_profile_filename = filename;
  }
}

const String &getActiveProfileFilename() {
  return g_settings.active_profile_filename;
}

void setWifiDeviceName(const String &name) {
  g_settings.wifi_device_name = sanitizeWifiDeviceName(name);
}

void setTempFaultHighC(float temperature_c) {
  g_settings.temp_fault_high_c = temperature_c;
}

void setTempFaultLowC(float temperature_c) {
  g_settings.temp_fault_low_c = temperature_c;
}

void setAbortBlowerDurationMs(unsigned long duration_ms) {
  g_settings.abort_blower_duration_ms = duration_ms;
}

void setSoundVolume(unsigned int volume) {
  g_settings.sound_volume = clampSoundVolume(volume);
}

void setButtonPressBeepEnabled(bool enabled) {
  g_settings.button_press_beep_enabled = enabled;
}

bool reloadIgnitionProfile() {
  ensureDirectory(kProfilesDirectory);
  migrateLegacyDefaultProfileIfNeeded();

  if (isSafeProfileFilename(g_settings.active_profile_filename)) {
    const String configured_path = buildProfilePath(g_settings.active_profile_filename);
    if (loadIgnitionProfileFromPath(configured_path, g_settings.active_profile_filename)) {
      return true;
    }
  }

  const String default_path = buildProfilePath(kDefaultProfileFilename);
  if (loadIgnitionProfileFromPath(default_path, kDefaultProfileFilename)) {
    return true;
  }

  if (loadIgnitionProfileFromPath(kLegacyRootDefaultProfilePath, kDefaultProfileFilename)) {
    return true;
  }

  g_loaded_profile_filename = kBuiltInProfileLabel;
  return g_ignition_profile.valid;
}

size_t listIgnitionProfiles(IgnitionProfileFileInfo *entries, size_t max_entries) {
  if (entries == nullptr || max_entries == 0) {
    return 0;
  }

  ensureDirectory(kProfilesDirectory);
  migrateLegacyDefaultProfileIfNeeded();

  String paths[kMaxIgnitionProfiles] = {};
  const size_t path_count =
      listFilesInDirectory(kProfilesDirectory, paths, kMaxIgnitionProfiles);

  size_t count = 0;
  for (size_t i = 0; i < path_count && count < max_entries; ++i) {
    const String path = normalizeListedProfilePath(paths[i]);
    const String filename = profileFilenameFromPath(path);
    if (!isSafeProfileFilename(filename)) {
      continue;
    }

    String json_text;
    if (!readFileText(path.c_str(), json_text)) {
      continue;
    }

    IgnitionProfile profile = {};
    String exported_at;
    if (!parseIgnitionProfileJsonText(json_text, profile, &exported_at)) {
      continue;
    }

    IgnitionProfileFileInfo &entry = entries[count++];
    entry.filename = filename;
    entry.exported_at = exported_at;
    entry.gp_segment_count = profile.gp_segment_count;
    entry.fan_segment_count = profile.fan_segment_count;
    entry.total_duration_ms = profile.total_duration_ms;
    entry.is_active = filename == g_settings.active_profile_filename;
    entry.is_default = filename == kDefaultProfileFilename;
  }

  sortProfileInfos(entries, count);
  return count;
}

bool saveIgnitionProfileFile(const String &filename,
                             const String &json_text,
                             String &error_message,
                             String *stored_filename) {
  error_message = "";

  const String sanitized_filename = sanitizeProfileFilename(filename);
  if (!isSafeProfileFilename(sanitized_filename)) {
    error_message = "Invalid profile filename.";
    return false;
  }

  IgnitionProfile profile = {};
  if (!parseIgnitionProfileJsonText(json_text, profile, nullptr)) {
    error_message = "Invalid ignition profile schema.";
    return false;
  }

  if (!ensureDirectory(kProfilesDirectory)) {
    error_message = "Failed to prepare profile directory.";
    return false;
  }

  const String storage_filename = shrinkProfileFilenameForStorage(sanitized_filename);
  const String storage_path = buildProfilePath(storage_filename);
  if (storage_path.length() > kMaxProfileStoragePathLength) {
    error_message = "Profile filename is too long for device storage.";
    return false;
  }

  if (!writeFileText(storage_path.c_str(), json_text)) {
    error_message = "Failed to write profile file.";
    return false;
  }

  if (stored_filename != nullptr) {
    *stored_filename = storage_filename;
  }

  return true;
}

bool deleteIgnitionProfileFile(const String &filename, String &error_message) {
  error_message = "";

  if (!isSafeProfileFilename(filename)) {
    error_message = "Invalid profile filename.";
    return false;
  }

  if (filename == kDefaultProfileFilename) {
    error_message = "Default profile cannot be deleted.";
    return false;
  }

  if (filename == g_settings.active_profile_filename) {
    error_message = "Active profile cannot be deleted.";
    return false;
  }

  const String path = buildProfilePath(filename);
  if (!pathExists(path.c_str())) {
    error_message = "Profile file not found.";
    return false;
  }

  if (!deleteFilePath(path.c_str())) {
    error_message = "Failed to delete profile file.";
    return false;
  }

  return true;
}

bool profileFileExists(const String &filename) {
  if (!isSafeProfileFilename(filename)) {
    return false;
  }

  return pathExists(buildProfilePath(filename).c_str());
}

bool saveConfig() {
  return saveConfigToFile();
}

bool saveDevice() {
  return saveDeviceToFile();
}

}  // namespace config
