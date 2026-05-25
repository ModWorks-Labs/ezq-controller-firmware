#include "unit_identity.h"

#include <array>
#include <cstdio>

#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "debug_console.h"

namespace unit_identity {
namespace {

constexpr char kTag[] = "unit_identity";
constexpr char kFactoryPartition[] = "factory_nvs";
constexpr char kFactoryNamespace[] = "identity";
constexpr char kUnitIdKey[] = "unit_id";
constexpr char kBootstrapUnitId[] = "EZQ-CTLR-B-0006";
constexpr std::array<uint8_t, 6> kBootstrapMac = {0x8C, 0xFD, 0x49, 0x2B, 0xBE, 0xC4};

std::array<uint8_t, 6> g_factory_mac = {};
bool g_factory_mac_valid = false;
std::string g_unit_id;

bool read_factory_mac(std::array<uint8_t, 6> &mac) {
  return esp_efuse_mac_get_default(mac.data()) == ESP_OK;
}

std::string mac_string(const std::array<uint8_t, 6> &mac) {
  char buffer[18] = {};
  snprintf(buffer,
           sizeof(buffer),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],
           mac[1],
           mac[2],
           mac[3],
           mac[4],
           mac[5]);
  return std::string(buffer);
}

bool load_unit_id(nvs_handle_t handle, std::string &unit_id) {
  size_t required_size = 0;
  esp_err_t err = nvs_get_str(handle, kUnitIdKey, nullptr, &required_size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }
  if (err != ESP_OK || required_size == 0) {
    DEV_LOGW(kTag, "Failed reading stored unit_id size: %s", esp_err_to_name(err));
    return false;
  }

  std::string value(required_size, '\0');
  err = nvs_get_str(handle, kUnitIdKey, value.data(), &required_size);
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Failed reading stored unit_id: %s", esp_err_to_name(err));
    return false;
  }

  if (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }

  unit_id = value;
  return !unit_id.empty();
}

bool store_unit_id(nvs_handle_t handle, const char *value) {
  esp_err_t err = nvs_set_str(handle, kUnitIdKey, value);
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Failed writing unit_id to factory partition: %s", esp_err_to_name(err));
    return false;
  }

  err = nvs_commit(handle);
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Failed committing unit_id to factory partition: %s", esp_err_to_name(err));
    return false;
  }

  return true;
}

}  // namespace

void init() {
  g_factory_mac_valid = read_factory_mac(g_factory_mac);
  if (!g_factory_mac_valid) {
    DEV_LOGW(kTag, "Factory MAC unavailable");
    return;
  }

  esp_err_t err = nvs_flash_init_partition(kFactoryPartition);
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    DEV_LOGW(kTag, "Factory partition init requested erase: %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase_partition(kFactoryPartition));
    err = nvs_flash_init_partition(kFactoryPartition);
  }
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Factory partition init failed: %s", esp_err_to_name(err));
    return;
  }

  nvs_handle_t handle = 0;
  err = nvs_open_from_partition(kFactoryPartition, kFactoryNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Factory partition open failed: %s", esp_err_to_name(err));
    return;
  }

  if (load_unit_id(handle, g_unit_id)) {
    DEV_LOGI(kTag, "Loaded unit_id from factory partition: %s", g_unit_id.c_str());
    nvs_close(handle);
    return;
  }

  if (g_factory_mac == kBootstrapMac) {
    if (store_unit_id(handle, kBootstrapUnitId)) {
      g_unit_id = kBootstrapUnitId;
      DEV_LOGI(kTag,
               "Provisioned factory unit_id for bench unit: %s (%s)",
               g_unit_id.c_str(),
               mac_string(g_factory_mac).c_str());
    }
  } else {
    DEV_LOGW(kTag,
             "Factory partition has no unit_id for MAC %s",
             mac_string(g_factory_mac).c_str());
  }

  nvs_close(handle);
}

std::string factory_mac_string() {
  if (!g_factory_mac_valid && !read_factory_mac(g_factory_mac)) {
    return "unavailable";
  }
  g_factory_mac_valid = true;
  return mac_string(g_factory_mac);
}

std::string unit_id() {
  return g_unit_id;
}

bool has_unit_id() {
  return !g_unit_id.empty();
}

}  // namespace unit_identity
