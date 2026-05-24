#include <Arduino.h>
#include <esp32-hal-rgb-led.h>

#include "config.h"
#include "pinout.h"
#include "ui.h"

namespace {

constexpr int kBuzzerChannel = 0;
constexpr int kBuzzerResolution = 10;
constexpr uint8_t kLedBrightness = 32;
constexpr int kMaxToneSteps = 16;

enum class LedMode {
  OFF,
  READY_GREEN,
  IDLE_FAN_BLUE,
  ARMING_YELLOW_FLASH,
  CYCLE_READY_YELLOW,
  HEATING_ORANGE,
  STOKING_BLUE,
  FAULT_RED,
};

struct ToneStep {
  unsigned int frequency_hz;
  unsigned int duration_ms;
  unsigned int gap_ms;
};

LedMode g_led_mode = LedMode::OFF;
bool g_led_flash_on = false;
bool g_fault_active = false;
float g_idle_fan_throttle_percent = 0.0f;

ToneStep g_tone_steps[kMaxToneSteps] = {};
int g_tone_step_count = 0;
int g_tone_step_index = 0;
bool g_tone_playing = false;
bool g_tone_in_gap = false;
unsigned long g_tone_deadline_ms = 0;
bool g_reminder_beep_enabled = false;
bool g_reminder_beep_playing = false;
unsigned long g_reminder_deadline_ms = 0;
unsigned long g_reminder_gap_ms = 0;

void setLedColor(uint8_t red, uint8_t green, uint8_t blue) {
  neopixelWrite(pinout::kLedPwmPin, red, green, blue);
}

void buzzerOff() {
  ledcWrite(kBuzzerChannel, 0);
}

bool soundPlaybackEnabled() {
  return config::getSettings().sound_volume > 0;
}

unsigned int getSoundVolumeLevelClamped() {
  unsigned int volume = config::getSettings().sound_volume;
  return volume > 4 ? 4 : volume;
}

unsigned int getBuzzerDutyForConfiguredVolume() {
  return ui_config::kBuzzerDutyByVolume[getSoundVolumeLevelClamped()];
}

void startGap(unsigned int gap_ms) {
  buzzerOff();
  g_tone_playing = false;
  g_tone_in_gap = true;
  g_tone_deadline_ms = millis() + gap_ms;
}

void startTone(const ToneStep &step) {
  ledcWriteTone(kBuzzerChannel, step.frequency_hz);
  ledcWrite(kBuzzerChannel, getBuzzerDutyForConfiguredVolume());
  g_tone_playing = true;
  g_tone_in_gap = false;
  g_tone_deadline_ms = millis() + step.duration_ms;
}

void clearToneSequence() {
  buzzerOff();
  g_tone_step_count = 0;
  g_tone_step_index = 0;
  g_tone_playing = false;
  g_tone_in_gap = false;
  g_tone_deadline_ms = 0;
}

void stopReminderBeepInternal() {
  g_reminder_beep_enabled = false;
  g_reminder_beep_playing = false;
  g_reminder_deadline_ms = 0;
  g_reminder_gap_ms = 0;
}

void queueToneSequence(const ToneStep *steps, int count) {
  if (!soundPlaybackEnabled()) {
    clearToneSequence();
    return;
  }

  if (count > kMaxToneSteps) {
    count = kMaxToneSteps;
  }

  for (int i = 0; i < count; ++i) {
    g_tone_steps[i] = steps[i];
  }

  g_tone_step_count = count;
  g_tone_step_index = 0;
  g_tone_playing = false;
  g_tone_in_gap = false;
  g_tone_deadline_ms = 0;
}

void updateToneSequence() {
  unsigned long now = millis();

  if (g_tone_step_count == 0) {
    return;
  }

  if (!g_tone_playing && !g_tone_in_gap) {
    startTone(g_tone_steps[g_tone_step_index]);
    return;
  }

  if (now < g_tone_deadline_ms) {
    return;
  }

  if (g_tone_playing) {
    unsigned int gap_ms = g_tone_steps[g_tone_step_index].gap_ms;
    if (gap_ms > 0) {
      startGap(gap_ms);
      return;
    }

    ++g_tone_step_index;
    g_tone_playing = false;
    if (g_tone_step_index >= g_tone_step_count) {
      clearToneSequence();
    }
    return;
  }

  if (g_tone_in_gap) {
    ++g_tone_step_index;
    g_tone_in_gap = false;
    if (g_tone_step_index >= g_tone_step_count) {
      clearToneSequence();
    }
  }
}

void updateReminderBeep() {
  if (!g_reminder_beep_enabled || !soundPlaybackEnabled() || g_tone_step_count > 0 ||
      g_tone_playing || g_tone_in_gap) {
    if (g_reminder_beep_playing) {
      buzzerOff();
      g_reminder_beep_playing = false;
    }
    return;
  }

  const unsigned long now = millis();
  if (!g_reminder_beep_playing) {
    if (now < g_reminder_deadline_ms) {
      return;
    }

    ledcWriteTone(kBuzzerChannel, ui_config::kBuzzerNoteMidHz);
    ledcWrite(kBuzzerChannel, getBuzzerDutyForConfiguredVolume());
    g_reminder_beep_playing = true;
    g_reminder_deadline_ms = now + 450;
    return;
  }

  if (now < g_reminder_deadline_ms) {
    return;
  }

  buzzerOff();
  g_reminder_beep_playing = false;
  g_reminder_deadline_ms = now + g_reminder_gap_ms;
}

void renderLed() {
  switch (g_led_mode) {
    case LedMode::OFF:
      setLedColor(0, 0, 0);
      break;

    case LedMode::READY_GREEN:
      setLedColor(0, kLedBrightness, 0);
      break;

    case LedMode::IDLE_FAN_BLUE: {
      if (g_idle_fan_throttle_percent <= 0.0f) {
        setLedColor(0, kLedBrightness, 0);
        break;
      }

      float throttle_percent = g_idle_fan_throttle_percent;
      if (throttle_percent > 95.0f) {
        throttle_percent = 95.0f;
      }

      unsigned long blink_period_ms = 900;
      blink_period_ms -= static_cast<unsigned long>(throttle_percent * 7.0f);
      if (blink_period_ms < 180) {
        blink_period_ms = 180;
      }

      unsigned long phase_ms = millis() % blink_period_ms;
      bool led_on = phase_ms < (blink_period_ms / 2);
      setLedColor(0, 0, led_on ? kLedBrightness : 0);
      break;
    }

    case LedMode::ARMING_YELLOW_FLASH:
      setLedColor(g_led_flash_on ? kLedBrightness : 0,
                  g_led_flash_on ? kLedBrightness : 0,
                  0);
      break;

    case LedMode::CYCLE_READY_YELLOW:
      setLedColor(kLedBrightness, kLedBrightness, 0);
      break;

    case LedMode::HEATING_ORANGE:
      setLedColor(kLedBrightness, 12, 0);
      break;

    case LedMode::STOKING_BLUE:
      setLedColor(0, 0, kLedBrightness);
      break;

    case LedMode::FAULT_RED:
      setLedColor(kLedBrightness, 0, 0);
      break;
  }
}

}

void setupUi() {
  ledcSetup(kBuzzerChannel, ui_config::kBuzzerFrequencyHz, kBuzzerResolution);
  ledcAttachPin(pinout::kBuzzerPin, kBuzzerChannel);
  clearToneSequence();
  g_led_mode = LedMode::OFF;
  renderLed();
}

void updateUi() {
  updateToneSequence();
  updateReminderBeep();
  renderLed();
}

bool uiIsBusy() {
  return g_tone_step_count > 0 || g_tone_playing || g_tone_in_gap;
}

void playStartupIndication() {
  static const ToneStep kStartupTones[] = {
      {ui_config::kBuzzerNoteMidHz, 95, 28},
      {ui_config::kBuzzerNoteHighHz, 95, 28},
      {ui_config::kBuzzerNoteBrightHz, 170, 0},
  };

  if (g_fault_active) {
    return;
  }

  setUiReadyIdle();
  queueToneSequence(kStartupTones, 3);
}

void playWakeIndication() {
  static const ToneStep kWakeTones[] = {
      {ui_config::kBuzzerNoteMidHz, 45, 30},
      {ui_config::kBuzzerNoteHighHz, 70, 0},
  };

  if (g_fault_active) {
    return;
  }

  setUiReadyIdle();
  queueToneSequence(kWakeTones, 2);
}

void playButtonPressTone() {
  static const ToneStep kButtonTone[] = {
      {ui_config::kBuzzerFrequencyHz, 35, 0},
  };

  if (g_fault_active || !config::getSettings().button_press_beep_enabled) {
    return;
  }

  queueToneSequence(kButtonTone, 1);
}

void playShutdownJingle() {
  static const ToneStep kShutdownTones[] = {
      {ui_config::kBuzzerNoteHighHz, 85, 35},
      {ui_config::kBuzzerNoteMidHz, 85, 35},
      {ui_config::kBuzzerNoteLowHz, 130, 0},
  };

  if (g_fault_active) {
    return;
  }

  queueToneSequence(kShutdownTones, 3);
}

void setUiReadyIdle() {
  if (g_fault_active) {
    return;
  }

  g_led_mode = LedMode::READY_GREEN;
}

void setUiForIdle(bool fan_off, float fan_throttle_percent) {
  if (g_fault_active) {
    return;
  }

  if (fan_off) {
    g_led_mode = LedMode::READY_GREEN;
  } else {
    g_led_mode = LedMode::IDLE_FAN_BLUE;
    g_idle_fan_throttle_percent = fan_throttle_percent;
  }
}

void setUiArmingFlashYellow(bool on_phase) {
  if (g_fault_active) {
    return;
  }

  g_led_mode = LedMode::ARMING_YELLOW_FLASH;
  g_led_flash_on = on_phase;
}

void setUiCycleReadyYellow() {
  if (g_fault_active) {
    return;
  }

  g_led_mode = LedMode::CYCLE_READY_YELLOW;
}

void setUiHeatingOrange() {
  if (g_fault_active) {
    return;
  }

  g_led_mode = LedMode::HEATING_ORANGE;
}

void setUiStokingBlue() {
  if (g_fault_active) {
    return;
  }

  g_led_mode = LedMode::STOKING_BLUE;
}

void setUiFaultLatched() {
  g_fault_active = true;
  g_led_mode = LedMode::FAULT_RED;
}

void setUiOff() {
  if (g_fault_active) {
    return;
  }

  g_led_mode = LedMode::OFF;
}

void playCycleArmedBeeps() {
  static const ToneStep kArmedBeeps[] = {
      {ui_config::kBuzzerNoteMidHz, 65, 50},
      {ui_config::kBuzzerNoteHighHz, 65, 50},
      {ui_config::kBuzzerNoteBrightHz, 85, 0},
  };

  if (g_fault_active) {
    return;
  }

  queueToneSequence(kArmedBeeps, 3);
}

void playCountdownBeep() {
  static const ToneStep kCountdownTone[] = {
      {ui_config::kBuzzerNoteMidHz, 80, 0},
  };

  if (g_fault_active) {
    return;
  }

  queueToneSequence(kCountdownTone, 1);
}

void playCountdownGoBeep() {
  static const ToneStep kGoTone[] = {
      {ui_config::kBuzzerNoteBrightHz, 120, 0},
  };

  if (g_fault_active) {
    return;
  }

  queueToneSequence(kGoTone, 1);
}

void playPhaseCompleteChirp() {
  static const ToneStep kChirp[] = {
      {ui_config::kBuzzerNoteMidHz, 75, 25},
      {ui_config::kBuzzerNoteHighHz, 105, 0},
  };

  if (g_fault_active) {
    return;
  }

  queueToneSequence(kChirp, 2);
}

void playCycleCompleteJingle() {
  static const ToneStep kCompleteJingle[] = {
      {ui_config::kBuzzerNoteLowHz, 100, 35},
      {ui_config::kBuzzerNoteMidHz, 120, 35},
      {ui_config::kBuzzerNoteHighHz, 150, 40},
      {ui_config::kBuzzerNoteBrightHz, 220, 0},
  };

  if (g_fault_active) {
    return;
  }

  queueToneSequence(kCompleteJingle, 4);
}

void startPostCycleReminderBeep() {
  if (g_fault_active) {
    return;
  }

  g_reminder_beep_enabled = true;
  g_reminder_beep_playing = false;
  g_reminder_gap_ms = 15000;
  g_reminder_deadline_ms = millis() + 15000;
}

void stopPostCycleReminderBeep() {
  stopReminderBeepInternal();
  if (g_tone_step_count == 0 && !g_tone_playing && !g_tone_in_gap) {
    buzzerOff();
  }
}

void triggerUiFault() {
  static const ToneStep kFaultJingle[] = {
      {ui_config::kBuzzerNoteLowHz, 260, 90},
      {ui_config::kBuzzerNoteFaultHz, 260, 90},
      {ui_config::kBuzzerNoteLowHz, 260, 100},
      {ui_config::kBuzzerNoteFaultHz, 360, 0},
  };

  if (g_fault_active) {
    return;
  }

  g_fault_active = true;
  g_led_mode = LedMode::FAULT_RED;
  queueToneSequence(kFaultJingle, 4);
}

bool uiFaultActive() {
  return g_fault_active;
}
