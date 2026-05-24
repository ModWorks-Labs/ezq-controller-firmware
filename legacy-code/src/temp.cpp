#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "pinout.h"
#include "temp.h"

namespace {

constexpr uint8_t kTmp1075Address = 0x48;
constexpr uint8_t kTmp1075TempReg = 0x00;
constexpr uint8_t kTmp1075ConfigReg = 0x01;
constexpr uint8_t kTmp1075LowLimitReg = 0x02;
constexpr uint8_t kTmp1075HighLimitReg = 0x03;
constexpr uint8_t kTmp1075ConfigByte = 0x08;
constexpr unsigned long kTmp1075FaultIgnoreAfterInitMs = 5000;
constexpr unsigned long kTemperatureRefreshMs = 500;
constexpr unsigned long kTemperatureRediscoveryMs = 3000;
bool g_tmp1075_present = false;
bool g_temperature_valid = false;
float g_temperature_cached_c = 0.0f;
unsigned long g_temperature_last_refresh_ms = 0;
unsigned int g_temperature_read_failures = 0;
volatile bool g_tmp1075_fault_triggered = false;
bool g_tmp1075_fault_alert_ready = false;
unsigned long g_tmp1075_fault_enable_ms = 0;

void IRAM_ATTR onTempFaultInterrupt() {
  g_tmp1075_fault_triggered = true;
}

bool pingAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool writeRegister16(uint8_t address, uint8_t reg, uint16_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value >> 8));
  Wire.write(static_cast<uint8_t>(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool readRegisters(uint8_t address, uint8_t start_reg, uint8_t *buffer, size_t length) {
  Wire.beginTransmission(address);
  Wire.write(start_reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  size_t received = Wire.requestFrom(static_cast<int>(address), static_cast<int>(length));
  if (received != length) {
    return false;
  }

  for (size_t i = 0; i < length; ++i) {
    buffer[i] = Wire.read();
  }

  return true;
}

bool readRegisterWithStop(uint8_t address, uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) {
    return false;
  }

  size_t received = Wire.requestFrom(static_cast<int>(address), 1);
  if (received != 1) {
    return false;
  }

  value = Wire.read();
  return true;
}

bool readTmp1075Celsius(float &temperature_c) {
  uint8_t buffer[2];
  if (!readRegisters(kTmp1075Address, kTmp1075TempReg, buffer, sizeof(buffer))) {
    Wire.beginTransmission(kTmp1075Address);
    Wire.write(kTmp1075TempReg);
    if (Wire.endTransmission(true) != 0) {
      return false;
    }

    delayMicroseconds(60);
    size_t received =
        Wire.requestFrom(static_cast<int>(kTmp1075Address), static_cast<int>(sizeof(buffer)));
    if (received != sizeof(buffer)) {
      return false;
    }

    for (size_t i = 0; i < sizeof(buffer); ++i) {
      buffer[i] = Wire.read();
    }
  }

  if (!g_tmp1075_present) {
    g_tmp1075_present = true;
  }

  if (buffer[0] == 0xFF && buffer[1] == 0xFF) {
    return false;
  }

  int16_t raw = static_cast<int16_t>((buffer[0] << 8) | buffer[1]);
  raw >>= 4;

  if (raw & 0x0800) {
    raw |= 0xF000;
  }

  temperature_c = raw * 0.0625f;
  return true;
}

bool refreshTemperatureCache(bool force) {
  const unsigned long now = millis();
  if (!force && (now - g_temperature_last_refresh_ms) < kTemperatureRefreshMs) {
    return g_temperature_valid;
  }

  g_temperature_last_refresh_ms = now;
  float temperature_c = 0.0f;
  if (!readTmp1075Celsius(temperature_c)) {
    ++g_temperature_read_failures;
    if (g_temperature_read_failures >= 3) {
      g_tmp1075_present = pingAddress(kTmp1075Address);
      if (!g_tmp1075_present) {
        g_temperature_valid = false;
      }
      g_temperature_read_failures = 0;
    }
    return g_temperature_valid;
  }

  g_temperature_read_failures = 0;
  g_temperature_cached_c = temperature_c;
  g_temperature_valid = true;
  return true;
}

uint16_t encodeTmp1075LimitRegister(float temperature_c) {
  int16_t raw = static_cast<int16_t>(temperature_c / 0.0625f);
  return static_cast<uint16_t>(raw << 4);
}

bool configureTmp1075FaultAlert() {
  pinMode(pinout::kTemperatureIntPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pinout::kTemperatureIntPin), onTempFaultInterrupt, FALLING);

  if (!writeRegister16(kTmp1075Address,
                       kTmp1075LowLimitReg,
                       encodeTmp1075LimitRegister(config::getSettings().temp_fault_low_c))) {
    return false;
  }

  if (!writeRegister16(kTmp1075Address,
                       kTmp1075HighLimitReg,
                       encodeTmp1075LimitRegister(config::getSettings().temp_fault_high_c))) {
    return false;
  }

  if (!writeRegister(kTmp1075Address, kTmp1075ConfigReg, kTmp1075ConfigByte)) {
    return false;
  }

  g_tmp1075_fault_triggered = false;
  g_tmp1075_fault_alert_ready = true;
  g_tmp1075_fault_enable_ms = millis() + kTmp1075FaultIgnoreAfterInitMs;
  return true;
}

}

void setupSensors() {
  Wire.begin(pinout::kI2cSdaPin, pinout::kI2cSclPin);
  Wire.setClock(100000);
  delay(50);

  g_tmp1075_present = false;
  g_temperature_valid = false;
  g_temperature_last_refresh_ms = 0;
  g_temperature_read_failures = 0;

  g_tmp1075_present = pingAddress(kTmp1075Address);
  if (g_tmp1075_present) {
    applyTemperatureSafetyConfig();
    refreshTemperatureCache(true);
  }
}

void updateSensors() {
  if (!g_tmp1075_present) {
    const unsigned long now = millis();
    if ((now - g_temperature_last_refresh_ms) >= kTemperatureRediscoveryMs) {
      g_temperature_last_refresh_ms = now;
      g_tmp1075_present = pingAddress(kTmp1075Address);
      if (g_tmp1075_present) {
        applyTemperatureSafetyConfig();
        refreshTemperatureCache(true);
      }
    }
    return;
  }

  refreshTemperatureCache(false);
}

SensorReadings readSensors() {
  SensorReadings readings = {};

  updateSensors();
  readings.tmp1075_ok = g_temperature_valid;
  readings.temperature_c = g_temperature_cached_c;

  return readings;
}

bool getCurrentTemperatureC(float &temperature_c) {
  if (!refreshTemperatureCache(false)) {
    refreshTemperatureCache(true);
  }

  if (!g_temperature_valid) {
    return false;
  }

  temperature_c = g_temperature_cached_c;
  return true;
}

bool getCurrentTemperatureF(float &temperature_f) {
  float temperature_c = 0.0f;
  if (!getCurrentTemperatureC(temperature_c)) {
    return false;
  }

  temperature_f = (temperature_c * 9.0f / 5.0f) + 32.0f;
  return true;
}

TempDebugStatus getTempDebugStatus() {
  TempDebugStatus status = {};
  status.tmp1075_present = g_tmp1075_present;
  status.temperature_valid = g_temperature_valid;
  status.temperature_c = g_temperature_cached_c;
  status.fault_alert_ready = g_tmp1075_fault_alert_ready;
  status.fault_line_low = digitalRead(pinout::kTemperatureIntPin) == LOW;
  status.read_failures = g_temperature_read_failures;
  return status;
}

bool applyTemperatureSafetyConfig() {
  g_tmp1075_present = pingAddress(kTmp1075Address);
  if (!g_tmp1075_present) {
    g_tmp1075_fault_alert_ready = false;
    g_temperature_valid = false;
    return false;
  }

  return configureTmp1075FaultAlert();
}

void ignoreTempFaultForMs(unsigned long duration_ms) {
  g_tmp1075_fault_triggered = false;
  g_tmp1075_fault_enable_ms = millis() + duration_ms;
}

bool tempFaultTriggered() {
  if (!g_tmp1075_fault_alert_ready) {
    return false;
  }

  if (millis() < g_tmp1075_fault_enable_ms) {
    return false;
  }

  return g_tmp1075_fault_triggered || (digitalRead(pinout::kTemperatureIntPin) == LOW);
}
