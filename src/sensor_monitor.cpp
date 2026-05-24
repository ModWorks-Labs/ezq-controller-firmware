#include "sensor_monitor.h"

#include <cstring>

#include "debug_console.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h"
#include "pinout.h"

namespace sensor_monitor {
namespace {

constexpr char kTag[] = "sensor_monitor";
constexpr i2c_port_t kI2cPort = I2C_NUM_0;
constexpr uint8_t kTmp1075Address = 0x48;
constexpr uint8_t kTmp1075TempReg = 0x00;
constexpr uint32_t kI2cFreqHz = 100000;
constexpr uint32_t kTempRefreshMs = 500;
constexpr uint32_t kBatteryRefreshMs = 500;

bool g_initialized = false;
Snapshot g_snapshot = {false, false, 0.0f, false, 0.0f};
uint32_t g_last_temp_read_ms = 0;
uint32_t g_last_battery_read_ms = 0;
adc_oneshot_unit_handle_t g_adc_handle = nullptr;
adc_channel_t g_adc_channel = ADC_CHANNEL_0;

bool read_tmp1075(float &temperature_c) {
  uint8_t reg = kTmp1075TempReg;
  uint8_t buffer[2] = {};
  const esp_err_t err = i2c_master_write_read_device(kI2cPort,
                                                      kTmp1075Address,
                                                      &reg,
                                                      sizeof(reg),
                                                      buffer,
                                                      sizeof(buffer),
                                                      pdMS_TO_TICKS(50));
  if (err != ESP_OK) {
    return false;
  }

  const int16_t raw = static_cast<int16_t>((buffer[0] << 8) | buffer[1]);
  int16_t shifted = raw >> 4;
  if ((shifted & 0x0800) != 0) {
    shifted |= 0xF000;
  }
  temperature_c = static_cast<float>(shifted) * 0.0625f;
  return true;
}

bool ping_tmp1075() {
  const uint8_t dummy = 0x00;
  return i2c_master_write_to_device(kI2cPort, kTmp1075Address, &dummy, 1, pdMS_TO_TICKS(25)) ==
         ESP_OK;
}

bool read_battery(float &battery_v) {
  int raw = 0;
  const esp_err_t err = adc_oneshot_read(g_adc_handle, g_adc_channel, &raw);
  if (err != ESP_OK) {
    return false;
  }
  battery_v = static_cast<float>(raw) * 0.00344001f + 0.24763f;
  return true;
}

}  // namespace

bool init() {
  if (g_initialized) {
    return true;
  }

  i2c_config_t i2c_config = {};
  i2c_config.mode = I2C_MODE_MASTER;
  i2c_config.sda_io_num = static_cast<gpio_num_t>(pinout::kI2cSdaPin);
  i2c_config.scl_io_num = static_cast<gpio_num_t>(pinout::kI2cSclPin);
  i2c_config.sda_pullup_en = GPIO_PULLUP_ENABLE;
  i2c_config.scl_pullup_en = GPIO_PULLUP_ENABLE;
  i2c_config.master.clk_speed = kI2cFreqHz;
  if (i2c_param_config(kI2cPort, &i2c_config) != ESP_OK) {
    DEV_LOGE(kTag, "i2c_param_config failed");
    return false;
  }
  if (i2c_driver_install(kI2cPort, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
    DEV_LOGE(kTag, "i2c_driver_install failed");
    return false;
  }

  adc_oneshot_unit_init_cfg_t adc_cfg = {};
  adc_cfg.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&adc_cfg, &g_adc_handle) != ESP_OK) {
    DEV_LOGE(kTag, "adc_oneshot_new_unit failed");
    return false;
  }

  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten = ADC_ATTEN_DB_12;
  chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_oneshot_config_channel(g_adc_handle, g_adc_channel, &chan_cfg) != ESP_OK) {
    DEV_LOGE(kTag, "adc_oneshot_config_channel failed");
    return false;
  }

  g_snapshot.temp_sensor_present = ping_tmp1075();
  if (g_snapshot.temp_sensor_present) {
    g_snapshot.temp_valid = read_tmp1075(g_snapshot.temp_c);
  }
  g_snapshot.battery_valid = read_battery(g_snapshot.battery_v);
  DEV_LOGI(kTag,
           "Sensor init: temp_present=%s temp_valid=%s temp=%.2fC battery_valid=%s battery=%.2fV",
           g_snapshot.temp_sensor_present ? "yes" : "no",
           g_snapshot.temp_valid ? "yes" : "no",
           g_snapshot.temp_c,
           g_snapshot.battery_valid ? "yes" : "no",
           g_snapshot.battery_v);
  g_initialized = true;
  return g_snapshot.temp_sensor_present && g_snapshot.temp_valid && g_snapshot.battery_valid;
}

bool initialized() {
  return g_initialized;
}

void update(uint32_t now_ms) {
  if (!g_initialized) {
    return;
  }

  if ((now_ms - g_last_temp_read_ms) >= kTempRefreshMs) {
    g_last_temp_read_ms = now_ms;
    g_snapshot.temp_sensor_present = ping_tmp1075();
    if (g_snapshot.temp_sensor_present) {
      g_snapshot.temp_valid = read_tmp1075(g_snapshot.temp_c);
    } else {
      g_snapshot.temp_valid = false;
    }
  }

  if ((now_ms - g_last_battery_read_ms) >= kBatteryRefreshMs) {
    g_last_battery_read_ms = now_ms;
    g_snapshot.battery_valid = read_battery(g_snapshot.battery_v);
  }
}

Snapshot snapshot() {
  return g_snapshot;
}

ControlFaultKind evaluate_fault(const config_store::Settings &settings) {
  if (!g_snapshot.temp_sensor_present || !g_snapshot.temp_valid || !g_snapshot.battery_valid) {
    return ControlFaultKind::SENSOR_FAILURE;
  }
  if (g_snapshot.temp_c >= settings.temp_fault_high_c) {
    return ControlFaultKind::OVERTEMPERATURE;
  }
  if (g_snapshot.battery_v <= settings.battery_fault_low_v) {
    return ControlFaultKind::BATTERY_UNDERVOLTAGE;
  }
  return ControlFaultKind::NONE;
}

}  // namespace sensor_monitor
