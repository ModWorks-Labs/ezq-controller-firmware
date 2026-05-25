#include "wifi_manager.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "debug_console.h"
#include "dev_config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "unit_identity.h"

namespace wifi_manager {
namespace {

constexpr char kTag[] = "wifi_manager";
constexpr char kFactoryPartition[] = "factory_nvs";
constexpr char kWifiNamespace[] = "wifi";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";
constexpr char kProvisionApSuffix[] = "-SETUP";
constexpr char kProvisionApIp[] = "192.168.4.1";
constexpr uint32_t kProvisionConnectTimeoutMs = 15000;

WifiStatus g_status = {
    .initialized = false,
    .connected = false,
    .mdns_ready = false,
    .got_ip = false,
    .credentials_stored = false,
    .ap_active = false,
    .connect_test_active = false,
    .last_connect_success = false,
    .retry_count = 0,
    .mode = WifiMode::STA,
    .hostname = dev_config::kDeviceHostname,
    .ip_address = "",
    .configured_ssid = "",
    .ap_ssid = "",
    .ap_ip_address = "",
    .last_connect_message = "",
};

esp_netif_t *g_sta_netif = nullptr;
esp_netif_t *g_ap_netif = nullptr;
bool g_sleep_suspended = false;
bool g_sta_should_connect = false;

std::string auth_mode_name(wifi_auth_mode_t auth_mode) {
  switch (auth_mode) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
      return "WAPI";
    default:
      return "UNKNOWN";
  }
}

void update_ip_string(const esp_ip4_addr_t &ip) {
  char buffer[16] = {};
  esp_ip4addr_ntoa(&ip, buffer, sizeof(buffer));
  g_status.ip_address = buffer;
}

std::string build_provision_ap_ssid() {
  std::string ssid = unit_identity::has_unit_id() ? unit_identity::unit_id() : unit_identity::kBoardId;
  ssid += kProvisionApSuffix;
  if (ssid.size() > 31) {
    ssid.resize(31);
  }
  return ssid;
}

bool load_credential_string(nvs_handle_t handle, const char *key, std::string &value) {
  size_t required_size = 0;
  esp_err_t err = nvs_get_str(handle, key, nullptr, &required_size);
  if (err == ESP_ERR_NVS_NOT_FOUND || required_size == 0) {
    value.clear();
    return false;
  }
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Failed reading Wi-Fi key '%s' size: %s", key, esp_err_to_name(err));
    value.clear();
    return false;
  }

  std::string temp(required_size, '\0');
  err = nvs_get_str(handle, key, temp.data(), &required_size);
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Failed reading Wi-Fi key '%s': %s", key, esp_err_to_name(err));
    value.clear();
    return false;
  }

  if (!temp.empty() && temp.back() == '\0') {
    temp.pop_back();
  }
  value = temp;
  return !value.empty();
}

bool with_wifi_namespace(nvs_open_mode_t mode, nvs_handle_t &handle) {
  esp_err_t err = nvs_flash_init_partition(kFactoryPartition);
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    DEV_LOGW(kTag, "Factory NVS requested erase for Wi-Fi namespace: %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase_partition(kFactoryPartition));
    err = nvs_flash_init_partition(kFactoryPartition);
  }
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Factory NVS init failed for Wi-Fi namespace: %s", esp_err_to_name(err));
    return false;
  }

  err = nvs_open_from_partition(kFactoryPartition, kWifiNamespace, mode, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Factory NVS open failed for Wi-Fi namespace: %s", esp_err_to_name(err));
    return false;
  }

  return true;
}

bool load_credentials(std::string &ssid, std::string &password) {
  nvs_handle_t handle = 0;
  if (!with_wifi_namespace(NVS_READONLY, handle)) {
    ssid.clear();
    password.clear();
    return false;
  }

  const bool have_ssid = load_credential_string(handle, kSsidKey, ssid);
  load_credential_string(handle, kPasswordKey, password);
  nvs_close(handle);
  return have_ssid && !ssid.empty();
}

bool store_credentials(const std::string &ssid, const std::string &password) {
  nvs_handle_t handle = 0;
  if (!with_wifi_namespace(NVS_READWRITE, handle)) {
    return false;
  }

  esp_err_t err = nvs_set_str(handle, kSsidKey, ssid.c_str());
  if (err == ESP_OK) {
    err = nvs_set_str(handle, kPasswordKey, password.c_str());
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Failed storing Wi-Fi credentials: %s", esp_err_to_name(err));
    return false;
  }

  g_status.credentials_stored = true;
  g_status.configured_ssid = ssid;
  return true;
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

void clear_station_runtime_status() {
  g_status.connected = false;
  g_status.got_ip = false;
  g_status.ip_address.clear();
  g_status.mdns_ready = false;
}

void connect_station() {
  const esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
  }
}

void configure_station(const std::string &ssid, const std::string &password) {
  wifi_config_t wifi_config = {};
  std::memcpy(wifi_config.sta.ssid, ssid.c_str(), std::min<std::size_t>(ssid.size(), sizeof(wifi_config.sta.ssid) - 1));
  std::memcpy(wifi_config.sta.password,
              password.c_str(),
              std::min<std::size_t>(password.size(), sizeof(wifi_config.sta.password) - 1));
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
}

void configure_access_point() {
  g_status.ap_ssid = build_provision_ap_ssid();
  g_status.ap_ip_address = kProvisionApIp;

  wifi_config_t ap_config = {};
  std::memcpy(ap_config.ap.ssid, g_status.ap_ssid.c_str(), g_status.ap_ssid.size());
  ap_config.ap.ssid_len = static_cast<uint8_t>(g_status.ap_ssid.size());
  ap_config.ap.channel = 1;
  ap_config.ap.authmode = WIFI_AUTH_OPEN;
  ap_config.ap.max_connection = 4;
  ap_config.ap.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
}

void event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    if (g_sta_should_connect) {
      if (g_status.mode == WifiMode::APSTA_TEST) {
        DEV_LOGI(kTag, "Provisioning connect test started for SSID '%s'", g_status.configured_ssid.c_str());
      } else {
        DEV_LOGI(kTag, "Wi-Fi station started; connecting to SSID '%s'", g_status.configured_ssid.c_str());
      }
      connect_station();
    }
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    clear_station_runtime_status();

    if (g_sleep_suspended) {
      DEV_LOGI(kTag, "Wi-Fi stopped for sleep");
      return;
    }

    if (g_status.mode == WifiMode::APSTA_TEST) {
      g_status.last_connect_success = false;
      g_status.last_connect_message = "Connection attempt failed; retrying";
    }

    if (g_sta_should_connect && g_status.retry_count < dev_config::kWifiConnectMaxRetries) {
      ++g_status.retry_count;
      DEV_LOGW(kTag, "Wi-Fi disconnected; retry %d/%d", g_status.retry_count, dev_config::kWifiConnectMaxRetries);
      connect_station();
    } else if (g_status.mode == WifiMode::STA) {
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

    if (g_status.mode == WifiMode::STA) {
      ensure_mdns();
      DEV_LOGI(kTag, "Wi-Fi connected: hostname=%s ip=%s", g_status.hostname.c_str(), g_status.ip_address.c_str());
    } else if (g_status.mode == WifiMode::APSTA_TEST) {
      g_status.last_connect_success = true;
      g_status.last_connect_message = "Wi-Fi credentials validated successfully";
      DEV_LOGI(kTag, "Provisioning connect test succeeded for SSID '%s' ip=%s",
               g_status.configured_ssid.c_str(),
               g_status.ip_address.c_str());
    }
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
  g_ap_netif = esp_netif_create_default_wifi_ap();
  configASSERT(g_sta_netif != nullptr);
  configASSERT(g_ap_netif != nullptr);
  ESP_ERROR_CHECK(esp_netif_set_hostname(g_sta_netif, g_status.hostname.c_str()));

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_config));

  ESP_ERROR_CHECK(
      esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, nullptr));
  ESP_ERROR_CHECK(
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, nullptr));

  std::string password;
  g_status.credentials_stored = load_credentials(g_status.configured_ssid, password);

  if (g_status.credentials_stored) {
    configure_station(g_status.configured_ssid, password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    g_status.mode = WifiMode::STA;
    g_sta_should_connect = true;
    DEV_LOGI(kTag, "Stored Wi-Fi credentials found for SSID '%s'", g_status.configured_ssid.c_str());
  } else {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_access_point();
    g_status.mode = WifiMode::PROVISION_AP;
    g_status.ap_active = true;
    g_status.last_connect_message = "Waiting for Wi-Fi credentials";
    g_sta_should_connect = false;
    DEV_LOGI(kTag,
             "No stored Wi-Fi credentials. Provisioning AP active: SSID=%s URL=http://%s/",
             g_status.ap_ssid.c_str(),
             g_status.ap_ip_address.c_str());
  }

  ESP_ERROR_CHECK(esp_wifi_start());
  g_status.initialized = true;
}

bool provisioning_required() {
  return !g_status.credentials_stored;
}

bool scan_networks(std::vector<ScanResult> &results, std::string &message) {
  results.clear();
  if (!g_status.initialized || !g_status.ap_active) {
    message = "Provisioning AP is not active";
    return false;
  }

  wifi_scan_config_t scan_config = {};
  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    message = std::string("Wi-Fi scan failed: ") + esp_err_to_name(err);
    return false;
  }

  uint16_t count = 0;
  err = esp_wifi_scan_get_ap_num(&count);
  if (err != ESP_OK) {
    message = std::string("Failed reading scan result count: ") + esp_err_to_name(err);
    return false;
  }

  std::vector<wifi_ap_record_t> records(count);
  err = esp_wifi_scan_get_ap_records(&count, records.data());
  if (err != ESP_OK) {
    message = std::string("Failed reading scan results: ") + esp_err_to_name(err);
    return false;
  }

  results.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    if (records[i].ssid[0] == '\0') {
      continue;
    }
    results.push_back({
        .ssid = reinterpret_cast<const char *>(records[i].ssid),
        .rssi = records[i].rssi,
        .secure = records[i].authmode != WIFI_AUTH_OPEN,
        .auth_mode = auth_mode_name(records[i].authmode),
    });
  }

  std::sort(results.begin(),
            results.end(),
            [](const ScanResult &lhs, const ScanResult &rhs) { return lhs.rssi > rhs.rssi; });
  message = "Scan complete";
  return true;
}

bool test_and_store_credentials(const std::string &ssid,
                                const std::string &password,
                                std::string &message) {
  if (!g_status.initialized || !g_status.ap_active) {
    message = "Provisioning AP is not active";
    return false;
  }
  if (ssid.empty() || ssid.size() > 32) {
    message = "SSID must be 1-32 characters";
    return false;
  }
  if (password.size() > 64) {
    message = "Password must be 0-64 characters";
    return false;
  }

  g_status.mode = WifiMode::APSTA_TEST;
  g_status.connect_test_active = true;
  g_status.last_connect_success = false;
  g_status.last_connect_message = "Connecting to selected network";
  g_status.retry_count = 0;
  g_status.configured_ssid = ssid;
  g_sta_should_connect = true;

  clear_station_runtime_status();
  const esp_err_t disconnect_err = esp_wifi_disconnect();
  if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
    DEV_LOGW(kTag, "esp_wifi_disconnect before provisioning test failed: %s", esp_err_to_name(disconnect_err));
  }
  configure_station(ssid, password);
  connect_station();

  const int64_t deadline_us =
      esp_timer_get_time() + static_cast<int64_t>(kProvisionConnectTimeoutMs) * 1000LL;
  while (esp_timer_get_time() < deadline_us) {
    if (g_status.connected && g_status.got_ip) {
      if (!store_credentials(ssid, password)) {
        g_status.connect_test_active = false;
        g_status.mode = WifiMode::PROVISION_AP;
        g_sta_should_connect = false;
        g_status.last_connect_success = false;
        g_status.last_connect_message = "Connected, but saving credentials failed";
        esp_wifi_disconnect();
        clear_station_runtime_status();
        message = g_status.last_connect_message;
        return false;
      }

      g_status.connect_test_active = false;
      g_status.mode = WifiMode::APSTA_TEST;
      g_status.last_connect_success = true;
      g_status.last_connect_message = "Wi-Fi credentials saved successfully";
      message = g_status.last_connect_message;
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  esp_wifi_disconnect();
  g_status.connect_test_active = false;
  g_status.mode = WifiMode::PROVISION_AP;
  g_sta_should_connect = false;
  g_status.last_connect_success = false;
  g_status.last_connect_message = "Connection test timed out";
  clear_station_runtime_status();
  message = g_status.last_connect_message;
  return false;
}

bool clear_credentials(std::string &message) {
  nvs_handle_t handle = 0;
  if (!with_wifi_namespace(NVS_READWRITE, handle)) {
    g_status.credentials_stored = false;
    g_status.configured_ssid.clear();
    message = "No stored Wi-Fi credentials were present";
    return true;
  }

  esp_err_t err = nvs_erase_key(handle, kSsidKey);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = ESP_OK;
  }
  if (err == ESP_OK) {
    esp_err_t password_err = nvs_erase_key(handle, kPasswordKey);
    if (password_err != ESP_OK && password_err != ESP_ERR_NVS_NOT_FOUND) {
      err = password_err;
    }
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  if (err != ESP_OK) {
    message = std::string("Failed to clear Wi-Fi credentials: ") + esp_err_to_name(err);
    DEV_LOGW(kTag, "%s", message.c_str());
    return false;
  }

  g_status.credentials_stored = false;
  g_status.configured_ssid.clear();
  g_status.last_connect_success = false;
  g_status.last_connect_message = "Wi-Fi credentials cleared";
  message = "Wi-Fi credentials cleared";
  return true;
}

void suspend_for_sleep() {
  if (!g_status.initialized || g_sleep_suspended || g_status.mode != WifiMode::STA) {
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
  if (!g_status.initialized || !g_sleep_suspended || g_status.mode != WifiMode::STA) {
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
