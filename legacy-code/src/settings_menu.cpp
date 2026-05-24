#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp32-hal-rgb-led.h>

#include "config.h"
#include "cycle_manager.h"
#include "fan.h"
#include "file_manager.h"
#include "glow_plug.h"
#include "pinout.h"
#include "serial_log.h"
#include "settings_menu.h"
#include "temp.h"
#include "ui.h"

namespace {

constexpr int kBuzzerChannel = 0;
constexpr int kBuzzerResolutionBits = 10;
constexpr unsigned long kMenuDoubleClickWindowMs = 400;
constexpr unsigned long kMenuHoldMs = 2000;
constexpr unsigned long kMenuInactivityTimeoutMs = 60000;
constexpr unsigned long kEditCommitDelayMs = 2000;
constexpr unsigned long kEditCancelTimeoutMs = 10000;
constexpr unsigned long kBlinkIntervalMs = 300;
constexpr unsigned long kMenuEntryDurationMs = 450;
constexpr unsigned long kMenuEntryFlashIntervalMs = 120;
constexpr unsigned long kEntryBeepDurationMs = 70;
constexpr unsigned long kEntryBeepGapMs = 110;
constexpr unsigned long kReadbackBeepDurationMs = 110;
constexpr unsigned long kReadbackBeepGapMs = 180;
constexpr unsigned long kOtaRestartDelayMs = 1200;
constexpr unsigned long kAutoPortalStartDelayMs = 1500;
constexpr unsigned long kPortalActivityKeepAwakeMs = 15000;
constexpr uint8_t kLedBrightness = 32;
constexpr char kOtaApNameSuffix[] = "-OTA";
constexpr char kOtaPortalPath[] = "/update";
constexpr char kOtaFilesystemPath[] = "/updatefs";
constexpr char kDashboardPath[] = "/dashboard.html";
constexpr char kTemplateFirmwareVersion[] = "__FIRMWARE_VERSION__";
constexpr char kTemplateBoardId[] = "__BOARD_ID__";
constexpr char kTemplateSerialNumber[] = "__SERIAL_NUMBER__";
constexpr char kTemplateApStatus[] = "__AP_STATUS__";
constexpr char kStateHandlingKey[] = "state_handling";
constexpr size_t kMaxProfileUploadBytes = 16384;

struct RgbColor {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

enum class MenuState {
  INACTIVE,
  SETTINGS_MENU,
  SETTINGS_EDIT,
  WEB_PORTAL_ACTIVE,
};

enum SettingIndex {
  kPortalSetting = 0,
  kIdleSleepSetting = 1,
  kAbortBlowerSetting = 2,
  kSoundSetting = 3,
  kSettingCount = 4,
};

struct BeepSequence {
  bool active;
  bool output_on;
  unsigned int beeps_remaining;
  unsigned int volume_level;
  unsigned long on_duration_ms;
  unsigned long gap_duration_ms;
  unsigned long deadline_ms;
};

struct UploadStatus {
  bool success;
  bool error;
  String error_message;
  String success_message;
};

constexpr RgbColor kMenuEntryColor = {kLedBrightness, kLedBrightness, kLedBrightness};
constexpr RgbColor kSettingColors[kSettingCount] = {
    {kLedBrightness, kLedBrightness, kLedBrightness},
    {kLedBrightness, 12, 0},
    {0, 0, kLedBrightness},
    {0, 28, 32},
};

MenuState g_state = MenuState::INACTIVE;
int g_setting_index = 0;
unsigned long g_last_interaction_ms = 0;

unsigned long g_menu_last_click_ms = 0;
bool g_menu_single_click_pending = false;
int g_menu_pending_origin_setting = 0;
bool g_menu_pending_exit = false;
bool g_menu_intro_active = false;
unsigned long g_menu_intro_deadline_ms = 0;

unsigned int g_edit_pending_count = 0;
unsigned long g_edit_entered_ms = 0;
unsigned long g_edit_last_click_ms = 0;
bool g_edit_blink_on = true;
unsigned long g_edit_blink_deadline_ms = 0;

bool g_button_down = false;
unsigned long g_button_press_start_ms = 0;
bool g_hold_handled = false;

BeepSequence g_beep_sequence = {false, false, 0, 0, 0, 0, 0};
WebServer g_ota_server(80);
bool g_routes_registered = false;
bool g_ota_ap_active = false;
String g_ota_ap_ssid;
IPAddress g_ota_ap_ip;
bool g_ota_restart_pending = false;
unsigned long g_ota_restart_deadline_ms = 0;
unsigned long g_ready_idle_portal_armed_ms = 0;
unsigned long g_last_portal_activity_ms = 0;

UploadStatus g_firmware_upload = {false, false, "", "Firmware uploaded. Rebooting device."};
UploadStatus g_filesystem_upload = {
    false, false, "", "Filesystem image uploaded. Rebooting device."};
UploadStatus g_profile_upload = {false, false, "", "Profile uploaded successfully."};
String g_profile_upload_filename;
String g_profile_upload_contents;

void applyColor(const RgbColor &color) {
  neopixelWrite(pinout::kLedPwmPin, color.red, color.green, color.blue);
}

unsigned int clampVolumeLevel(unsigned int volume_level) {
  return volume_level > 4 ? 4 : volume_level;
}

void stopBeep() {
  ledcWrite(kBuzzerChannel, 0);
  g_beep_sequence.active = false;
  g_beep_sequence.output_on = false;
}

unsigned int getBuzzerDutyForVolumeLevel(unsigned int volume_level) {
  return ui_config::kBuzzerDutyByVolume[clampVolumeLevel(volume_level)];
}

void driveBeep(unsigned int volume_level) {
  ledcWriteTone(kBuzzerChannel, ui_config::kBuzzerFrequencyHz);
  ledcWrite(kBuzzerChannel, getBuzzerDutyForVolumeLevel(volume_level));
}

void startCountBeepSequence(unsigned int count,
                            unsigned int volume_level,
                            unsigned long on_duration_ms,
                            unsigned long gap_duration_ms) {
  g_beep_sequence.active = count > 0;
  g_beep_sequence.output_on = count > 0;
  g_beep_sequence.beeps_remaining = count;
  g_beep_sequence.volume_level = clampVolumeLevel(volume_level);
  g_beep_sequence.on_duration_ms = on_duration_ms;
  g_beep_sequence.gap_duration_ms = gap_duration_ms;
  g_beep_sequence.deadline_ms = millis() + on_duration_ms;

  if (count == 0) {
    stopBeep();
    return;
  }

  driveBeep(g_beep_sequence.volume_level);
}

void updateBeepSequence() {
  if (!g_beep_sequence.active) {
    return;
  }

  const unsigned long now = millis();
  if (now < g_beep_sequence.deadline_ms) {
    return;
  }

  if (g_beep_sequence.output_on) {
    ledcWrite(kBuzzerChannel, 0);
    g_beep_sequence.output_on = false;
    --g_beep_sequence.beeps_remaining;

    if (g_beep_sequence.beeps_remaining == 0) {
      g_beep_sequence.active = false;
      return;
    }

    g_beep_sequence.deadline_ms = now + g_beep_sequence.gap_duration_ms;
    return;
  }

  driveBeep(g_beep_sequence.volume_level);
  g_beep_sequence.output_on = true;
  g_beep_sequence.deadline_ms = now + g_beep_sequence.on_duration_ms;
}

void clearPendingMenuSingleClick() {
  g_menu_single_click_pending = false;
  g_menu_last_click_ms = 0;
  g_menu_pending_origin_setting = 0;
  g_menu_pending_exit = false;
}

void clearStateGestureTracking() {
  clearPendingMenuSingleClick();
  g_edit_pending_count = 0;
  g_edit_entered_ms = 0;
  g_edit_last_click_ms = 0;
  g_button_down = false;
  g_button_press_start_ms = 0;
  g_hold_handled = false;
}

void resetEditBlink() {
  g_edit_blink_on = true;
  g_edit_blink_deadline_ms = millis() + kBlinkIntervalMs;
}

void updateEditBlink() {
  if (g_state != MenuState::SETTINGS_EDIT) {
    return;
  }

  const unsigned long now = millis();
  if (now < g_edit_blink_deadline_ms) {
    return;
  }

  do {
    g_edit_blink_deadline_ms += kBlinkIntervalMs;
    g_edit_blink_on = !g_edit_blink_on;
  } while (now >= g_edit_blink_deadline_ms);
}

String buildOtaApSsid() {
  String ssid = config::getWifiDeviceName();
  if (ssid.length() == 0) {
    ssid = config::getSettings().board_id;
  }
  if (ssid.length() == 0) {
    ssid = "EZQ-CTLR-A";
  }
  ssid += kOtaApNameSuffix;
  return ssid;
}

String extractUploadFilename(const String &raw_filename) {
  int slash_index = raw_filename.lastIndexOf('/');
  int backslash_index = raw_filename.lastIndexOf('\\');
  const int split_index = slash_index > backslash_index ? slash_index : backslash_index;
  if (split_index < 0) {
    return raw_filename;
  }

  return raw_filename.substring(split_index + 1);
}

String buildDashboardHtml() {
  String html;
  if (!readFileText(kDashboardPath, html)) {
    html =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>EZQ Portal</title></head><body>"
        "<h1>EZQ Web Portal</h1><p>Board: " + config::getSettings().board_id +
        "</p><p>Serial: " + config::getSettings().serial_number +
        "</p><p>Wi-Fi Name: " + config::getWifiDeviceName() +
        "</p><p>Firmware: " + config::getSettings().firmware_version +
        "</p><p>Open the full dashboard asset in LittleFS to use the portal.</p></body></html>";
    return html;
  }

  html.replace(kTemplateFirmwareVersion, config::getSettings().firmware_version);
  html.replace(kTemplateBoardId, config::getSettings().board_id);
  html.replace(kTemplateSerialNumber, config::getSettings().serial_number);
  html.replace(kTemplateApStatus, g_ota_ap_ssid);
  return html;
}

void sendDashboardPage() {
  g_last_portal_activity_ms = millis();
  g_ota_server.send(200, "text/html", buildDashboardHtml());
}

const char *getContentTypeForPath(const String &path) {
  if (path.endsWith(".png")) {
    return "image/png";
  }
  if (path.endsWith(".svg")) {
    return "image/svg+xml";
  }
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) {
    return "image/jpeg";
  }
  if (path.endsWith(".webp")) {
    return "image/webp";
  }
  if (path.endsWith(".gif")) {
    return "image/gif";
  }
  if (path.endsWith(".css")) {
    return "text/css";
  }
  if (path.endsWith(".js")) {
    return "application/javascript";
  }
  if (path.endsWith(".json")) {
    return "application/json";
  }

  return "application/octet-stream";
}

bool tryServeStaticFile(const String &uri) {
  if (uri.length() == 0 || uri == "/" || uri.startsWith("/api/")) {
    return false;
  }

  if (!pathExists(uri.c_str())) {
    return false;
  }

  File file = LittleFS.open(uri, "r");
  if (!file) {
    return false;
  }

  g_last_portal_activity_ms = millis();
  g_ota_server.streamFile(file, getContentTypeForPath(uri));
  file.close();
  return true;
}

void sendJsonDocument(int status_code, DynamicJsonDocument &doc) {
  g_last_portal_activity_ms = millis();
  String body;
  serializeJson(doc, body);
  g_ota_server.send(status_code, "application/json", body);
}

void sendJsonMessage(int status_code, bool ok, const String &message) {
  DynamicJsonDocument doc(256);
  doc["ok"] = ok;
  doc["message"] = message;
  sendJsonDocument(status_code, doc);
}

bool parseJsonBody(DynamicJsonDocument &doc, String &error_message) {
  error_message = "";

  if (!g_ota_server.hasArg("plain")) {
    error_message = "Missing JSON body.";
    return false;
  }

  const DeserializationError error = deserializeJson(doc, g_ota_server.arg("plain"));
  if (error) {
    error_message = "Invalid JSON body.";
    return false;
  }

  return true;
}

void resetUploadStatus(UploadStatus &status) {
  status.success = false;
  status.error = false;
  status.error_message = "";
}

void handleUploadFinished(UploadStatus &status, const String &default_failure_message) {
  if (status.success) {
    sendJsonMessage(200, true, status.success_message);
    g_ota_restart_pending = true;
    g_ota_restart_deadline_ms = millis() + kOtaRestartDelayMs;
    return;
  }

  String message = status.error_message;
  if (message.length() == 0) {
    message = default_failure_message;
  }

  sendJsonMessage(500, false, message);
}

void handleUploadChunk(UploadStatus &status, int update_command, const String &aborted_message) {
  HTTPUpload &upload = g_ota_server.upload();

  switch (upload.status) {
    case UPLOAD_FILE_START:
      resetUploadStatus(status);
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, update_command)) {
        status.error = true;
        status.error_message = Update.errorString();
      }
      break;

    case UPLOAD_FILE_WRITE:
      if (!status.error &&
          Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        status.error = true;
        status.error_message = Update.errorString();
      }
      break;

    case UPLOAD_FILE_END:
      if (!status.error && Update.end(true)) {
        status.success = true;
      } else {
        status.error = true;
        if (status.error_message.length() == 0) {
          status.error_message = Update.errorString();
        }
      }
      break;

    case UPLOAD_FILE_ABORTED:
      status.error = true;
      status.error_message = aborted_message;
      Update.abort();
      break;

    default:
      break;
  }
}

void handleFirmwareUpdateFinished() {
  handleUploadFinished(g_firmware_upload, "Firmware upload failed.");
}

void handleFirmwareUpdateUpload() {
  handleUploadChunk(g_firmware_upload, U_FLASH, "Firmware upload aborted.");
}

void handleFilesystemUpdateFinished() {
  handleUploadFinished(g_filesystem_upload, "Filesystem image upload failed.");
}

void handleFilesystemUpdateUpload() {
  handleUploadChunk(g_filesystem_upload, U_SPIFFS, "Filesystem image upload aborted.");
}

bool shouldReloadProfileNow() {
  const CycleRuntimeStatus status = getCycleRuntimeStatus();
  return status.mode != CycleRuntimeMode::COUNTDOWN &&
         status.mode != CycleRuntimeMode::IGNITION &&
         status.mode != CycleRuntimeMode::COOLDOWN;
}

void sendStatusJson() {
  DynamicJsonDocument doc(1024);
  const CycleRuntimeStatus status = getCycleRuntimeStatus();

  doc["mode"] = status.mode_name;
  doc["state_label"] = status.state_label;
  doc["elapsed_ms"] = status.elapsed_ms;
  if (status.has_total_ms) {
    doc["total_ms"] = status.total_ms;
  } else {
    doc["total_ms"] = nullptr;
  }

  float temperature_c = 0.0f;
  float temperature_f = 0.0f;
  if (getCurrentTemperatureC(temperature_c)) {
    doc["temperature_c"] = temperature_c;
  } else {
    doc["temperature_c"] = nullptr;
  }

  if (getCurrentTemperatureF(temperature_f)) {
    doc["temperature_f"] = temperature_f;
  } else {
    doc["temperature_f"] = nullptr;
  }

  doc["fan_throttle_percent"] = getFanThrottlePercent();
  doc["glow_plug_percent"] = getGlowPlugPercent();
  doc["active_profile_filename"] = config::getActiveProfileFilename();
  doc["loaded_profile_filename"] = config::getLoadedIgnitionProfileFilename();
  doc["can_start_cycle"] = status.can_start_cycle;
  doc["can_start_blower"] = status.can_start_blower;
  doc["can_abort_cycle"] = status.can_abort_cycle;
  doc["can_stop_blower"] = status.can_stop_blower;
  doc["can_manual_control"] = status.can_manual_control;
  doc["can_stop_manual_control"] = status.can_stop_manual_control;
  doc["is_fault"] = status.is_fault;
  doc["is_abort_cooling"] = status.is_abort_cooling;
  doc["ap_active"] = g_ota_ap_active;
  if (g_ota_ap_active) {
    doc["ap_ssid"] = g_ota_ap_ssid;
  } else {
    doc["ap_ssid"] = nullptr;
  }
  doc["wifi_device_name"] = config::getWifiDeviceName();

  doc["board_id"] = config::getSettings().board_id;
  doc["serial_number"] = config::getSettings().serial_number;
  doc["firmware_version"] = config::getSettings().firmware_version;

  sendJsonDocument(200, doc);
}

void handleControlStartCycle() {
  if (!requestWebStartCycle()) {
    sendJsonMessage(409, false, "Cycle start is not valid in the current state.");
    return;
  }

  sendJsonMessage(200, true, "Cycle countdown started.");
}

void handleControlStartBlower() {
  if (!requestWebStartBlower()) {
    sendJsonMessage(409, false, "Blower mode is not valid in the current state.");
    return;
  }

  sendJsonMessage(200, true, "Blower mode started.");
}

void handleControlAbortCycle() {
  if (!requestWebAbortCycle()) {
    sendJsonMessage(409, false, "Abort is not valid in the current state.");
    return;
  }

  sendJsonMessage(200, true, "Cycle abort accepted.");
}

void handleControlStopBlower() {
  if (!requestWebStopBlower()) {
    sendJsonMessage(409, false, "Blower stop is not valid in the current state.");
    return;
  }

  sendJsonMessage(200, true, "Blower mode stopped.");
}

void handleControlStartManual() {
  if (!requestWebEnterManualControl()) {
    sendJsonMessage(409, false, "Quick test mode can only be entered from ready idle.");
    return;
  }

  sendJsonMessage(200, true, "Quick test mode active.");
}

void handleControlManualOutput() {
  DynamicJsonDocument doc(512);
  String error_message;
  if (!parseJsonBody(doc, error_message)) {
    sendJsonMessage(400, false, error_message);
    return;
  }

  const float glow_plug_percent = doc["glow_plug_percent"] | 0.0f;
  const float fan_throttle_percent = doc["fan_throttle_percent"] | 0.0f;
  if (!requestWebSetManualOutputs(glow_plug_percent, fan_throttle_percent)) {
    sendJsonMessage(409, false, "Quick test output control is not valid in the current state.");
    return;
  }

  sendJsonMessage(200, true, "Quick test outputs updated.");
}

void handleControlStopManual() {
  if (!requestWebStopManualOutputs()) {
    sendJsonMessage(409, false, "Quick test outputs are not active.");
    return;
  }

  sendJsonMessage(200, true, "Quick test outputs stopped.");
}

void handleGetConfig() {
  DynamicJsonDocument doc(1024);

  JsonObject device = doc.createNestedObject("device");
  device["firmware_version"] = config::getSettings().firmware_version;
  device["board_id"] = config::getSettings().board_id;
  device["serial_number"] = config::getSettings().serial_number;
  device["wifi_device_name"] = config::getWifiDeviceName();

  JsonObject state_handling = doc.createNestedObject(kStateHandlingKey);
  state_handling["idle_sleep_timeout_ms"] = config::getSettings().idle_sleep_timeout_ms;
  state_handling["active_profile_filename"] = config::getActiveProfileFilename();

  JsonObject safety = doc.createNestedObject("safety");
  safety["temp_fault_high_c"] = config::getSettings().temp_fault_high_c;
  safety["temp_fault_low_c"] = config::getSettings().temp_fault_low_c;
  safety["abort_blower_duration_ms"] = config::getSettings().abort_blower_duration_ms;

  JsonObject ui = doc.createNestedObject("ui");
  ui["sound_volume"] = config::getSettings().sound_volume;
  ui["button_press_beep_enabled"] = config::getSettings().button_press_beep_enabled;

  sendJsonDocument(200, doc);
}

void handlePostConfig() {
  DynamicJsonDocument doc(1536);
  String error_message;
  if (!parseJsonBody(doc, error_message)) {
    sendJsonMessage(400, false, error_message);
    return;
  }

  JsonObjectConst state_handling =
      doc[kStateHandlingKey].isNull() ? doc["cycle"] : doc[kStateHandlingKey];
  JsonObjectConst device = doc["device"];
  JsonObjectConst safety = doc["safety"];
  JsonObjectConst ui = doc["ui"];

  bool device_changed = false;
  bool profile_changed = false;
  if (!device["wifi_device_name"].isNull()) {
    config::setWifiDeviceName(device["wifi_device_name"].as<const char *>());
    device_changed = true;
  }
  if (!state_handling["idle_sleep_timeout_ms"].isNull()) {
    config::setIdleSleepTimeoutMs(state_handling["idle_sleep_timeout_ms"].as<unsigned long>());
  }

  if (!state_handling["active_profile_filename"].isNull()) {
    const String requested_profile =
        state_handling["active_profile_filename"].as<const char *>();
    if (!config::profileFileExists(requested_profile)) {
      sendJsonMessage(400, false, "Selected active profile file was not found.");
      return;
    }

    config::setActiveProfileFilename(requested_profile);
    profile_changed = true;
  }

  if (!safety["temp_fault_high_c"].isNull()) {
    config::setTempFaultHighC(safety["temp_fault_high_c"].as<float>());
  }
  if (!safety["temp_fault_low_c"].isNull()) {
    config::setTempFaultLowC(safety["temp_fault_low_c"].as<float>());
  }
  if (!safety["abort_blower_duration_ms"].isNull()) {
    config::setAbortBlowerDurationMs(safety["abort_blower_duration_ms"].as<unsigned long>());
  }

  if (!ui["sound_volume"].isNull()) {
    config::setSoundVolume(ui["sound_volume"].as<unsigned int>());
  }
  if (!ui["button_press_beep_enabled"].isNull()) {
    config::setButtonPressBeepEnabled(ui["button_press_beep_enabled"].as<bool>());
  }

  if (!config::saveConfig()) {
    sendJsonMessage(500, false, "Failed to save config.json.");
    return;
  }
  if (device_changed && !config::saveDevice()) {
    sendJsonMessage(500, false, "Failed to save device.json.");
    return;
  }

  const bool thresholds_applied = applyTemperatureSafetyConfig();
  bool profile_reloaded = false;
  if (profile_changed && shouldReloadProfileNow()) {
    profile_reloaded = config::reloadIgnitionProfile();
  }

  DynamicJsonDocument response(256);
  response["ok"] = true;
  response["message"] = "Settings saved.";
  response["temperature_safety_applied"] = thresholds_applied;
  response["profile_reloaded"] = profile_reloaded;
  response["device_changed"] = device_changed;
  sendJsonDocument(200, response);
}

void handleGetProfiles() {
  config::IgnitionProfileFileInfo profiles[config::kMaxIgnitionProfiles] = {};
  const size_t count = config::listIgnitionProfiles(profiles, config::kMaxIgnitionProfiles);

  DynamicJsonDocument doc(4096);
  doc["active_profile_filename"] = config::getActiveProfileFilename();
  doc["loaded_profile_filename"] = config::getLoadedIgnitionProfileFilename();

  JsonArray list = doc.createNestedArray("profiles");
  for (size_t i = 0; i < count; ++i) {
    JsonObject entry = list.createNestedObject();
    entry["filename"] = profiles[i].filename;
    entry["exported_at"] = profiles[i].exported_at;
    entry["gp_segment_count"] = profiles[i].gp_segment_count;
    entry["fan_segment_count"] = profiles[i].fan_segment_count;
    entry["total_duration_ms"] = profiles[i].total_duration_ms;
    entry["is_active"] = profiles[i].is_active;
    entry["is_default"] = profiles[i].is_default;
  }

  sendJsonDocument(200, doc);
}

void handleSetActiveProfile() {
  DynamicJsonDocument doc(512);
  String error_message;
  if (!parseJsonBody(doc, error_message)) {
    sendJsonMessage(400, false, error_message);
    return;
  }

  const String filename = doc["filename"] | "";
  if (!config::profileFileExists(filename)) {
    sendJsonMessage(400, false, "Selected profile file was not found.");
    return;
  }

  config::setActiveProfileFilename(filename);
  if (!config::saveConfig()) {
    sendJsonMessage(500, false, "Failed to save active profile selection.");
    return;
  }

  const bool profile_reloaded = shouldReloadProfileNow() ? config::reloadIgnitionProfile() : false;

  DynamicJsonDocument response(256);
  response["ok"] = true;
  response["message"] = "Active profile updated.";
  response["profile_reloaded"] = profile_reloaded;
  sendJsonDocument(200, response);
}

void handleDeleteProfileByName(const String &filename) {
  String error_message;
  if (!config::deleteIgnitionProfileFile(filename, error_message)) {
    sendJsonMessage(400, false, error_message);
    return;
  }

  sendJsonMessage(200, true, "Profile deleted.");
}

void handleProfileUploadFinished() {
  if (g_profile_upload.success) {
    sendJsonMessage(200, true, g_profile_upload.success_message);
    return;
  }

  String message = g_profile_upload.error_message;
  if (message.length() == 0) {
    message = "Profile upload failed.";
  }
  sendJsonMessage(500, false, message);
}

void handleProfileUploadData() {
  HTTPUpload &upload = g_ota_server.upload();

  switch (upload.status) {
    case UPLOAD_FILE_START:
      resetUploadStatus(g_profile_upload);
      g_profile_upload_filename = extractUploadFilename(upload.filename);
      g_profile_upload_contents = "";
      break;

    case UPLOAD_FILE_WRITE:
      if (!g_profile_upload.error) {
        if (g_profile_upload_contents.length() + upload.currentSize > kMaxProfileUploadBytes) {
          g_profile_upload.error = true;
          g_profile_upload.error_message = "Profile file is too large.";
        } else {
          g_profile_upload_contents.concat(
              reinterpret_cast<const char *>(upload.buf), upload.currentSize);
        }
      }
      break;

    case UPLOAD_FILE_END:
      if (!g_profile_upload.error) {
        String error_message;
        String stored_filename;
        if (config::saveIgnitionProfileFile(
                g_profile_upload_filename, g_profile_upload_contents, error_message, &stored_filename)) {
          g_profile_upload.success_message = stored_filename.length() == 0
                                                 ? "Profile uploaded successfully."
                                                 : "Profile uploaded as " + stored_filename + ".";
          if (stored_filename == config::getActiveProfileFilename() &&
              shouldReloadProfileNow()) {
            config::reloadIgnitionProfile();
          }
          g_profile_upload.success = true;
        } else {
          g_profile_upload.error = true;
          g_profile_upload.error_message = error_message;
        }
      }
      break;

    case UPLOAD_FILE_ABORTED:
      g_profile_upload.error = true;
      g_profile_upload.error_message = "Profile upload aborted.";
      break;

    default:
      break;
  }
}

void handleNotFound() {
  if (g_ota_server.method() == HTTP_DELETE) {
    const String uri = g_ota_server.uri();
    const String prefix = "/api/profiles/";
    if (uri.startsWith(prefix)) {
      handleDeleteProfileByName(uri.substring(prefix.length()));
      return;
    }
  }

  if (g_ota_server.uri() == "/favicon.ico") {
    g_ota_server.send(204, "text/plain", "");
    return;
  }

  if (g_ota_server.method() == HTTP_GET && tryServeStaticFile(g_ota_server.uri())) {
    return;
  }

  sendJsonMessage(404, false, "Not found.");
}

void configureRoutesIfNeeded() {
  if (g_routes_registered) {
    return;
  }

  g_ota_server.on("/", HTTP_GET, sendDashboardPage);
  g_ota_server.on(kOtaPortalPath, HTTP_GET, sendDashboardPage);
  g_ota_server.on(kOtaPortalPath, HTTP_POST, handleFirmwareUpdateFinished, handleFirmwareUpdateUpload);
  g_ota_server.on(kOtaFilesystemPath,
                  HTTP_POST,
                  handleFilesystemUpdateFinished,
                  handleFilesystemUpdateUpload);

  g_ota_server.on("/api/status", HTTP_GET, sendStatusJson);
  g_ota_server.on("/api/control/start-cycle", HTTP_POST, handleControlStartCycle);
  g_ota_server.on("/api/control/start-blower", HTTP_POST, handleControlStartBlower);
  g_ota_server.on("/api/control/abort-cycle", HTTP_POST, handleControlAbortCycle);
  g_ota_server.on("/api/control/stop-blower", HTTP_POST, handleControlStopBlower);
  g_ota_server.on("/api/control/start-manual", HTTP_POST, handleControlStartManual);
  g_ota_server.on("/api/control/manual-output", HTTP_POST, handleControlManualOutput);
  g_ota_server.on("/api/control/stop-manual", HTTP_POST, handleControlStopManual);

  g_ota_server.on("/api/config", HTTP_GET, handleGetConfig);
  g_ota_server.on("/api/config", HTTP_POST, handlePostConfig);

  g_ota_server.on("/api/profiles", HTTP_GET, handleGetProfiles);
  g_ota_server.on("/api/profiles/upload",
                  HTTP_POST,
                  handleProfileUploadFinished,
                  handleProfileUploadData);
  g_ota_server.on("/api/profiles/active", HTTP_POST, handleSetActiveProfile);
  g_ota_server.onNotFound(handleNotFound);

  g_routes_registered = true;
}

void startOtaAccessPoint() {
  if (g_ota_ap_active) {
    return;
  }

  configureRoutesIfNeeded();
  WiFi.mode(WIFI_AP);
  g_ota_ap_ssid = buildOtaApSsid();
  WiFi.softAP(g_ota_ap_ssid.c_str());
  g_ota_ap_ip = WiFi.softAPIP();

  g_ota_server.begin();
  g_ota_ap_active = true;
  resetUploadStatus(g_firmware_upload);
  resetUploadStatus(g_filesystem_upload);
  resetUploadStatus(g_profile_upload);
  g_profile_upload_filename = "";
  g_profile_upload_contents = "";
  g_ota_restart_pending = false;
  g_ota_restart_deadline_ms = 0;
  g_last_portal_activity_ms = 0;

  DebugSerial.println();
  DebugSerial.println("OTA AP active");
  DebugSerial.print("SSID: ");
  DebugSerial.println(g_ota_ap_ssid);
  DebugSerial.print("Update URL: http://");
  DebugSerial.print(g_ota_ap_ip);
  DebugSerial.println(kOtaPortalPath);
}

void stopOtaAccessPoint() {
  if (!g_ota_ap_active) {
    return;
  }

  g_ota_server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  g_ota_ap_active = false;
  g_ota_ap_ssid = "";
  g_ota_ap_ip = IPAddress();
  resetUploadStatus(g_firmware_upload);
  resetUploadStatus(g_filesystem_upload);
  resetUploadStatus(g_profile_upload);
  g_profile_upload_filename = "";
  g_profile_upload_contents = "";
  g_ota_restart_pending = false;
  g_ota_restart_deadline_ms = 0;
  g_last_portal_activity_ms = 0;
}

void exitSettingsMenu() {
  stopOtaAccessPoint();
  g_state = MenuState::INACTIVE;
  g_setting_index = 0;
  g_menu_intro_active = false;
  g_menu_intro_deadline_ms = 0;
  clearStateGestureTracking();
  stopBeep();
}

void beginMenuEntry() {
  g_state = MenuState::SETTINGS_MENU;
  g_setting_index = 0;
  g_menu_intro_active = true;
  g_menu_intro_deadline_ms = millis() + kMenuEntryDurationMs;
  clearStateGestureTracking();
  g_last_interaction_ms = millis();
  startCountBeepSequence(2, 4, kEntryBeepDurationMs, kEntryBeepGapMs);
}

void enterWebPortalActive() {
  startOtaAccessPoint();
  g_state = MenuState::WEB_PORTAL_ACTIVE;
  clearStateGestureTracking();
  g_last_interaction_ms = millis();
  startCountBeepSequence(3, 4, kEntryBeepDurationMs, kEntryBeepGapMs);
}

void enterSettingsEdit() {
  if (g_menu_single_click_pending && !g_menu_pending_exit) {
    g_setting_index = g_menu_pending_origin_setting;
  }

  clearPendingMenuSingleClick();
  g_state = MenuState::SETTINGS_EDIT;
  g_edit_pending_count = 0;
  g_edit_entered_ms = millis();
  g_edit_last_click_ms = 0;
  g_last_interaction_ms = millis();
  resetEditBlink();
}

const RgbColor &currentSettingColor() {
  return kSettingColors[g_setting_index];
}

void renderMenuLed() {
  const unsigned long now = millis();

  if (g_menu_intro_active) {
    const bool led_on = ((now / kMenuEntryFlashIntervalMs) % 2UL) == 0;
    applyColor(led_on ? kMenuEntryColor : RgbColor{0, 0, 0});
    return;
  }

  if (g_state == MenuState::SETTINGS_MENU) {
    applyColor(currentSettingColor());
    return;
  }

  if (g_state == MenuState::SETTINGS_EDIT) {
    applyColor(g_edit_blink_on ? currentSettingColor() : RgbColor{0, 0, 0});
  }
}

unsigned int clampSettingCount(int setting_index, unsigned int count) {
  switch (setting_index) {
    case kIdleSleepSetting:
      return count > 8 ? 8 : max(1U, count);
    case kAbortBlowerSetting:
      return count > 8 ? 8 : max(1U, count);
    case kSoundSetting:
      return count > 5 ? 5 : max(1U, count);
    default:
      return 1;
  }
}

unsigned int getStoredCountForSetting(int setting_index) {
  const config::Settings &settings = config::getSettings();

  switch (setting_index) {
    case kIdleSleepSetting:
      return clampSettingCount(
          setting_index,
          static_cast<unsigned int>((settings.idle_sleep_timeout_ms + 15000UL) / 30000UL));
    case kAbortBlowerSetting:
      return clampSettingCount(
          setting_index,
          static_cast<unsigned int>((settings.abort_blower_duration_ms + 5000UL) / 10000UL));
    case kSoundSetting:
      switch (clampVolumeLevel(settings.sound_volume)) {
        case 4:
          return 1;
        case 3:
          return 2;
        case 2:
          return 3;
        case 1:
          return 4;
        case 0:
        default:
          return 5;
      }
    case kPortalSetting:
    default:
      return 1;
  }
}

unsigned int getReadbackVolumeLevel(int setting_index) {
  if (setting_index == kSoundSetting) {
    const unsigned int count = getStoredCountForSetting(setting_index);
    if (count >= 5) {
      return 0;
    }
    return 5 - count;
  }

  return clampVolumeLevel(config::getSettings().sound_volume);
}

void playSettingReadback(int setting_index) {
  if (setting_index == kPortalSetting) {
    startCountBeepSequence(1, 4, kReadbackBeepDurationMs, kReadbackBeepGapMs);
    return;
  }

  startCountBeepSequence(getStoredCountForSetting(setting_index),
                         getReadbackVolumeLevel(setting_index),
                         kReadbackBeepDurationMs,
                         kReadbackBeepGapMs);
}

void commitSettingValue() {
  const unsigned int clamped_count = clampSettingCount(g_setting_index, g_edit_pending_count);

  switch (g_setting_index) {
    case kIdleSleepSetting:
      config::setIdleSleepTimeoutMs(clamped_count * 30000UL);
      break;
    case kAbortBlowerSetting:
      config::setAbortBlowerDurationMs(clamped_count * 10000UL);
      break;
    case kSoundSetting:
      config::setSoundVolume(clamped_count >= 5 ? 0 : 5 - clamped_count);
      break;
    default:
      break;
  }

  config::saveConfig();
  g_state = MenuState::SETTINGS_MENU;
  g_edit_pending_count = 0;
  g_edit_entered_ms = 0;
  g_edit_last_click_ms = 0;
  g_last_interaction_ms = millis();
}

void cancelEditMode() {
  g_state = MenuState::SETTINGS_MENU;
  g_edit_pending_count = 0;
  g_edit_entered_ms = 0;
  g_edit_last_click_ms = 0;
  g_last_interaction_ms = millis();
}

void handleMenuShortClick(unsigned long now_ms) {
  g_last_interaction_ms = now_ms;

  if (!g_menu_single_click_pending || now_ms - g_menu_last_click_ms > kMenuDoubleClickWindowMs) {
    g_menu_single_click_pending = true;
    g_menu_last_click_ms = now_ms;
    g_menu_pending_origin_setting = g_setting_index;
    g_menu_pending_exit = (g_setting_index + 1 >= kSettingCount);

    if (!g_menu_pending_exit) {
      ++g_setting_index;
    }
    return;
  }

  int readback_setting_index = g_menu_pending_origin_setting;
  if (!g_menu_pending_exit) {
    g_setting_index = g_menu_pending_origin_setting;
  }

  clearPendingMenuSingleClick();
  playSettingReadback(readback_setting_index);
}

void processPendingMenuSingleClick() {
  if (!g_menu_single_click_pending) {
    return;
  }

  if (g_menu_pending_exit) {
    exitSettingsMenu();
    return;
  }

  clearPendingMenuSingleClick();
}

}  // namespace

void setupSettingsMenu() {
  ledcSetup(kBuzzerChannel, ui_config::kBuzzerFrequencyHz, kBuzzerResolutionBits);
  configureRoutesIfNeeded();
}

void enterSettingsMenu() {
  beginMenuEntry();
}

bool settingsMenuIsActive() {
  return g_state != MenuState::INACTIVE;
}

bool settingsMenuBlocksCycleManager() {
  return g_state != MenuState::INACTIVE && g_state != MenuState::WEB_PORTAL_ACTIVE;
}

bool settingsMenuOwnsUi() {
  return g_state != MenuState::INACTIVE && g_state != MenuState::WEB_PORTAL_ACTIVE;
}

bool settingsMenuAllowsSleep() {
  return g_state == MenuState::INACTIVE && !settingsMenuPreventsSleep();
}

bool settingsMenuPreventsSleep() {
  if (!g_ota_ap_active) {
    return false;
  }

  if (WiFi.softAPgetStationNum() == 0) {
    return false;
  }

  const unsigned long now = millis();
  return g_last_portal_activity_ms != 0 &&
         (now - g_last_portal_activity_ms) <= kPortalActivityKeepAwakeMs;
}

void notifySettingsMenuButtonPressed(unsigned long press_started_ms) {
  if (g_state == MenuState::INACTIVE) {
    return;
  }

  g_button_down = true;
  g_button_press_start_ms = press_started_ms;
  g_hold_handled = false;
}

void notifySettingsMenuButtonReleased(unsigned long release_ms) {
  if (g_state == MenuState::INACTIVE) {
    return;
  }

  g_button_down = false;

  if (g_state == MenuState::SETTINGS_MENU) {
    if (!g_hold_handled &&
        !g_menu_intro_active &&
        release_ms - g_button_press_start_ms < kMenuHoldMs) {
      handleMenuShortClick(release_ms);
    }
    return;
  }

  if (g_state == MenuState::WEB_PORTAL_ACTIVE) {
    return;
  }

  if (!g_hold_handled && g_state == MenuState::SETTINGS_EDIT) {
    ++g_edit_pending_count;
    g_edit_last_click_ms = release_ms;
    g_last_interaction_ms = release_ms;
  }
}

void updateSettingsMenu() {
  const unsigned long now = millis();

  if (!g_ota_ap_active) {
    if (cycleIsReadyIdle()) {
      if (g_ready_idle_portal_armed_ms == 0) {
        g_ready_idle_portal_armed_ms = now;
      } else if (now - g_ready_idle_portal_armed_ms >= kAutoPortalStartDelayMs) {
        startOtaAccessPoint();
      }
    } else {
      g_ready_idle_portal_armed_ms = 0;
    }
  } else {
    g_ready_idle_portal_armed_ms = 0;
  }

  if (g_ota_ap_active) {
    g_ota_server.handleClient();
    if (g_ota_restart_pending && now >= g_ota_restart_deadline_ms) {
      ESP.restart();
    }
  }

  if (g_state == MenuState::INACTIVE) {
    return;
  }

  if (g_button_down &&
      !g_hold_handled &&
      !g_menu_intro_active &&
      (g_state == MenuState::SETTINGS_MENU || g_state == MenuState::WEB_PORTAL_ACTIVE) &&
      now - g_button_press_start_ms >= kMenuHoldMs) {
    g_hold_handled = true;

    if (g_state == MenuState::WEB_PORTAL_ACTIVE) {
      stopOtaAccessPoint();
      g_state = MenuState::SETTINGS_MENU;
      g_setting_index = kPortalSetting;
      clearStateGestureTracking();
      g_last_interaction_ms = now;
    } else if (g_setting_index == kPortalSetting) {
      enterWebPortalActive();
    } else {
      enterSettingsEdit();
    }
  }

  updateBeepSequence();
  updateEditBlink();

  if (g_menu_intro_active && now >= g_menu_intro_deadline_ms) {
    g_menu_intro_active = false;
    g_last_interaction_ms = now;
  }

  if (g_state == MenuState::SETTINGS_MENU) {
    if (g_menu_single_click_pending &&
        now - g_menu_last_click_ms >= kMenuDoubleClickWindowMs) {
      processPendingMenuSingleClick();
      g_last_interaction_ms = now;
    }

    if (now - g_last_interaction_ms >= kMenuInactivityTimeoutMs) {
      exitSettingsMenu();
      return;
    }
  }

  if (g_state == MenuState::SETTINGS_EDIT) {
    if (g_edit_pending_count > 0 && now - g_edit_last_click_ms >= kEditCommitDelayMs) {
      commitSettingValue();
    } else if (g_edit_pending_count == 0 &&
               now - g_edit_entered_ms >= kEditCancelTimeoutMs) {
      cancelEditMode();
    }
  }

  if (settingsMenuOwnsUi()) {
    renderMenuLed();
  }
}
