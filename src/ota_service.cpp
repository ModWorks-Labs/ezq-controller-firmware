#include "ota_service.h"

#include <cstring>

#include "debug_console.h"
#include "dev_config.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace ota_service {
namespace {

constexpr char kTag[] = "ota_service";
constexpr char kStatusUri[] = "/api/dev/status";
constexpr char kOtaUri[] = "/api/dev/ota";
constexpr int64_t kRebootDelayUs = 500000;

httpd_handle_t g_server = nullptr;
bool g_reboot_pending = false;
int64_t g_reboot_at_us = 0;

std::string partition_label(const esp_partition_t *partition) {
  return partition == nullptr ? "n/a" : std::string(partition->label);
}

esp_err_t status_handler(httpd_req_t *request) {
  const auto status = get_status();

  char body[384] = {};
  snprintf(body,
           sizeof(body),
           "{\"ok\":true,\"ota_server\":%s,\"reboot_pending\":%s,"
           "\"running_partition\":\"%s\",\"boot_partition\":\"%s\","
           "\"next_update_partition\":\"%s\"}",
           status.server_running ? "true" : "false",
           status.reboot_pending ? "true" : "false",
           status.running_partition.c_str(),
           status.boot_partition.c_str(),
           status.next_update_partition.c_str());

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, body);
}

esp_err_t ota_post_handler(httpd_req_t *request) {
  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
  if (update_partition == nullptr) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
    return ESP_FAIL;
  }

  esp_ota_handle_t ota_handle = 0;
  esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    DEV_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    return err;
  }

  char buffer[1024] = {};
  int remaining = request->content_len;
  while (remaining > 0) {
    const int read = httpd_req_recv(request, buffer, remaining > static_cast<int>(sizeof(buffer))
                                                 ? static_cast<int>(sizeof(buffer))
                                                 : remaining);
    if (read <= 0) {
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA receive failed");
      return ESP_FAIL;
    }

    err = esp_ota_write(ota_handle, buffer, static_cast<size_t>(read));
    if (err != ESP_OK) {
      DEV_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(err));
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
      return err;
    }

    remaining -= read;
  }

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    DEV_LOGE(kTag, "esp_ota_end failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid firmware image");
    return err;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    DEV_LOGE(kTag, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to select boot slot");
    return err;
  }

  DEV_LOGI(kTag, "OTA image written to partition '%s'", update_partition->label);

  httpd_resp_set_type(request, "application/json");
  httpd_resp_sendstr(request,
                     "{\"ok\":true,\"message\":\"Firmware uploaded successfully. Rebooting.\"}");

  g_reboot_pending = true;
  g_reboot_at_us = esp_timer_get_time() + kRebootDelayUs;
  return ESP_OK;
}

}  // namespace

void init() {
  if (g_server != nullptr) {
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = dev_config::kOtaHttpPort;
  config.stack_size = 8192;

  ESP_ERROR_CHECK(httpd_start(&g_server, &config));

  const httpd_uri_t status_uri = {
      .uri = kStatusUri,
      .method = HTTP_GET,
      .handler = status_handler,
      .user_ctx = nullptr,
  };

  const httpd_uri_t ota_uri = {
      .uri = kOtaUri,
      .method = HTTP_POST,
      .handler = ota_post_handler,
      .user_ctx = nullptr,
  };

  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &status_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &ota_uri));

  DEV_LOGI(kTag, "OTA HTTP server listening on port %d", dev_config::kOtaHttpPort);
}

void poll() {
  if (g_reboot_pending && esp_timer_get_time() >= g_reboot_at_us) {
    DEV_LOGI(kTag, "Rebooting into updated firmware");
    esp_restart();
  }
}

OtaStatus get_status() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);

  return {
      .server_running = g_server != nullptr,
      .reboot_pending = g_reboot_pending,
      .running_partition = partition_label(running),
      .boot_partition = partition_label(boot),
      .next_update_partition = partition_label(next),
  };
}

}  // namespace ota_service
