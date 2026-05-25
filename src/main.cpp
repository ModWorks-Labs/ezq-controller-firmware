#include "debug_console.h"
#include "control_runtime.h"
#include "dev_config.h"
#include "ota_service.h"
#include "update_manager.h"
#include "wifi_manager.h"
#include "unit_identity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pinout.h"

namespace {

constexpr char kTag[] = "app_main";
constexpr TickType_t kLoopDelay = pdMS_TO_TICKS(10);

const char *wifi_mode_label(wifi_manager::WifiMode mode) {
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

void log_runtime_status();
void log_dev_hold_status(bool low, uint64_t stable_low_ms);

bool boot9_is_low() {
  return gpio_get_level(static_cast<gpio_num_t>(pinout::kBootStrapPin)) == 0;
}

void init_boot_hold_pin() {
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << pinout::kBootStrapPin;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&io));
}

void wait_for_boot9_hold_release() {
  DEV_LOGI(kTag,
           "\n--- DEV HOLD ---\n"
           "Waiting for GPIO%d low and stable for %d ms before control init.\n",
           pinout::kBootStrapPin,
           dev_config::kDevHoldSettleMs);

  uint64_t stable_low_since_us = 0;
  uint64_t next_hold_log_us = 0;

  while (true) {
    ota_service::poll();

    const uint64_t now_us = esp_timer_get_time();
    const bool low = boot9_is_low();
    if (low) {
      if (stable_low_since_us == 0) {
        stable_low_since_us = now_us;
      }
      const uint64_t stable_ms = (now_us - stable_low_since_us) / 1000ULL;
      if (stable_ms >= static_cast<uint64_t>(dev_config::kDevHoldSettleMs)) {
        DEV_LOGI(kTag,
                 "\nGPIO%d has been low for %llu ms.\nLeaving developer hold.\n",
                 pinout::kBootStrapPin,
                 static_cast<unsigned long long>(stable_ms));
        return;
      }
    } else {
      stable_low_since_us = 0;
    }

    if (next_hold_log_us == 0 || now_us >= next_hold_log_us) {
      const uint64_t stable_ms = stable_low_since_us == 0 ? 0 : (now_us - stable_low_since_us) / 1000ULL;
      log_dev_hold_status(low, stable_ms);
      next_hold_log_us =
          now_us + static_cast<uint64_t>(dev_config::kDevHoldStatusMs) * 1000ULL;
    }

    vTaskDelay(pdMS_TO_TICKS(dev_config::kDevHoldPollMs));
  }
}

void log_runtime_status() {
  const auto wifi = wifi_manager::get_status();
  const auto control = control_app::get_status();
  char temp_line[48] = {};
  char battery_line[48] = {};
  char detail_line[48] = {};

  snprintf(temp_line,
           sizeof(temp_line),
           "Temp: %s",
           control.temp_valid ? "" : "n/a");
  if (control.temp_valid) {
    snprintf(temp_line, sizeof(temp_line), "Temp: %.2f C", control.temp_c);
  }

  snprintf(battery_line,
           sizeof(battery_line),
           "Battery: %s",
           control.battery_valid ? "" : "n/a");
  if (control.battery_valid) {
    snprintf(battery_line, sizeof(battery_line), "Battery: %.2f V", control.battery_v);
  }

  if (control.detail_name[0] != '\0') {
    snprintf(detail_line, sizeof(detail_line), "Detail: %s\n", control.detail_name);
  }

  DEV_LOGI(kTag,
           "\n--- %s ---\n"
           "WiFi Mode: %s\n"
           "WiFi: %s\n"
           "IP: %s:%d\n"
           "Setup AP: %s\n"
           "%s"
           "%s\n"
           "%s\n",
           control.state_name,
           wifi_mode_label(wifi.mode),
           wifi.connected ? "Connected" : "Disconnected",
           wifi.ip_address.empty() ? "n/a" : wifi.ip_address.c_str(),
           dev_config::kOtaHttpPort,
           wifi.ap_active ? wifi.ap_ssid.c_str() : "n/a",
           detail_line,
           temp_line,
           battery_line);
}

void log_dev_hold_status(bool low, uint64_t stable_low_ms) {
  const auto wifi = wifi_manager::get_status();

  DEV_LOGI(kTag,
           "\n--- DEV HOLD ---\n"
           "WiFi Mode: %s\n"
           "WiFi: %s\n"
           "IP: %s:%d\n"
           "Debug: %s:%d\n"
           "Setup AP: %s\n"
           "BOOT9: %s\n"
           "Stable Low: %llu / %d ms\n"
           "Waiting for full hold duration...\n",
           wifi_mode_label(wifi.mode),
           wifi.connected ? "Connected" : "Disconnected",
           wifi.ip_address.empty() ? "n/a" : wifi.ip_address.c_str(),
           dev_config::kOtaHttpPort,
           wifi.ip_address.empty() ? "n/a" : wifi.ip_address.c_str(),
           dev_config::kDebugTcpPort,
           wifi.ap_active ? wifi.ap_ssid.c_str() : "n/a",
           low ? "Low" : "High",
           static_cast<unsigned long long>(stable_low_ms),
           dev_config::kDevHoldSettleMs);
}

}

extern "C" void app_main() {
  esp_log_level_set("gpio", ESP_LOG_WARN);
  esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
  esp_log_level_set("mdns_mem", ESP_LOG_WARN);

  DEV_LOGI(kTag, "EZQ controller firmware bootstrap starting");
  init_boot_hold_pin();
  DEV_LOGI(kTag, "Initializing unit identity...");
  unit_identity::init();
  DEV_LOGI(kTag, "Initializing update manager...");
  update_manager::init();

  DEV_LOGI(kTag, "Initializing Wi-Fi manager...");
  wifi_manager::init();
  DEV_LOGI(kTag, "Initializing debug console...");
  debug_console::init();
  DEV_LOGI(kTag, "Initializing OTA service...");
  ota_service::init();
  if (boot9_is_low()) {
    wait_for_boot9_hold_release();
  }
  DEV_LOGI(kTag, "\nInitializing control app...");
  control_app::init();

  DEV_LOGI(kTag, "Control app initialized successfully.\n");
  log_runtime_status();
  uint64_t next_status_log_us = 0;

  while (true) {
    ota_service::poll();
    control_app::tick();

    const auto wifi = wifi_manager::get_status();
    const uint64_t now_us = esp_timer_get_time();
    const uint64_t status_period_us =
        static_cast<uint64_t>((wifi.connected ? dev_config::kStatusConnectedLogMs
                                              : dev_config::kStatusDisconnectedLogMs)) *
        1000ULL;
    if (next_status_log_us == 0 || now_us >= next_status_log_us) {
      log_runtime_status();
      next_status_log_us = now_us + status_period_us;
    }

    vTaskDelay(kLoopDelay);
  }
}
