#include "update_manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <time.h>
#include <vector>

#include "cJSON.h"
#include "dev_config.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unit_identity.h"

namespace update_manager {
namespace {

constexpr char kTag[] = "update_manager";
constexpr int kHttpTimeoutMs = 10000;
constexpr uint32_t kWorkerStackBytes = 12288;

enum class AsyncOperation {
  NONE,
  CHECK,
  APPLY,
};

struct Semver {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::vector<std::string> prerelease;
};

UpdateStatus g_status = {};
UpdateTarget g_target = {};
bool g_initialized = false;
bool g_pending_confirmation = false;
uint32_t g_ready_idle_since_ms = 0;
TaskHandle_t g_worker_task = nullptr;
AsyncOperation g_async_operation = AsyncOperation::NONE;
AsyncState g_async_state = AsyncState::IDLE;
CheckDecision g_async_check_decision = CheckDecision::NONE;
bool g_async_apply_success = false;

bool is_numeric(const std::string &value) {
  if (value.empty()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string trim_copy(const std::string &value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return value.substr(begin, end - begin);
}

bool parse_uint(const std::string &text, int &out_value) {
  if (text.empty() || !is_numeric(text)) {
    return false;
  }
  out_value = std::stoi(text);
  return true;
}

bool parse_semver(const std::string &raw, Semver &out_version) {
  const std::string value = trim_copy(raw);
  if (value.empty()) {
    return false;
  }

  const std::size_t plus_pos = value.find('+');
  const std::string without_build = value.substr(0, plus_pos);
  const std::size_t dash_pos = without_build.find('-');
  const std::string core = without_build.substr(0, dash_pos);
  const std::string prerelease = dash_pos == std::string::npos ? "" : without_build.substr(dash_pos + 1);

  std::array<std::string, 3> parts = {};
  std::size_t start = 0;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    const std::size_t dot_pos = core.find('.', start);
    if (dot_pos == std::string::npos) {
      parts[index] = core.substr(start);
      start = core.size();
      if (index != parts.size() - 1) {
        return false;
      }
      break;
    }

    parts[index] = core.substr(start, dot_pos - start);
    start = dot_pos + 1;
  }

  if (start < core.size()) {
    return false;
  }

  if (!parse_uint(parts[0], out_version.major) ||
      !parse_uint(parts[1], out_version.minor) ||
      !parse_uint(parts[2], out_version.patch)) {
    return false;
  }

  out_version.prerelease.clear();
  if (!prerelease.empty()) {
    std::size_t token_start = 0;
    while (token_start <= prerelease.size()) {
      const std::size_t dot_pos = prerelease.find('.', token_start);
      std::string token = dot_pos == std::string::npos
                              ? prerelease.substr(token_start)
                              : prerelease.substr(token_start, dot_pos - token_start);
      if (token.empty()) {
        return false;
      }
      out_version.prerelease.push_back(token);
      if (dot_pos == std::string::npos) {
        break;
      }
      token_start = dot_pos + 1;
    }
  }

  return true;
}

int compare_prerelease_identifiers(const std::string &lhs, const std::string &rhs) {
  const bool lhs_numeric = is_numeric(lhs);
  const bool rhs_numeric = is_numeric(rhs);
  if (lhs_numeric && rhs_numeric) {
    const int lhs_value = std::stoi(lhs);
    const int rhs_value = std::stoi(rhs);
    if (lhs_value < rhs_value) {
      return -1;
    }
    if (lhs_value > rhs_value) {
      return 1;
    }
    return 0;
  }

  if (lhs_numeric != rhs_numeric) {
    return lhs_numeric ? -1 : 1;
  }

  if (lhs < rhs) {
    return -1;
  }
  if (lhs > rhs) {
    return 1;
  }
  return 0;
}

int compare_semver(const std::string &lhs_raw, const std::string &rhs_raw, bool &ok) {
  Semver lhs = {};
  Semver rhs = {};
  if (!parse_semver(lhs_raw, lhs) || !parse_semver(rhs_raw, rhs)) {
    ok = false;
    return 0;
  }

  ok = true;
  if (lhs.major != rhs.major) {
    return lhs.major < rhs.major ? -1 : 1;
  }
  if (lhs.minor != rhs.minor) {
    return lhs.minor < rhs.minor ? -1 : 1;
  }
  if (lhs.patch != rhs.patch) {
    return lhs.patch < rhs.patch ? -1 : 1;
  }

  if (lhs.prerelease.empty() && rhs.prerelease.empty()) {
    return 0;
  }
  if (lhs.prerelease.empty()) {
    return 1;
  }
  if (rhs.prerelease.empty()) {
    return -1;
  }

  const std::size_t count = std::min(lhs.prerelease.size(), rhs.prerelease.size());
  for (std::size_t index = 0; index < count; ++index) {
    const int compare = compare_prerelease_identifiers(lhs.prerelease[index], rhs.prerelease[index]);
    if (compare != 0) {
      return compare;
    }
  }

  if (lhs.prerelease.size() < rhs.prerelease.size()) {
    return -1;
  }
  if (lhs.prerelease.size() > rhs.prerelease.size()) {
    return 1;
  }
  return 0;
}

std::string current_version() {
  const auto *description = esp_app_get_description();
  return description != nullptr ? std::string(description->version) : std::string("unknown");
}

void set_result(const char *result, const std::string &message) {
  g_status.last_result = result;
  g_status.last_message = message;
}

esp_err_t http_event_handler(esp_http_client_event_t *event) {
  if (event == nullptr) {
    return ESP_FAIL;
  }

  if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data != nullptr && event->data != nullptr &&
      event->data_len > 0) {
    auto *body = static_cast<std::string *>(event->user_data);
    body->append(static_cast<const char *>(event->data), static_cast<std::size_t>(event->data_len));
  }

  return ESP_OK;
}

bool fetch_url_to_string(const std::string &url, std::string &body, std::string &message) {
  body.clear();
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = kHttpTimeoutMs;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.user_agent = "EZQ-Controller-Updater/1";
  config.event_handler = http_event_handler;
  config.user_data = &body;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    message = "http_client_init_failed";
    return false;
  }

  const esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    message = std::string("http_perform_failed:") + esp_err_to_name(err);
    esp_http_client_cleanup(client);
    return false;
  }

  const int status_code = esp_http_client_get_status_code(client);
  if (status_code < 200 || status_code >= 300) {
    char buffer[64] = {};
    snprintf(buffer, sizeof(buffer), "http_status_%d", status_code);
    message = buffer;
    esp_http_client_cleanup(client);
    return false;
  }
  esp_http_client_cleanup(client);
  return true;
}

bool parse_target_object(cJSON *object, const char *selector, UpdateTarget &target, std::string &message) {
  if (object == nullptr || !cJSON_IsObject(object)) {
    return false;
  }

  cJSON *version = cJSON_GetObjectItemCaseSensitive(object, "version");
  cJSON *firmware_url = cJSON_GetObjectItemCaseSensitive(object, "firmware_url");
  cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(object, "sha256");
  cJSON *size = cJSON_GetObjectItemCaseSensitive(object, "size");
  if (!cJSON_IsString(version) || !cJSON_IsString(firmware_url) || !cJSON_IsString(sha256) ||
      !cJSON_IsNumber(size)) {
    message = std::string("invalid_target:") + selector;
    return false;
  }

  target.valid = true;
  target.selector = selector;
  target.version = version->valuestring;
  target.firmware_url = firmware_url->valuestring;
  target.sha256 = sha256->valuestring;
  target.size = size->valueint;
  return true;
}

bool resolve_target_from_manifest(cJSON *root, UpdateTarget &target, std::string &message) {
  cJSON *channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
  if (!cJSON_IsString(channel) || std::string(channel->valuestring) != "stable") {
    message = "unsupported_channel";
    return false;
  }

  cJSON *schema_version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
  if (!cJSON_IsNumber(schema_version) || schema_version->valueint != 1) {
    message = "unsupported_schema";
    return false;
  }

  if (unit_identity::has_unit_id()) {
    cJSON *overrides = cJSON_GetObjectItemCaseSensitive(root, "unit_overrides");
    if (cJSON_IsObject(overrides)) {
      cJSON *override = cJSON_GetObjectItemCaseSensitive(overrides, unit_identity::unit_id().c_str());
      if (override != nullptr) {
        return parse_target_object(override, unit_identity::unit_id().c_str(), target, message);
      }
    }
  }

  cJSON *boards = cJSON_GetObjectItemCaseSensitive(root, "boards");
  if (!cJSON_IsObject(boards)) {
    message = "boards_missing";
    return false;
  }

  cJSON *board = cJSON_GetObjectItemCaseSensitive(boards, unit_identity::kBoardId);
  if (board == nullptr) {
    message = "no_board_target";
    return false;
  }

  return parse_target_object(board, unit_identity::kBoardId, target, message);
}

std::string to_hex_string(const uint8_t *bytes, std::size_t length) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.resize(length * 2);
  for (std::size_t index = 0; index < length; ++index) {
    output[index * 2] = kHex[(bytes[index] >> 4) & 0x0F];
    output[index * 2 + 1] = kHex[bytes[index] & 0x0F];
  }
  return output;
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool system_time_is_reasonable() {
  time_t now = 0;
  time(&now);
  struct tm time_info = {};
#if defined(_WIN32)
  gmtime_s(&time_info, &now);
#else
  gmtime_r(&now, &time_info);
#endif
  const int year = time_info.tm_year + 1900;
  return year >= 2024;
}

bool ensure_time_synced(std::string &message) {
  if (system_time_is_reasonable()) {
    return true;
  }

  ESP_LOGI(kTag, "System time is not valid yet. Syncing SNTP before HTTPS update check.");
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
  const esp_err_t init_err = esp_netif_sntp_init(&config);
  if (init_err != ESP_OK) {
    message = std::string("sntp_init_failed:") + esp_err_to_name(init_err);
    return false;
  }

  const esp_err_t wait_err =
      esp_netif_sntp_sync_wait(pdMS_TO_TICKS(dev_config::kUpdateTimeSyncWaitMs));
  esp_netif_sntp_deinit();
  if (wait_err != ESP_OK) {
    message = std::string("sntp_sync_failed:") + esp_err_to_name(wait_err);
    return false;
  }

  if (!system_time_is_reasonable()) {
    message = "sntp_sync_invalid_time";
    return false;
  }

  time_t now = 0;
  time(&now);
  struct tm time_info = {};
#if defined(_WIN32)
  gmtime_s(&time_info, &now);
#else
  gmtime_r(&now, &time_info);
#endif
  char buffer[64] = {};
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &time_info);
  ESP_LOGI(kTag, "SNTP time sync complete: %s", buffer);
  return true;
}

bool verify_partition_sha256(const esp_partition_t *partition,
                             const std::string &expected_sha256,
                             std::string &message) {
  if (partition == nullptr) {
    message = "missing_update_partition";
    return false;
  }

  std::array<uint8_t, 32> digest = {};
  const esp_err_t err = esp_partition_get_sha256(partition, digest.data());
  if (err != ESP_OK) {
    message = std::string("partition_sha256_failed:") + esp_err_to_name(err);
    return false;
  }

  const std::string actual = to_hex_string(digest.data(), digest.size());
  if (lower_copy(actual) != lower_copy(trim_copy(expected_sha256))) {
    message = "sha256_mismatch";
    ESP_LOGE(kTag, "OTA image SHA mismatch: expected=%s actual=%s",
             expected_sha256.c_str(), actual.c_str());
    return false;
  }

  return true;
}

void refresh_confirmation_state() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
  if (running != nullptr &&
      esp_ota_get_state_partition(running, &image_state) == ESP_OK &&
      image_state == ESP_OTA_IMG_PENDING_VERIFY) {
    g_pending_confirmation = true;
    g_status.pending_confirmation = true;
    g_status.rollback_armed = true;
    if (g_status.last_result.empty()) {
      g_status.last_result = "pending_confirmation";
      g_status.last_message = "Current image is pending rollback confirmation.";
    }
  } else {
    g_pending_confirmation = false;
    g_status.pending_confirmation = false;
    g_status.rollback_armed = false;
  }
  g_ready_idle_since_ms = 0;
}

void worker_task(void *) {
  if (g_async_operation == AsyncOperation::CHECK) {
    g_async_check_decision = check_for_update();
  } else if (g_async_operation == AsyncOperation::APPLY) {
    g_async_apply_success = perform_pending_update();
  }

  g_async_state = AsyncState::COMPLETE;
  g_worker_task = nullptr;
  vTaskDelete(nullptr);
}

bool start_worker(AsyncOperation operation) {
  if (g_worker_task != nullptr || g_async_state == AsyncState::RUNNING) {
    return false;
  }

  g_async_operation = operation;
  g_async_state = AsyncState::RUNNING;
  g_async_check_decision = CheckDecision::NONE;
  g_async_apply_success = false;

  const BaseType_t created = xTaskCreate(
      worker_task, "update_worker", kWorkerStackBytes, nullptr, tskIDLE_PRIORITY + 1, &g_worker_task);
  if (created != pdPASS) {
    g_worker_task = nullptr;
    g_async_operation = AsyncOperation::NONE;
    g_async_state = AsyncState::IDLE;
    set_result("worker_start_failed", "Failed to create update worker task.");
    return false;
  }

  return true;
}

}  // namespace

void init() {
  g_status = {};
  g_status.manifest_url = dev_config::kUpdateManifestUrl;
  g_status.current_version = current_version();
  g_target = {};
  g_initialized = true;
  refresh_confirmation_state();
}

UpdateStatus get_status() {
  if (!g_initialized) {
    init();
  }
  g_status.current_version = current_version();
  g_status.pending_confirmation = g_pending_confirmation;
  g_status.rollback_armed = g_pending_confirmation;
  return g_status;
}

CheckDecision check_for_update() {
  if (!g_initialized) {
    init();
  }

  g_status.current_version = current_version();
  g_status.available_version.clear();
  g_status.target_selector.clear();
  g_status.update_available = false;
  g_target = {};

  bool version_ok = false;
  Semver current = {};
  if (!parse_semver(g_status.current_version, current)) {
    set_result("invalid_current_version", "Current firmware version is not valid semver.");
    return CheckDecision::ERROR;
  }

  std::string manifest_body;
  std::string message;
  if (!ensure_time_synced(message)) {
    set_result("time_sync_failed", message);
    return CheckDecision::ERROR;
  }

  if (!fetch_url_to_string(g_status.manifest_url, manifest_body, message)) {
    set_result("manifest_fetch_failed", message);
    return CheckDecision::ERROR;
  }

  cJSON *root = cJSON_Parse(manifest_body.c_str());
  if (root == nullptr) {
    set_result("manifest_parse_failed", "Manifest JSON could not be parsed.");
    return CheckDecision::ERROR;
  }

  UpdateTarget target = {};
  const bool resolved = resolve_target_from_manifest(root, target, message);
  cJSON_Delete(root);

  if (!resolved) {
    if (message == "no_board_target") {
      set_result("no_target", "No update target matched this board.");
      return CheckDecision::NO_UPDATE;
    }
    set_result("manifest_invalid", message);
    return CheckDecision::ERROR;
  }

  g_status.available_version = target.version;
  g_status.target_selector = target.selector;
  const int compare = compare_semver(g_status.current_version, target.version, version_ok);
  if (!version_ok) {
    set_result("manifest_invalid_version", "Manifest target version is not valid semver.");
    return CheckDecision::ERROR;
  }

  if (compare >= 0) {
    set_result("up_to_date", "Current firmware is up to date.");
    return CheckDecision::NO_UPDATE;
  }

  g_target = target;
  g_status.update_available = true;
  set_result("update_available", "A newer firmware version is available.");
  return CheckDecision::UPDATE_AVAILABLE;
}

bool perform_pending_update() {
  if (!g_initialized) {
    init();
  }

  if (!g_target.valid) {
    set_result("ota_skipped", "No pending update target is available.");
    return false;
  }

  const esp_partition_t *target_partition = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t *running_partition = esp_ota_get_running_partition();

  esp_http_client_config_t http_config = {};
  http_config.url = g_target.firmware_url.c_str();
  http_config.timeout_ms = kHttpTimeoutMs;
  http_config.crt_bundle_attach = esp_crt_bundle_attach;
  http_config.user_agent = "EZQ-Controller-Updater/1";

  esp_https_ota_config_t ota_config = {};
  ota_config.http_config = &http_config;

  esp_https_ota_handle_t handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
  if (err != ESP_OK) {
    set_result("ota_begin_failed", esp_err_to_name(err));
    return false;
  }

  g_status.ota_in_progress = true;
  set_result("ota_in_progress", "Downloading firmware update.");

  while (true) {
    err = esp_https_ota_perform(handle);
    if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      continue;
    }
    break;
  }

  if (err != ESP_OK) {
    g_status.ota_in_progress = false;
    esp_https_ota_abort(handle);
    set_result("ota_download_failed", esp_err_to_name(err));
    return false;
  }

  if (!esp_https_ota_is_complete_data_received(handle)) {
    g_status.ota_in_progress = false;
    esp_https_ota_abort(handle);
    set_result("ota_incomplete", "Firmware image download was incomplete.");
    return false;
  }

  err = esp_https_ota_finish(handle);
  g_status.ota_in_progress = false;
  if (err != ESP_OK) {
    set_result("ota_finish_failed", esp_err_to_name(err));
    return false;
  }

  std::string verify_message;
  if (!verify_partition_sha256(target_partition, g_target.sha256, verify_message)) {
    if (running_partition != nullptr) {
      esp_ota_set_boot_partition(running_partition);
    }
    set_result("ota_verify_failed", verify_message);
    return false;
  }

  g_status.available_version = g_target.version;
  set_result("ota_applied", "Firmware downloaded successfully. Rebooting into new image.");
  return true;
}

bool pending_update_available() {
  return g_target.valid;
}

bool start_check_async() {
  if (!g_initialized) {
    init();
  }
  return start_worker(AsyncOperation::CHECK);
}

bool start_apply_async() {
  if (!g_initialized) {
    init();
  }
  return start_worker(AsyncOperation::APPLY);
}

AsyncState async_state() {
  return g_async_state;
}

CheckDecision async_check_decision() {
  return g_async_check_decision;
}

bool async_apply_success() {
  return g_async_apply_success;
}

void clear_async_state() {
  if (g_async_state == AsyncState::RUNNING) {
    return;
  }
  g_async_operation = AsyncOperation::NONE;
  g_async_state = AsyncState::IDLE;
  g_async_check_decision = CheckDecision::NONE;
  g_async_apply_success = false;
}

void tick_post_boot(ControlStateId state, ControlFaultKind fault_kind, uint32_t now_ms) {
  if (!g_initialized) {
    init();
  }

  if (!g_pending_confirmation) {
    return;
  }

  if (fault_kind != ControlFaultKind::NONE || state == ControlStateId::FAULT) {
    set_result("rollback_triggered", "Fault before confirmation; rolling back.");
    ESP_LOGE(kTag, "Current OTA image faulted before confirmation; rolling back.");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return;
  }

  if (state != ControlStateId::READY_IDLE) {
    g_ready_idle_since_ms = 0;
    return;
  }

  if (g_ready_idle_since_ms == 0) {
    g_ready_idle_since_ms = now_ms;
    return;
  }

  if ((now_ms - g_ready_idle_since_ms) < static_cast<uint32_t>(dev_config::kUpdateConfirmStableMs)) {
    return;
  }

  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    set_result("confirm_failed", esp_err_to_name(err));
    ESP_LOGE(kTag, "Failed confirming OTA image: %s", esp_err_to_name(err));
    return;
  }

  g_pending_confirmation = false;
  g_status.pending_confirmation = false;
  g_status.rollback_armed = false;
  set_result("confirmed", "Current OTA image confirmed valid.");
  ESP_LOGI(kTag, "Confirmed OTA image after stable READY_IDLE window.");
}

}  // namespace update_manager
