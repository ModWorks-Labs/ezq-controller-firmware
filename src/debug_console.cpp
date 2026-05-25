#include "debug_console.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include <lwip/sockets.h>
#include <unistd.h>

#include "control_runtime.h"
#include "config_store.h"
#include "dev_config.h"
#include "esp_check.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ota_service.h"
#include "update_manager.h"
#include "unit_identity.h"
#include "wifi_manager.h"

namespace debug_console {
namespace {

constexpr char kTag[] = "debug_console";
constexpr size_t kLogBufferSize = 512;
constexpr size_t kLineBufferSize = 256;
constexpr int kListenBacklog = 1;
constexpr TickType_t kClientPollDelay = pdMS_TO_TICKS(25);
constexpr uint32_t kServerTaskStackBytes = 4096;
constexpr uint32_t kClientTaskStackBytes = 6144;

SemaphoreHandle_t g_client_mutex = nullptr;
int g_listen_fd = -1;
int g_client_fd = -1;
bool g_monitor_enabled = false;

const char *fault_name(ControlFaultKind fault_kind) {
  switch (fault_kind) {
    case ControlFaultKind::NONE:
      return "none";
    case ControlFaultKind::INIT_FAILURE:
      return "init_failure";
    case ControlFaultKind::SENSOR_FAILURE:
      return "sensor_failure";
    case ControlFaultKind::OVERTEMPERATURE:
      return "overtemperature";
    case ControlFaultKind::BATTERY_UNDERVOLTAGE:
      return "battery_undervoltage";
    default:
      return "unknown";
  }
}

const char *level_to_letter(esp_log_level_t level) {
  switch (level) {
    case ESP_LOG_ERROR:
      return "E";
    case ESP_LOG_WARN:
      return "W";
    case ESP_LOG_INFO:
      return "I";
    case ESP_LOG_DEBUG:
      return "D";
    case ESP_LOG_VERBOSE:
      return "V";
    case ESP_LOG_NONE:
    default:
      return "N";
  }
}

const char *wifi_mode_name(wifi_manager::WifiMode mode) {
  switch (mode) {
    case wifi_manager::WifiMode::STA:
      return "STA";
    case wifi_manager::WifiMode::PROVISION_AP:
      return "PROVISION_AP";
    case wifi_manager::WifiMode::APSTA_TEST:
      return "APSTA_TEST";
    default:
      return "UNKNOWN";
  }
}

void disconnect_client_locked() {
  if (g_client_fd >= 0) {
    shutdown(g_client_fd, SHUT_RDWR);
    close(g_client_fd);
    g_client_fd = -1;
  }
  g_monitor_enabled = false;
}

void send_text(const char *text) {
  if (text == nullptr || g_client_mutex == nullptr) {
    return;
  }

  if (xSemaphoreTake(g_client_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  if (g_client_fd >= 0) {
    const size_t length = strlen(text);
    const int sent = send(g_client_fd, text, static_cast<int>(length), 0);
    if (sent < 0) {
      disconnect_client_locked();
    }
  }

  xSemaphoreGive(g_client_mutex);
}

void send_line(const std::string &line) {
  send_text(line.c_str());
}

std::string build_status_text() {
  const auto wifi = wifi_manager::get_status();
  const auto ota = ota_service::get_status();
  const auto control = control_app::get_status();
  const auto update = update_manager::get_status();

  char buffer[1152] = {};
  snprintf(buffer,
           sizeof(buffer),
           "wifi mode=%s connected=%s ip=%s hostname=%s retries=%d mdns=%s creds=%s ap=%s ap_ip=%s\n"
           "ota server=%s reboot_pending=%s running=%s boot=%s next=%s\n"
           "update current=%s available=%s result=%s pending_confirmation=%s rollback_armed=%s\n"
           "control state=%s detail=%s elapsed_ms=%lu total_ms=%lu total_known=%s "
           "can_start_cycle=%s can_start_blower=%s can_abort_cycle=%s can_sleep=%s "
           "fan=%s glow=%s fault=%s temp=%s batt=%s\n",
           wifi_mode_name(wifi.mode),
           wifi.connected ? "yes" : "no",
           wifi.ip_address.empty() ? "n/a" : wifi.ip_address.c_str(),
           wifi.hostname.c_str(),
           wifi.retry_count,
           wifi.mdns_ready ? "yes" : "no",
           wifi.credentials_stored ? "yes" : "no",
           wifi.ap_active ? wifi.ap_ssid.c_str() : "n/a",
           wifi.ap_active ? wifi.ap_ip_address.c_str() : "n/a",
           ota.server_running ? "yes" : "no",
           ota.reboot_pending ? "yes" : "no",
           ota.running_partition.c_str(),
           ota.boot_partition.c_str(),
           ota.next_update_partition.c_str(),
           update.current_version.c_str(),
           update.available_version.empty() ? "n/a" : update.available_version.c_str(),
           update.last_result.empty() ? "-" : update.last_result.c_str(),
           update.pending_confirmation ? "yes" : "no",
           update.rollback_armed ? "yes" : "no",
           control.state_name,
           control.detail_name[0] != '\0' ? control.detail_name : "-",
           static_cast<unsigned long>(control.elapsed_ms),
           static_cast<unsigned long>(control.total_ms),
           control.has_total_ms ? "yes" : "no",
           control.can_start_cycle ? "yes" : "no",
           control.can_start_blower ? "yes" : "no",
           control.can_abort_cycle ? "yes" : "no",
           control.can_sleep ? "yes" : "no",
           control.fan_active ? "on" : "off",
           control.glow_plug_active ? "on" : "off",
           fault_name(control.fault_kind),
           control.temp_valid ? "ok" : "n/a",
           control.battery_valid ? "ok" : "n/a");
  return std::string(buffer);
}

std::string build_info_text() {
  const auto wifi = wifi_manager::get_status();
  const auto ota = ota_service::get_status();
  const auto control = control_app::get_status();
  const auto update = update_manager::get_status();
  char temp_text[32] = {};
  char battery_text[32] = {};

  snprintf(temp_text,
           sizeof(temp_text),
           "%s",
           control.temp_valid ? "" : "n/a");
  if (control.temp_valid) {
    snprintf(temp_text, sizeof(temp_text), "%.2f C", static_cast<double>(control.temp_c));
  }

  snprintf(battery_text,
           sizeof(battery_text),
           "%s",
           control.battery_valid ? "" : "n/a");
  if (control.battery_valid) {
    snprintf(
        battery_text, sizeof(battery_text), "%.2f V", static_cast<double>(control.battery_v));
  }

  char buffer[768] = {};
  snprintf(buffer,
           sizeof(buffer),
           "board_id=%s\n"
           "unit_id=%s\n"
           "factory_mac=%s\n"
           "firmware_version=%s\n"
           "wifi_mode=%s\n"
           "wifi_credentials_stored=%s\n"
           "wifi_connected=%s\n"
           "hostname=%s\n"
           "ip=%s\n"
           "mdns=%s\n"
           "setup_ap_ssid=%s\n"
           "setup_ap_ip=%s\n"
           "ota_server=%s\n"
           "running_partition=%s\n"
           "boot_partition=%s\n"
           "next_update_partition=%s\n"
           "available_firmware_version=%s\n"
           "update_result=%s\n"
           "update_message=%s\n"
           "pending_confirmation=%s\n"
           "rollback_armed=%s\n"
           "state=%s\n"
           "detail=%s\n"
           "temp=%s\n"
           "battery=%s\n",
           unit_identity::kBoardId,
           unit_identity::has_unit_id() ? unit_identity::unit_id().c_str() : "unprovisioned",
           unit_identity::factory_mac_string().c_str(),
           update.current_version.c_str(),
           wifi_mode_name(wifi.mode),
           wifi.credentials_stored ? "yes" : "no",
           wifi.connected ? "yes" : "no",
           wifi.hostname.c_str(),
           wifi.ip_address.empty() ? "n/a" : wifi.ip_address.c_str(),
           wifi.mdns_ready ? "yes" : "no",
           wifi.ap_active ? wifi.ap_ssid.c_str() : "n/a",
           wifi.ap_active ? wifi.ap_ip_address.c_str() : "n/a",
           ota.server_running ? "yes" : "no",
           ota.running_partition.c_str(),
           ota.boot_partition.c_str(),
           ota.next_update_partition.c_str(),
           update.available_version.empty() ? "n/a" : update.available_version.c_str(),
           update.last_result.empty() ? "-" : update.last_result.c_str(),
           update.last_message.empty() ? "-" : update.last_message.c_str(),
           update.pending_confirmation ? "yes" : "no",
           update.rollback_armed ? "yes" : "no",
           control.state_name,
           control.detail_name[0] != '\0' ? control.detail_name : "-",
           temp_text,
           battery_text);
  return std::string(buffer);
}

std::string build_settings_text() {
  const auto &settings = config_store::settings();
  char buffer[384] = {};
  snprintf(buffer,
           sizeof(buffer),
           "idle_sleep_timeout_ms=%lu\n"
           "abort_blower_duration_ms=%lu\n"
           "temp_fault_high_c=%.1f\n"
           "temp_fault_low_c=%.1f\n"
           "battery_warning_v=%.2f\n"
           "battery_fault_v=%.2f\n"
           "sound_volume=%u\n"
           "button_press_beep_enabled=%s\n"
           "active_profile=%s\n",
           static_cast<unsigned long>(settings.idle_sleep_timeout_ms),
           static_cast<unsigned long>(settings.abort_blower_duration_ms),
           static_cast<double>(settings.temp_fault_high_c),
           static_cast<double>(settings.temp_fault_low_c),
           static_cast<double>(settings.battery_warning_v),
           static_cast<double>(settings.battery_fault_low_v),
           static_cast<unsigned>(settings.sound_volume),
           settings.button_press_beep_enabled ? "true" : "false",
           settings.active_profile_name.data());
  return std::string(buffer);
}

void handle_command(const std::string &line) {
  if (line == "help") {
    send_text("Commands: help, info, restart, wifi_reset, status, wifi, ota, mac, settings, set <key> <value>, monitor on|off\n");
    return;
  }

  if (line == "info") {
    send_line(build_info_text());
    return;
  }

  if (line == "restart") {
    send_text("Restarting device...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return;
  }

  if (line == "wifi_reset") {
    std::string message;
    if (!wifi_manager::clear_credentials(message)) {
      send_line(message + "\n");
      return;
    }
    send_text("Wi-Fi credentials cleared. Rebooting into provisioning AP...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return;
  }

  if (line == "status") {
    send_line(build_status_text());
    return;
  }

  if (line == "wifi") {
    const auto wifi = wifi_manager::get_status();
    char buffer[256] = {};
    snprintf(buffer,
             sizeof(buffer),
             "mode=%s connected=%s creds=%s hostname=%s ip=%s retries=%d mdns=%s ap=%s ap_ip=%s message=%s\n",
             wifi_mode_name(wifi.mode),
             wifi.connected ? "yes" : "no",
             wifi.credentials_stored ? "yes" : "no",
             wifi.hostname.c_str(),
             wifi.ip_address.empty() ? "n/a" : wifi.ip_address.c_str(),
             wifi.retry_count,
             wifi.mdns_ready ? "yes" : "no",
             wifi.ap_active ? wifi.ap_ssid.c_str() : "n/a",
             wifi.ap_active ? wifi.ap_ip_address.c_str() : "n/a",
             wifi.last_connect_message.empty() ? "-" : wifi.last_connect_message.c_str());
    send_text(buffer);
    return;
  }

  if (line == "ota") {
    const auto ota = ota_service::get_status();
    const auto update = update_manager::get_status();
    char buffer[512] = {};
    snprintf(buffer,
             sizeof(buffer),
             "server=%s reboot_pending=%s running=%s boot=%s next=%s current=%s available=%s result=%s pending_confirmation=%s rollback_armed=%s\n",
             ota.server_running ? "yes" : "no",
             ota.reboot_pending ? "yes" : "no",
             ota.running_partition.c_str(),
             ota.boot_partition.c_str(),
             ota.next_update_partition.c_str(),
             update.current_version.c_str(),
             update.available_version.empty() ? "n/a" : update.available_version.c_str(),
             update.last_result.empty() ? "-" : update.last_result.c_str(),
             update.pending_confirmation ? "yes" : "no",
             update.rollback_armed ? "yes" : "no");
    send_text(buffer);
    return;
  }

  if (line == "mac") {
    char buffer[256] = {};
    snprintf(buffer,
             sizeof(buffer),
             "board_id=%s factory_mac=%s unit_id=%s\n",
             unit_identity::kBoardId,
             unit_identity::factory_mac_string().c_str(),
             unit_identity::has_unit_id() ? unit_identity::unit_id().c_str() : "unprovisioned");
    send_text(buffer);
    return;
  }

  if (line == "settings") {
    send_line(build_settings_text());
    return;
  }

  if (line == "monitor on") {
    g_monitor_enabled = true;
    send_text("Serial monitor enabled.\n");
    return;
  }

  if (line == "monitor off") {
    g_monitor_enabled = false;
    send_text("Serial monitor disabled.\n");
    return;
  }

  if (line.rfind("set ", 0) == 0) {
    std::istringstream stream(line);
    std::string command;
    std::string key;
    std::string value;
    stream >> command >> key >> value;
    if (key.empty() || value.empty()) {
      send_text("Usage: set <key> <value>\n");
      return;
    }

    std::string message;
    if (config_store::update_setting(key, value, message)) {
      send_text((message + "\n").c_str());
    } else {
      send_text(("Failed: " + message + "\n").c_str());
    }
    return;
  }

  send_text("Unknown command. Type 'help'.\n");
}

void client_session_task(void *parameter) {
  const int client_fd = static_cast<int>(reinterpret_cast<intptr_t>(parameter));
  std::string pending;
  pending.reserve(kLineBufferSize);

  send_text("EZQ debug console ready. Type 'help'.\n");

  while (true) {
    char buffer[128] = {};
    const int received = recv(client_fd, buffer, sizeof(buffer), 0);
    if (received == 0) {
      break;
    }
    if (received < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        vTaskDelay(kClientPollDelay);
        continue;
      }
      break;
    }

    for (int i = 0; i < received; ++i) {
      const char ch = buffer[i];
      if (ch == '\r') {
        continue;
      }
      if (ch == '\n') {
        if (!pending.empty()) {
          handle_command(pending);
          pending.clear();
        }
        continue;
      }
      if (pending.size() < (kLineBufferSize - 1)) {
        pending.push_back(ch);
      }
    }
  }

  if (xSemaphoreTake(g_client_mutex, portMAX_DELAY) == pdTRUE) {
    if (g_client_fd == client_fd) {
      disconnect_client_locked();
    }
    xSemaphoreGive(g_client_mutex);
  }

  DEV_LOGI(kTag, "Debug console client disconnected");
  vTaskDelete(nullptr);
}

void server_task(void *) {
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(dev_config::kDebugTcpPort);

  g_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (g_listen_fd < 0) {
    DEV_LOGE(kTag, "Failed to create debug socket: errno=%d", errno);
    vTaskDelete(nullptr);
    return;
  }

  int enable = 1;
  setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  if (bind(g_listen_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    DEV_LOGE(kTag, "Failed to bind debug socket: errno=%d", errno);
    close(g_listen_fd);
    g_listen_fd = -1;
    vTaskDelete(nullptr);
    return;
  }

  if (listen(g_listen_fd, kListenBacklog) != 0) {
    DEV_LOGE(kTag, "Failed to listen on debug socket: errno=%d", errno);
    close(g_listen_fd);
    g_listen_fd = -1;
    vTaskDelete(nullptr);
    return;
  }

  DEV_LOGI(kTag, "Debug console listening on TCP port %d", dev_config::kDebugTcpPort);

  while (true) {
    sockaddr_in client_address = {};
    socklen_t client_len = sizeof(client_address);
    const int accepted_fd =
        accept(g_listen_fd, reinterpret_cast<sockaddr *>(&client_address), &client_len);
    if (accepted_fd < 0) {
      DEV_LOGW(kTag, "Debug console accept failed: errno=%d", errno);
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000;
    setsockopt(accepted_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (xSemaphoreTake(g_client_mutex, portMAX_DELAY) != pdTRUE) {
      close(accepted_fd);
      continue;
    }

    disconnect_client_locked();
    g_client_fd = accepted_fd;
    xSemaphoreGive(g_client_mutex);

    DEV_LOGI(kTag, "Debug console client connected");
    xTaskCreate(client_session_task, "dbg_client", kClientTaskStackBytes, reinterpret_cast<void *>(accepted_fd), 5,
                nullptr);
  }
}

}  // namespace

void init() {
  if (g_client_mutex != nullptr) {
    return;
  }

  g_client_mutex = xSemaphoreCreateMutex();
  configASSERT(g_client_mutex != nullptr);
  xTaskCreate(server_task, "dbg_server", kServerTaskStackBytes, nullptr, 5, nullptr);
}

bool client_connected() {
  if (g_client_mutex == nullptr) {
    return false;
  }

  bool connected = false;
  if (xSemaphoreTake(g_client_mutex, portMAX_DELAY) == pdTRUE) {
    connected = g_client_fd >= 0;
    xSemaphoreGive(g_client_mutex);
  }
  return connected;
}

void log_message(esp_log_level_t level, const char *tag, const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_message_v(level, tag, format, args);
  va_end(args);
}

void log_message_v(esp_log_level_t level, const char *tag, const char *format, va_list args) {
  char message[kLogBufferSize] = {};
  const int count = vsnprintf(message, sizeof(message), format, args);
  if (count <= 0) {
    return;
  }

  const bool has_newline = count > 0 && message[count - 1] == '\n';
  esp_log_write(level, tag != nullptr ? tag : "app", "%s%s", message, has_newline ? "" : "\n");

  std::string outbound;
  outbound.reserve(static_cast<size_t>(count) + 16);
  outbound.push_back('[');
  outbound.append(level_to_letter(level));
  outbound.append("] ");
  outbound.append(tag != nullptr ? tag : "app");
  outbound.append(": ");
  outbound.append(message);
  if (outbound.empty() || outbound.back() != '\n') {
    outbound.push_back('\n');
  }
  if (g_monitor_enabled) {
    send_line(outbound);
  }
}

}  // namespace debug_console
