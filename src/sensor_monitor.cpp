#include "sensor_monitor.h"

#include <cmath>
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
constexpr uint8_t kTmp1075ConfigReg = 0x01;
constexpr uint8_t kTmp1075LowLimitReg = 0x02;
constexpr uint8_t kTmp1075HighLimitReg = 0x03;
constexpr uint8_t kTmp1075ConfigComparatorMode = 0x68;  // 220 ms conversion, 2-fault queue, active-low comparator, continuous conversion
constexpr uint32_t kI2cFreqHz = 100000;
constexpr uint32_t kTempRefreshMs = 500;
constexpr uint32_t kBatteryRefreshMs = 500;
constexpr uint8_t kBatteryFaultDebounceSamples = 3;

bool g_initialized = false;
Snapshot g_snapshot = {false, false, 0.0f, false, 0.0f};
uint32_t g_last_temp_read_ms = 0;
uint32_t g_last_battery_read_ms = 0;
adc_oneshot_unit_handle_t g_adc_handle = nullptr;
adc_channel_t g_adc_channel = ADC_CHANNEL_0;
bool g_temp_alert_ready = false;
volatile bool g_temp_alert_active = false;
bool g_gpio_isr_service_installed = false;
float g_programmed_temp_low_c = 0.0f;
float g_programmed_temp_high_c = 0.0f;
uint8_t g_battery_invalid_sample_count = 0;
uint8_t g_battery_undervoltage_sample_count = 0;
bool g_battery_sensor_fault_debounced = false;
bool g_battery_undervoltage_debounced = false;

bool nearly_equal(float lhs, float rhs) {
  return std::fabs(lhs - rhs) < 0.01f;
}

uint16_t encode_tmp1075_limit(float temperature_c) {
  const float clamped = std::fmax(-128.0f, std::fmin(127.9375f, temperature_c));
  const int raw = static_cast<int>(std::lround(clamped * 16.0f));
  return static_cast<uint16_t>(raw & 0x0FFF) << 4;
}

bool write_tmp1075_config(uint8_t config_value) {
  const uint8_t payload[] = {kTmp1075ConfigReg, config_value};
  return i2c_master_write_to_device(
             kI2cPort, kTmp1075Address, payload, sizeof(payload), pdMS_TO_TICKS(50)) == ESP_OK;
}

bool write_tmp1075_limit(uint8_t reg, float temperature_c) {
  const uint16_t encoded = encode_tmp1075_limit(temperature_c);
  const uint8_t payload[] = {
      reg,
      static_cast<uint8_t>((encoded >> 8) & 0xFF),
      static_cast<uint8_t>(encoded & 0xFF),
  };
  return i2c_master_write_to_device(
             kI2cPort, kTmp1075Address, payload, sizeof(payload), pdMS_TO_TICKS(50)) == ESP_OK;
}

void IRAM_ATTR temp_alert_isr_handler(void *) {
  g_temp_alert_active =
      gpio_get_level(static_cast<gpio_num_t>(pinout::kTemperatureIntPin)) == 0;
}

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

bool program_tmp1075_alert_thresholds(const config_store::Settings &settings) {
  if (!write_tmp1075_config(kTmp1075ConfigComparatorMode)) {
    DEV_LOGW(kTag, "Failed writing TMP1075 config register");
    return false;
  }
  if (!write_tmp1075_limit(kTmp1075LowLimitReg, settings.temp_fault_low_c)) {
    DEV_LOGW(kTag, "Failed writing TMP1075 TLOW register");
    return false;
  }
  if (!write_tmp1075_limit(kTmp1075HighLimitReg, settings.temp_fault_high_c)) {
    DEV_LOGW(kTag, "Failed writing TMP1075 THIGH register");
    return false;
  }

  g_programmed_temp_low_c = settings.temp_fault_low_c;
  g_programmed_temp_high_c = settings.temp_fault_high_c;
  g_temp_alert_ready = true;
  g_temp_alert_active = gpio_get_level(static_cast<gpio_num_t>(pinout::kTemperatureIntPin)) == 0;
  DEV_LOGI(kTag,
           "TMP1075 alert configured: TLOW=%.2fC THIGH=%.2fC mode=comparator queue=2 active_low=yes",
           static_cast<double>(g_programmed_temp_low_c),
           static_cast<double>(g_programmed_temp_high_c));
  return true;
}

bool ensure_tmp1075_alert_programmed(const config_store::Settings &settings) {
  if (!g_snapshot.temp_sensor_present) {
    g_temp_alert_ready = false;
    g_temp_alert_active = false;
    return false;
  }

  if (g_temp_alert_ready &&
      nearly_equal(g_programmed_temp_low_c, settings.temp_fault_low_c) &&
      nearly_equal(g_programmed_temp_high_c, settings.temp_fault_high_c)) {
    return true;
  }

  const bool programmed = program_tmp1075_alert_thresholds(settings);
  if (!programmed) {
    g_temp_alert_ready = false;
  }
  return programmed;
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

void update_battery_fault_debounce(const config_store::Settings &settings) {
  const bool battery_valid = g_snapshot.battery_valid;
  const bool undervoltage = battery_valid && g_snapshot.battery_v <= settings.battery_fault_low_v;

  if (!battery_valid) {
    if (g_battery_invalid_sample_count < kBatteryFaultDebounceSamples) {
      ++g_battery_invalid_sample_count;
    }
    g_battery_undervoltage_sample_count = 0;
  } else if (undervoltage) {
    g_battery_invalid_sample_count = 0;
    if (g_battery_undervoltage_sample_count < kBatteryFaultDebounceSamples) {
      ++g_battery_undervoltage_sample_count;
    }
  } else {
    g_battery_invalid_sample_count = 0;
    g_battery_undervoltage_sample_count = 0;
  }

  const bool sensor_fault_debounced =
      g_battery_invalid_sample_count >= kBatteryFaultDebounceSamples;
  const bool undervoltage_debounced =
      g_battery_undervoltage_sample_count >= kBatteryFaultDebounceSamples;

  if (sensor_fault_debounced && !g_battery_sensor_fault_debounced) {
    DEV_LOGW(kTag,
             "Battery ADC invalid for %u consecutive samples; fault armed",
             static_cast<unsigned>(kBatteryFaultDebounceSamples));
  } else if (!sensor_fault_debounced && g_battery_sensor_fault_debounced) {
    DEV_LOGI(kTag, "Battery ADC recovered before fault condition persisted");
  }

  if (undervoltage_debounced && !g_battery_undervoltage_debounced) {
    DEV_LOGW(kTag,
             "Battery undervoltage persisted for %u consecutive samples: %.2fV <= %.2fV",
             static_cast<unsigned>(kBatteryFaultDebounceSamples),
             static_cast<double>(g_snapshot.battery_v),
             static_cast<double>(settings.battery_fault_low_v));
  } else if (!undervoltage_debounced && g_battery_undervoltage_debounced) {
    DEV_LOGI(kTag, "Battery voltage recovered above fault threshold");
  }

  g_battery_sensor_fault_debounced = sensor_fault_debounced;
  g_battery_undervoltage_debounced = undervoltage_debounced;
}

}  // namespace

bool init(const config_store::Settings &settings) {
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

  gpio_config_t temp_int_gpio = {};
  temp_int_gpio.pin_bit_mask = 1ULL << pinout::kTemperatureIntPin;
  temp_int_gpio.mode = GPIO_MODE_INPUT;
  temp_int_gpio.pull_up_en = GPIO_PULLUP_DISABLE;
  temp_int_gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
  temp_int_gpio.intr_type = GPIO_INTR_ANYEDGE;
  if (gpio_config(&temp_int_gpio) != ESP_OK) {
    DEV_LOGE(kTag, "gpio_config(temp_int) failed");
    return false;
  }
  if (!g_gpio_isr_service_installed) {
    const esp_err_t isr_service_err = gpio_install_isr_service(0);
    if (isr_service_err != ESP_OK && isr_service_err != ESP_ERR_INVALID_STATE) {
      DEV_LOGE(kTag, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_service_err));
      return false;
    }
    g_gpio_isr_service_installed = true;
  }
  gpio_isr_handler_remove(static_cast<gpio_num_t>(pinout::kTemperatureIntPin));
  const esp_err_t isr_err = gpio_isr_handler_add(
      static_cast<gpio_num_t>(pinout::kTemperatureIntPin), temp_alert_isr_handler, nullptr);
  if (isr_err != ESP_OK) {
    DEV_LOGE(kTag, "gpio_isr_handler_add(temp_int) failed: %s", esp_err_to_name(isr_err));
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
    ensure_tmp1075_alert_programmed(settings);
  }
  g_snapshot.battery_valid = read_battery(g_snapshot.battery_v);
  update_battery_fault_debounce(settings);
  DEV_LOGI(kTag,
           "Sensor init: temp_present=%s temp_valid=%s temp=%.2fC temp_alert=%s battery_valid=%s battery=%.2fV",
           g_snapshot.temp_sensor_present ? "yes" : "no",
           g_snapshot.temp_valid ? "yes" : "no",
           g_snapshot.temp_c,
           g_temp_alert_active ? "active" : "idle",
           g_snapshot.battery_valid ? "yes" : "no",
           g_snapshot.battery_v);
  g_initialized = true;
  return g_snapshot.temp_sensor_present && g_snapshot.temp_valid && g_temp_alert_ready &&
         g_snapshot.battery_valid;
}

bool initialized() {
  return g_initialized;
}

void update(uint32_t now_ms, const config_store::Settings &settings) {
  if (!g_initialized) {
    return;
  }

  g_temp_alert_active = gpio_get_level(static_cast<gpio_num_t>(pinout::kTemperatureIntPin)) == 0;

  if ((now_ms - g_last_temp_read_ms) >= kTempRefreshMs) {
    g_last_temp_read_ms = now_ms;
    g_snapshot.temp_sensor_present = ping_tmp1075();
    if (g_snapshot.temp_sensor_present) {
      g_snapshot.temp_valid = read_tmp1075(g_snapshot.temp_c);
      ensure_tmp1075_alert_programmed(settings);
    } else {
      g_snapshot.temp_valid = false;
      g_temp_alert_ready = false;
      g_temp_alert_active = false;
    }
  }

  if ((now_ms - g_last_battery_read_ms) >= kBatteryRefreshMs) {
    g_last_battery_read_ms = now_ms;
    g_snapshot.battery_valid = read_battery(g_snapshot.battery_v);
    update_battery_fault_debounce(settings);
  }
}

Snapshot snapshot() {
  return g_snapshot;
}

ControlFaultKind evaluate_fault(const config_store::Settings &settings) {
  if (g_battery_sensor_fault_debounced) {
    return ControlFaultKind::SENSOR_FAILURE;
  }
  if (settings.sensor_fault_detection_enabled &&
      (!g_snapshot.temp_sensor_present || !g_snapshot.temp_valid || !g_temp_alert_ready)) {
    return ControlFaultKind::SENSOR_FAILURE;
  }
  if (settings.sensor_fault_detection_enabled && g_temp_alert_active) {
    return ControlFaultKind::OVERTEMPERATURE;
  }
  if (g_battery_undervoltage_debounced) {
    return ControlFaultKind::BATTERY_UNDERVOLTAGE;
  }
  return ControlFaultKind::NONE;
}

}  // namespace sensor_monitor
