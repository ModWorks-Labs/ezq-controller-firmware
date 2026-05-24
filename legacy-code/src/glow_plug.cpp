#include <Arduino.h>

#include "config.h"
#include "glow_plug.h"
#include "pinout.h"

namespace {

constexpr int kGlowPlugChannel = 2;
constexpr int kGlowPlugFrequencyHz = 20000;
constexpr int kGlowPlugResolutionBits = 8;
constexpr int kGlowPlugMaxDuty = 255;

float g_glow_plug_percent = 0.0f;

void writeGlowPlugDuty(int duty) {
  if (duty < 0) {
    duty = 0;
  }

  if (duty > kGlowPlugMaxDuty) {
    duty = kGlowPlugMaxDuty;
  }

  ledcWrite(kGlowPlugChannel, duty);
}

}

void setupGlowPlug() {
  pinMode(pinout::kGlowPlugDrivePin, OUTPUT);
  digitalWrite(pinout::kGlowPlugDrivePin, LOW);
  ledcSetup(kGlowPlugChannel, kGlowPlugFrequencyHz, kGlowPlugResolutionBits);
  ledcAttachPin(pinout::kGlowPlugDrivePin, kGlowPlugChannel);
  turnGlowPlugOff();
}

void updateGlowPlug() {
  int duty = 0;
  if (g_glow_plug_percent > 0.0f) {
    float clamped_percent = g_glow_plug_percent;
    if (clamped_percent > 100.0f) {
      clamped_percent = 100.0f;
    }
    duty = static_cast<int>((clamped_percent / 100.0f) * kGlowPlugMaxDuty + 0.5f);
  }

  writeGlowPlugDuty(duty);
}

void turnGlowPlugOn() {
  setGlowPlugPercent(100.0f);
}

void turnGlowPlugOff() {
  setGlowPlugPercent(0.0f);
}

void setGlowPlugPercent(float throttle_percent) {
  if (throttle_percent <= 0.0f) {
    g_glow_plug_percent = 0.0f;
  } else if (throttle_percent >= 100.0f) {
    g_glow_plug_percent = 100.0f;
  } else {
    g_glow_plug_percent = throttle_percent;
  }

  updateGlowPlug();
}

float getGlowPlugPercent() {
  return g_glow_plug_percent;
}

bool glowPlugIsOn() {
  return g_glow_plug_percent > 0.0f;
}
