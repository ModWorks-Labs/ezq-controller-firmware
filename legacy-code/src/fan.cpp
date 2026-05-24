#include <Arduino.h>

#include "fan.h"
#include "pinout.h"

namespace {

constexpr int kFanChannel = 4;
#ifndef FAN_PWM_FREQUENCY_HZ
#define FAN_PWM_FREQUENCY_HZ 20000
#endif
constexpr int kFanFrequencyHz = FAN_PWM_FREQUENCY_HZ;
constexpr int kFanResolutionBits = 8;
constexpr int kFanMaxDuty = 255;
#ifndef FAN_WAKE_PULSE_MS
#define FAN_WAKE_PULSE_MS 10
#endif
constexpr unsigned long kFanWakePulseMs = FAN_WAKE_PULSE_MS;
constexpr float kFanMaxThrottlePercent = 100.0f;

float g_current_throttle_percent = 0.0f;
float g_target_throttle_percent = 0.0f;
bool g_pwm_attached = false;

struct FanThrottleMapPoint {
  float commanded_percent;
  float effective_percent;
};

// Isolation-test characterization showed:
// - 5% is the minimum stable running point, so keep it unchanged.
// - The ESC appears to saturate before a true 5.0 V equivalent input, likely
//   around 4.7-4.8 V, so the scripted 100% point intentionally lands below a
//   literal 100% analog-equivalent command while still representing user-facing
//   100% / full output.
// - Above roughly 20% command, current climbed by about 0.6 A per extra 10% duty
//   until the system hit the current ceiling near the top end.
// This table redistributes user-facing command percentages into more even current
// increments while preserving the 5% floor and adding top-end margin.
constexpr FanThrottleMapPoint kFanThrottleMap[] = {
    {0.0f, 0.0f},
    {5.0f, 5.0f},
    {10.0f, 18.4f},
    {20.0f, 26.2f},
    {30.0f, 33.4f},
    {40.0f, 40.6f},
    {50.0f, 47.7f},
    {60.0f, 55.9f},
    {70.0f, 62.4f},
    {80.0f, 68.4f},
    {90.0f, 74.8f},
    {100.0f, 96.0f},
};

float mapThrottlePercent(float commanded_percent) {
  if (commanded_percent <= kFanThrottleMap[0].commanded_percent) {
    return kFanThrottleMap[0].effective_percent;
  }

  const size_t point_count = sizeof(kFanThrottleMap) / sizeof(kFanThrottleMap[0]);
  for (size_t i = 1; i < point_count; ++i) {
    const FanThrottleMapPoint &prev = kFanThrottleMap[i - 1];
    const FanThrottleMapPoint &next = kFanThrottleMap[i];
    if (commanded_percent > next.commanded_percent) {
      continue;
    }

    const float segment_span = next.commanded_percent - prev.commanded_percent;
    if (segment_span <= 0.0f) {
      return next.effective_percent;
    }

    const float segment_progress = (commanded_percent - prev.commanded_percent) / segment_span;
    return prev.effective_percent +
           ((next.effective_percent - prev.effective_percent) * segment_progress);
  }

  return kFanThrottleMap[point_count - 1].effective_percent;
}

int throttlePercentToDuty(float throttle_percent) {
  if (throttle_percent <= 0.0f) {
    return 0;
  }

  if (throttle_percent > kFanMaxThrottlePercent) {
    throttle_percent = kFanMaxThrottlePercent;
  }

  throttle_percent = mapThrottlePercent(throttle_percent);
  return static_cast<int>((throttle_percent / 100.0f) * kFanMaxDuty + 0.5f);
}

void writeFanDuty(float throttle_percent) {
  ledcWrite(kFanChannel, throttlePercentToDuty(throttle_percent));
}

void attachFanPwmIfNeeded() {
  if (g_pwm_attached) {
    return;
  }

  ledcAttachPin(pinout::kBlowerFanDrivePwmPin, kFanChannel);
  g_pwm_attached = true;
}

void detachFanPwmIfNeeded() {
  if (!g_pwm_attached) {
    return;
  }

  ledcDetachPin(pinout::kBlowerFanDrivePwmPin);
  g_pwm_attached = false;
}

void driveFanPinLow() {
  detachFanPwmIfNeeded();
  pinMode(pinout::kBlowerFanDrivePwmPin, OUTPUT);
  digitalWrite(pinout::kBlowerFanDrivePwmPin, LOW);
}

}

void setupFan() {
  g_current_throttle_percent = 0.0f;
  g_target_throttle_percent = 0.0f;
  g_pwm_attached = false;

  pinMode(pinout::kBlowerFanDrivePwmPin, OUTPUT);
  digitalWrite(pinout::kBlowerFanDrivePwmPin, LOW);
  ledcSetup(kFanChannel, kFanFrequencyHz, kFanResolutionBits);
  writeFanDuty(0.0f);
  driveFanPinLow();
}

void updateFan() {
  if (g_current_throttle_percent <= 0.0f) {
    writeFanDuty(0.0f);
    driveFanPinLow();
    return;
  }

  attachFanPwmIfNeeded();
  writeFanDuty(g_current_throttle_percent);
}

void setFanOff() {
  setFanThrottlePercent(0.0f);
}

void setFanOn() {
  setFanThrottlePercent(kFanMaxThrottlePercent);
}

void setFanThrottlePercent(float throttle_percent) {
  if (throttle_percent <= 0.0f) {
    throttle_percent = 0.0f;
  } else {
    if (throttle_percent > kFanMaxThrottlePercent) {
      throttle_percent = kFanMaxThrottlePercent;
    }
  }

  const float requested_throttle_percent = throttle_percent;
  const bool waking_fan = (g_current_throttle_percent <= 0.0f && throttle_percent > 0.0f);
  const float delta = requested_throttle_percent - g_current_throttle_percent;
  if (!waking_fan && delta > -0.05f && delta < 0.05f) {
    g_target_throttle_percent = requested_throttle_percent;
    return;
  }

  if (waking_fan && kFanWakePulseMs > 0) {
    attachFanPwmIfNeeded();
    ledcWrite(kFanChannel, kFanMaxDuty);
    delay(kFanWakePulseMs);
  }

  g_current_throttle_percent = throttle_percent;
  g_target_throttle_percent = throttle_percent;
  updateFan();
}

float getFanThrottlePercent() {
  return g_current_throttle_percent;
}

float getFanTargetThrottlePercent() {
  return g_target_throttle_percent;
}

bool fanIsOff() {
  return g_target_throttle_percent <= 0.0f;
}
