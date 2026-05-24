#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#include "config.h"
#include "cycle_manager.h"
#include "fan.h"
#include "file_manager.h"
#include "glow_plug.h"
#include "pinout.h"
#include "serial_log.h"
#include "settings_menu.h"
#include "temp.h"
#include "ui.h"

namespace {

constexpr unsigned long kButtonDebounceMs = 50;
constexpr unsigned long kTempFaultIgnoreAfterWakeMs = 10000;
constexpr unsigned long kButtonIgnoreAfterWakeMs = 250;
constexpr unsigned long kSettingsEntryClickGapMs = 450;
constexpr unsigned long kSettingsEntryHoldForwardMs = 600;
constexpr unsigned int kSettingsEntryClickCount = 5;

bool g_button_raw_pressed = false;
bool g_button_stable_pressed = false;
bool g_sleeping = false;
bool g_require_post_wake_release = false;
unsigned long g_ignore_button_until_ms = 0;
unsigned long g_button_last_raw_change_ms = 0;

struct PendingReadyIdleClick {
  unsigned long press_started_ms;
  unsigned long release_ms;
};

PendingReadyIdleClick g_pending_ready_idle_clicks[kSettingsEntryClickCount] = {};
unsigned int g_pending_ready_idle_click_count = 0;
bool g_ready_idle_press_tracking = false;
bool g_ready_idle_press_forwarded = false;
unsigned long g_ready_idle_press_started_ms = 0;
unsigned long g_ready_idle_last_release_ms = 0;

bool settingsEntryEligible() {
  return cycleIsReadyIdle() && fanIsOff() && !settingsMenuIsActive();
}

void clearPendingReadyIdleClicks() {
  g_pending_ready_idle_click_count = 0;
  g_ready_idle_last_release_ms = 0;
}

void clearReadyIdlePressTracking() {
  g_ready_idle_press_tracking = false;
  g_ready_idle_press_forwarded = false;
  g_ready_idle_press_started_ms = 0;
}

void resetButtonRoutingState() {
  clearPendingReadyIdleClicks();
  clearReadyIdlePressTracking();
}

void replayPendingReadyIdleClicks() {
  for (unsigned int i = 0; i < g_pending_ready_idle_click_count; ++i) {
    notifyButtonPressedAt(g_pending_ready_idle_clicks[i].press_started_ms);
    notifyButtonReleasedAt(g_pending_ready_idle_clicks[i].release_ms);
  }

  clearPendingReadyIdleClicks();
}

void enterSettingsMenuFromReadyIdle() {
  clearPendingReadyIdleClicks();
  clearReadyIdlePressTracking();
  enterSettingsMenu();
}

void handleDebouncedButtonPress(unsigned long press_started_ms) {
  DebugSerial.print("Button press detected at ");
  DebugSerial.println(press_started_ms);

  if (settingsMenuIsActive()) {
    notifySettingsMenuButtonPressed(press_started_ms);
    return;
  }

  playButtonPressTone();

  if (settingsEntryEligible()) {
    if (g_pending_ready_idle_click_count > 0 &&
        press_started_ms - g_ready_idle_last_release_ms > kSettingsEntryClickGapMs) {
      replayPendingReadyIdleClicks();
    }

    g_ready_idle_press_tracking = true;
    g_ready_idle_press_forwarded = false;
    g_ready_idle_press_started_ms = press_started_ms;
    return;
  }

  notifyButtonPressedAt(press_started_ms);
}

void handleDebouncedButtonRelease(unsigned long release_ms) {
  DebugSerial.print("Button release detected at ");
  DebugSerial.println(release_ms);

  if (settingsMenuIsActive()) {
    notifySettingsMenuButtonReleased(release_ms);
    return;
  }

  if (g_ready_idle_press_tracking) {
    g_ready_idle_press_tracking = false;

    if (g_ready_idle_press_forwarded) {
      g_ready_idle_press_forwarded = false;
      notifyButtonReleasedAt(release_ms);
      return;
    }

    if (g_pending_ready_idle_click_count < kSettingsEntryClickCount) {
      g_pending_ready_idle_clicks[g_pending_ready_idle_click_count++] = {
          g_ready_idle_press_started_ms,
          release_ms,
      };
      g_ready_idle_last_release_ms = release_ms;
    } else {
      replayPendingReadyIdleClicks();
    }

    if (g_pending_ready_idle_click_count >= kSettingsEntryClickCount) {
      enterSettingsMenuFromReadyIdle();
    }

    return;
  }

  notifyButtonReleasedAt(release_ms);
}

void updateReadyIdleButtonInterception(unsigned long now_ms) {
  if (!settingsEntryEligible()) {
    if (!g_ready_idle_press_tracking) {
      clearPendingReadyIdleClicks();
    }
    return;
  }

  if (g_ready_idle_press_tracking &&
      !g_ready_idle_press_forwarded &&
      now_ms - g_ready_idle_press_started_ms >= kSettingsEntryHoldForwardMs) {
    replayPendingReadyIdleClicks();
    notifyButtonPressedAt(g_ready_idle_press_started_ms);
    g_ready_idle_press_forwarded = true;
  }

  if (!g_ready_idle_press_tracking &&
      g_pending_ready_idle_click_count > 0 &&
      now_ms - g_ready_idle_last_release_ms >= kSettingsEntryClickGapMs) {
    replayPendingReadyIdleClicks();
  }
}

void restoreButtonPinAfterWake() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  gpio_wakeup_disable(static_cast<gpio_num_t>(pinout::kButtonInputPin));
  gpio_sleep_sel_dis(static_cast<gpio_num_t>(pinout::kButtonInputPin));
  gpio_reset_pin(static_cast<gpio_num_t>(pinout::kButtonInputPin));
  pinMode(pinout::kButtonInputPin, INPUT_PULLUP);
  gpio_set_direction(static_cast<gpio_num_t>(pinout::kButtonInputPin), GPIO_MODE_INPUT);
  gpio_set_pull_mode(static_cast<gpio_num_t>(pinout::kButtonInputPin), GPIO_PULLUP_ONLY);
}

void resetButtonDebounceState(unsigned long now_ms) {
  g_button_raw_pressed = false;
  g_button_stable_pressed = false;
  g_button_last_raw_change_ms = now_ms;
}

void enterLightSleep() {
  resetButtonRoutingState();
  turnGlowPlugOff();
  setFanOff();
  updateFan();
  updateGlowPlug();
  setUiOff();
  updateUi();
  g_sleeping = true;

  gpio_wakeup_enable(static_cast<gpio_num_t>(pinout::kButtonInputPin), GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();

  g_sleeping = false;
  restoreButtonPinAfterWake();
  notifyWakeFromSleep();
  ignoreTempFaultForMs(kTempFaultIgnoreAfterWakeMs);
  resetButtonRoutingState();

  const unsigned long wake_ms = millis();
  resetButtonDebounceState(wake_ms);
  g_require_post_wake_release = true;
  g_ignore_button_until_ms = wake_ms + kButtonIgnoreAfterWakeMs;

  setUiReadyIdle();
  playWakeIndication();
  updateUi();
}

}

void setup() {
  delay(250);
  DebugSerial.begin(115200);

  pinMode(pinout::kButtonInputPin, INPUT_PULLUP);
  const bool initial_button_pressed = digitalRead(pinout::kButtonInputPin) == LOW;
  g_button_raw_pressed = initial_button_pressed;
  g_button_stable_pressed = initial_button_pressed;
  g_button_last_raw_change_ms = millis();

  setupFileManager();
  config::setupConfig();
  setupUi();
  setupSettingsMenu();
  setupFan();
  setupGlowPlug();
  setupSensors();
  setupCycleManager();
  playStartupIndication();
}

void loop() {
  static bool temp_fault_notified = false;
  static unsigned long last_ready_idle_sensor_debug_ms = 0;
  unsigned long now_ms = millis();

  DebugSerial.update();

  if (g_sleeping) {
    return;
  }

  const bool button_pressed = digitalRead(pinout::kButtonInputPin) == LOW;
  if (button_pressed != g_button_raw_pressed) {
    g_button_raw_pressed = button_pressed;
    g_button_last_raw_change_ms = now_ms;
  }

  if (g_require_post_wake_release) {
    if (now_ms >= g_ignore_button_until_ms && !button_pressed) {
      g_require_post_wake_release = false;
      resetButtonDebounceState(now_ms);
    } else {
      resetButtonRoutingState();
    }
  } else if (now_ms >= g_ignore_button_until_ms) {
    if (g_button_raw_pressed != g_button_stable_pressed &&
        now_ms - g_button_last_raw_change_ms >= kButtonDebounceMs) {
      g_button_stable_pressed = g_button_raw_pressed;
      if (g_button_stable_pressed) {
        handleDebouncedButtonPress(now_ms);
      } else {
        handleDebouncedButtonRelease(now_ms);
      }
    }
  } else {
    resetButtonRoutingState();
    resetButtonDebounceState(now_ms);
  }

  updateReadyIdleButtonInterception(now_ms);

  if (!temp_fault_notified && tempFaultTriggered()) {
    temp_fault_notified = true;
    notifyTempFault();
  }

  updateSettingsMenu();

  if (!settingsMenuBlocksCycleManager()) {
    updateCycleManager();
  }

  updateSensors();
  updateFan();
  updateGlowPlug();
  if (!settingsMenuOwnsUi()) {
    updateUi();
  }

  if (cycleIsReadyIdle()) {
    if (now_ms - last_ready_idle_sensor_debug_ms >= 1000) {
      last_ready_idle_sensor_debug_ms = now_ms;
      const TempDebugStatus temp_status = getTempDebugStatus();
      DebugSerial.print("READY_IDLE temp: present=");
      DebugSerial.print(temp_status.tmp1075_present ? "yes" : "no");
      DebugSerial.print(", valid=");
      DebugSerial.print(temp_status.temperature_valid ? "yes" : "no");
      DebugSerial.print(", temp_c=");
      if (temp_status.temperature_valid) {
        DebugSerial.print(temp_status.temperature_c, 2);
      } else {
        DebugSerial.print("n/a");
      }
      DebugSerial.print(", fault_ready=");
      DebugSerial.print(temp_status.fault_alert_ready ? "yes" : "no");
      DebugSerial.print(", int_low=");
      DebugSerial.print(temp_status.fault_line_low ? "yes" : "no");
      DebugSerial.print(", read_failures=");
      DebugSerial.println(temp_status.read_failures);
    }
  } else {
    last_ready_idle_sensor_debug_ms = now_ms;
  }

  if (settingsMenuAllowsSleep() && cycleShouldEnterDeepSleep()) {
    temp_fault_notified = false;
    enterLightSleep();
  }
}
