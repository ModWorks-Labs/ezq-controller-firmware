#include "wifi_manager.h"

#include <cstring>

#include "debug_console.h"
#include "dev_config.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"

namespace wifi_manager {
namespace {

constexpr char kTag[] = "wifi_manager";

WifiStatus g_status = {
    .initialized = false,
    .connected = false,
    .mdns_ready = false,
    .got_ip = false,
    .retry_count = 0,
    .hostname = dev_config::kDeviceHostname,
    .ip_address = "",
};

esp_netif_t *g_sta_netif = nullptr;
bool g_sleep_suspended = false;

void update_ip_string(const esp_ip4_addr_t &ip) {
  char buffer[16] = {};
  esp_ip4addr_ntoa(&ip, buffer, sizeof(buffer));
  g_status.ip_address = buffer;
}

void ensure_mdns() {
  if (g_status.mdns_ready) {
    return;
  }

  const esp_err_t err = mdns_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    DEV_LOGW(kTag, "mDNS init failed: %s", esp_err_to_name(err));
    return;
  }

  mdns_hostname_set(g_status.hostname.c_str());
  mdns_instance_name_set(dev_config::kMdnsInstanceName);
  g_status.mdns_ready = true;
  DEV_LOGI(kTag, "mDNS registered at %s.local", g_status.hostname.c_str());
}

void connect_station() {
  const esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
  }
}

void event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    DEV_LOGI(kTag, "Wi-Fi station started; connecting to SSID '%s'", dev_config::kWifiSsid);
    connect_station();
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    g_status.connected = false;
    g_status.got_ip = false;
    g_status.ip_address.clear();
    g_status.mdns_ready = false;

    if (g_sleep_suspended) {
      DEV_LOGI(kTag, "Wi-Fi stopped for sleep");
      return;
    }

    if (g_status.retry_count < dev_config::kWifiConnectMaxRetries) {
      ++g_status.retry_count;
      DEV_LOGW(kTag, "Wi-Fi disconnected; retry %d/%d", g_status.retry_count,
               dev_config::kWifiConnectMaxRetries);
      connect_station();
    } else {
      DEV_LOGE(kTag, "Wi-Fi connection retries exhausted");
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    g_status.connected = true;
    g_status.got_ip = true;
    g_status.retry_count = 0;
    update_ip_string(event->ip_info.ip);
    ensure_mdns();
    DEV_LOGI(kTag, "Wi-Fi connected: hostname=%s ip=%s", g_status.hostname.c_str(),
             g_status.ip_address.c_str());
  }
}

}  // namespace

void init() {
  if (g_status.initialized) {
    return;
  }

  const esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(nvs_err);
  }

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  g_sta_netif = esp_netif_create_default_wifi_sta();
  configASSERT(g_sta_netif != nullptr);
  ESP_ERROR_CHECK(esp_netif_set_hostname(g_sta_netif, g_status.hostname.c_str()));

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_config));

  ESP_ERROR_CHECK(
      esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr,
                                          nullptr));
  ESP_ERROR_CHECK(
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr,
                                          nullptr));

  wifi_config_t wifi_config = {};
  memcpy(wifi_config.sta.ssid, dev_config::kWifiSsid, strlen(dev_config::kWifiSsid));
  memcpy(wifi_config.sta.password, dev_config::kWifiPassword, strlen(dev_config::kWifiPassword));
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  g_status.initialized = true;
}

void suspend_for_sleep() {
  if (!g_status.initialized || g_sleep_suspended) {
    return;
  }

  g_sleep_suspended = true;
  const esp_err_t err = esp_wifi_stop();
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "esp_wifi_stop failed before sleep: %s", esp_err_to_name(err));
    g_sleep_suspended = false;
  }
}

void resume_after_sleep() {
  if (!g_status.initialized || !g_sleep_suspended) {
    return;
  }

  const esp_err_t err = esp_wifi_start();
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "esp_wifi_start failed after sleep: %s", esp_err_to_name(err));
    return;
  }

  g_sleep_suspended = false;
  g_status.retry_count = 0;
}

WifiStatus get_status() {
  return g_status;
}

}  // namespace wifi_manager
