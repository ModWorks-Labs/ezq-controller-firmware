#ifndef TEMP_H
#define TEMP_H

struct SensorReadings {
  bool tmp1075_ok;
  float temperature_c;
};

struct TempDebugStatus {
  bool tmp1075_present;
  bool temperature_valid;
  float temperature_c;
  bool fault_alert_ready;
  bool fault_line_low;
  unsigned int read_failures;
};

void setupSensors();
void updateSensors();
SensorReadings readSensors();
bool getCurrentTemperatureC(float &temperature_c);
bool getCurrentTemperatureF(float &temperature_f);
TempDebugStatus getTempDebugStatus();
bool applyTemperatureSafetyConfig();
void ignoreTempFaultForMs(unsigned long duration_ms);
bool tempFaultTriggered();

#endif
