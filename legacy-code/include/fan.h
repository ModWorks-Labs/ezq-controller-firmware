#ifndef FAN_H
#define FAN_H

void setupFan();
void updateFan();
void setFanOff();
void setFanThrottlePercent(float throttle_percent);
void setFanOn();
float getFanThrottlePercent();
float getFanTargetThrottlePercent();
bool fanIsOff();

#endif
