#include <Arduino.h>

#include "config.h"
#include "cycle_manager.h"
#include "fan.h"
#include "glow_plug.h"
#include "settings_menu.h"
#include "ui.h"

namespace {

enum class CycleState {
  READY_IDLE,
  ARMING_STAGE_1,
  ARMING_STAGE_2,
  WAIT_FOR_RELEASE,
  COUNTDOWN,
  IGNITION_PROFILE,
  POST_CYCLE_HOLD,
  MANUAL_CONTROL,
  COOLDOWN,
  FAULT_LATCHED,
  SLEEP_PREP,
};

enum class CooldownReason {
  NONE,
  USER_ABORT,
  FAULT,
};

constexpr unsigned long kArmingFlashStartMs = 1500;
constexpr unsigned long kArmingAcceptMs = 3000;
constexpr unsigned long kCountdownTotalMs = 3000;
constexpr unsigned long kCountdownStepMs = 1000;
constexpr unsigned long kFaultCooldownDurationMs = 20000;
constexpr unsigned long kPostCycleHoldDurationMs = 180000;
constexpr float kPostCycleFanThrottlePercent = 50.0f;

CycleState g_state = CycleState::READY_IDLE;
CooldownReason g_cooldown_reason = CooldownReason::NONE;
unsigned long g_state_entry_ms = 0;
unsigned long g_button_press_start_ms = 0;
unsigned long g_cooldown_duration_ms = 0;
int g_countdown_beeps_sent = 0;
bool g_button_is_down = false;
bool g_fault_latched = false;
bool g_button_press_started_in_blower_only = false;
bool g_sleep_requested = false;

float clampPercent(float percent) {
  if (percent < 0.0f) {
    return 0.0f;
  }

  if (percent > 100.0f) {
    return 100.0f;
  }

  return percent;
}

bool blowerModeActive() {
  return g_state == CycleState::READY_IDLE && !g_fault_latched && !fanIsOff();
}

bool canStartFromIdle() {
  return g_state == CycleState::READY_IDLE && !g_fault_latched && fanIsOff();
}

bool canEnterManualControl() {
  return g_state == CycleState::READY_IDLE && !g_fault_latched && fanIsOff();
}

bool manualControlActive() {
  return g_state == CycleState::MANUAL_CONTROL;
}

void updateManualUi() {
  if (getGlowPlugPercent() > 0.0f) {
    setUiHeatingOrange();
  } else if (getFanThrottlePercent() > 0.0f) {
    setUiStokingBlue();
  } else {
    setUiReadyIdle();
  }
}

float findNextHoldThrottle(const config::IgnitionProfileSegment *segments,
                           unsigned int segment_count,
                           unsigned int start_index) {
  for (unsigned int i = start_index; i < segment_count; ++i) {
    if (!segments[i].is_ramp) {
      return clampPercent(segments[i].throttle_percent);
    }
  }

  return 0.0f;
}

float evaluateTimeline(const config::IgnitionProfileSegment *segments,
                       unsigned int segment_count,
                       unsigned long elapsed_ms) {
  float current_throttle = 0.0f;
  unsigned long elapsed_remaining_ms = elapsed_ms;

  for (unsigned int i = 0; i < segment_count; ++i) {
    const config::IgnitionProfileSegment &segment = segments[i];
    const unsigned long segment_duration_ms = segment.duration_ms;

    if (elapsed_remaining_ms < segment_duration_ms) {
      if (!segment.is_ramp) {
        return clampPercent(segment.throttle_percent);
      }

      const float start_throttle = current_throttle;
      const float end_throttle = findNextHoldThrottle(segments, segment_count, i + 1);
      if (segment_duration_ms == 0) {
        return end_throttle;
      }

      const float progress =
          static_cast<float>(elapsed_remaining_ms) / static_cast<float>(segment_duration_ms);
      return clampPercent(start_throttle + ((end_throttle - start_throttle) * progress));
    }

    elapsed_remaining_ms -= segment_duration_ms;
    if (segment.is_ramp) {
      current_throttle = findNextHoldThrottle(segments, segment_count, i + 1);
    } else {
      current_throttle = clampPercent(segment.throttle_percent);
    }
  }

  return 0.0f;
}

void applyIgnitionProfile(unsigned long elapsed_ms) {
  const config::IgnitionProfile &profile = config::getIgnitionProfile();
  setGlowPlugPercent(evaluateTimeline(profile.gp_segments, profile.gp_segment_count, elapsed_ms));
  setFanThrottlePercent(evaluateTimeline(profile.fan_segments, profile.fan_segment_count, elapsed_ms));
}

void enterState(CycleState new_state) {
  if (g_state == CycleState::POST_CYCLE_HOLD && new_state != CycleState::POST_CYCLE_HOLD) {
    stopPostCycleReminderBeep();
  }

  g_state = new_state;
  g_state_entry_ms = millis();

  switch (g_state) {
    case CycleState::READY_IDLE:
      g_sleep_requested = false;
      g_cooldown_reason = CooldownReason::NONE;
      g_cooldown_duration_ms = 0;
      turnGlowPlugOff();
      setFanOff();
      setUiReadyIdle();
      break;

    case CycleState::ARMING_STAGE_1:
      turnGlowPlugOff();
      setUiReadyIdle();
      break;

    case CycleState::ARMING_STAGE_2:
      turnGlowPlugOff();
      setUiArmingFlashYellow(false);
      break;

    case CycleState::WAIT_FOR_RELEASE:
      turnGlowPlugOff();
      setUiCycleReadyYellow();
      playCycleArmedBeeps();
      break;

    case CycleState::COUNTDOWN:
      config::reloadIgnitionProfile();
      turnGlowPlugOff();
      setUiCycleReadyYellow();
      g_countdown_beeps_sent = 0;
      playCountdownBeep();
      g_countdown_beeps_sent = 1;
      break;

    case CycleState::IGNITION_PROFILE:
      setUiHeatingOrange();
      playPhaseCompleteChirp();
      break;

    case CycleState::POST_CYCLE_HOLD:
      g_sleep_requested = false;
      turnGlowPlugOff();
      setFanThrottlePercent(kPostCycleFanThrottlePercent);
      setUiStokingBlue();
      playCycleCompleteJingle();
      startPostCycleReminderBeep();
      break;

    case CycleState::MANUAL_CONTROL:
      g_sleep_requested = false;
      updateManualUi();
      break;

    case CycleState::COOLDOWN:
      turnGlowPlugOff();
      setFanOn();
      if (g_cooldown_reason == CooldownReason::FAULT) {
        triggerUiFault();
      } else {
        setUiStokingBlue();
      }
      break;

    case CycleState::FAULT_LATCHED:
      g_fault_latched = true;
      turnGlowPlugOff();
      setFanOff();
      setUiFaultLatched();
      break;

    case CycleState::SLEEP_PREP:
      turnGlowPlugOff();
      setFanOff();
      setUiOff();
      playShutdownJingle();
      break;
  }
}

void enterBlowerOnlyMode(unsigned long now_ms) {
  g_state = CycleState::READY_IDLE;
  g_state_entry_ms = now_ms;
  g_cooldown_reason = CooldownReason::NONE;
  g_cooldown_duration_ms = 0;
  g_sleep_requested = false;
  turnGlowPlugOff();
  setFanOn();
  setUiForIdle(false, getFanThrottlePercent());
}

void startFaultCooldown() {
  g_cooldown_reason = CooldownReason::FAULT;
  g_cooldown_duration_ms = kFaultCooldownDurationMs;
  enterState(CycleState::COOLDOWN);
}

void startUserAbortCooling() {
  g_cooldown_reason = CooldownReason::USER_ABORT;
  g_cooldown_duration_ms = config::getSettings().abort_blower_duration_ms;
  enterState(CycleState::COOLDOWN);
}

void handleUserAbort() {
  if (g_state == CycleState::COUNTDOWN) {
    enterState(CycleState::READY_IDLE);
    return;
  }

  if (g_state != CycleState::IGNITION_PROFILE) {
    return;
  }

  if (glowPlugIsOn()) {
    startUserAbortCooling();
  } else {
    enterState(CycleState::READY_IDLE);
  }
}

}  // namespace

void setupCycleManager() {
  g_fault_latched = false;
  g_button_is_down = false;
  g_button_press_start_ms = 0;
  g_button_press_started_in_blower_only = false;
  g_sleep_requested = false;
  g_cooldown_duration_ms = 0;
  enterState(CycleState::READY_IDLE);
}

void updateCycleManager() {
  const unsigned long now = millis();
  const config::Settings &settings = config::getSettings();

  switch (g_state) {
    case CycleState::READY_IDLE:
      setUiForIdle(fanIsOff(), getFanThrottlePercent());
      if (fanIsOff() && !g_button_is_down &&
          !settingsMenuPreventsSleep() &&
          (now - g_state_entry_ms >= settings.idle_sleep_timeout_ms)) {
        enterState(CycleState::SLEEP_PREP);
      }
      break;

    case CycleState::ARMING_STAGE_1:
      if (g_button_is_down && (now - g_button_press_start_ms >= kArmingFlashStartMs)) {
        enterState(CycleState::ARMING_STAGE_2);
      }
      break;

    case CycleState::ARMING_STAGE_2:
      setUiArmingFlashYellow(((now / 250) % 2) == 0);
      if (g_button_is_down && (now - g_button_press_start_ms >= kArmingAcceptMs)) {
        enterState(CycleState::WAIT_FOR_RELEASE);
      }
      break;

    case CycleState::WAIT_FOR_RELEASE:
      break;

    case CycleState::COUNTDOWN: {
      const unsigned long elapsed_ms = now - g_state_entry_ms;

      if (g_countdown_beeps_sent == 1 && elapsed_ms >= kCountdownStepMs) {
        playCountdownBeep();
        g_countdown_beeps_sent = 2;
      } else if (g_countdown_beeps_sent == 2 && elapsed_ms >= (2 * kCountdownStepMs)) {
        playCountdownGoBeep();
        g_countdown_beeps_sent = 3;
      } else if (elapsed_ms >= kCountdownTotalMs) {
        enterState(CycleState::IGNITION_PROFILE);
      }
      break;
    }

    case CycleState::IGNITION_PROFILE: {
      const unsigned long ignition_elapsed_ms = now - g_state_entry_ms;
      const config::IgnitionProfile &profile = config::getIgnitionProfile();
      applyIgnitionProfile(ignition_elapsed_ms);

      if (getGlowPlugPercent() > 0.0f) {
        setUiHeatingOrange();
      } else if (getFanThrottlePercent() > 0.0f) {
        setUiStokingBlue();
      }

      if (ignition_elapsed_ms >= profile.total_duration_ms) {
        turnGlowPlugOff();
        enterState(CycleState::POST_CYCLE_HOLD);
      }
      break;
    }

    case CycleState::POST_CYCLE_HOLD:
      setFanThrottlePercent(kPostCycleFanThrottlePercent);
      setUiStokingBlue();
      if (now - g_state_entry_ms >= kPostCycleHoldDurationMs) {
        enterState(CycleState::READY_IDLE);
      }
      break;

    case CycleState::MANUAL_CONTROL:
      updateManualUi();
      break;

    case CycleState::COOLDOWN:
      if (now - g_state_entry_ms >= g_cooldown_duration_ms) {
        if (g_cooldown_reason == CooldownReason::FAULT) {
          enterState(CycleState::FAULT_LATCHED);
        } else {
          enterState(CycleState::READY_IDLE);
        }
      }
      break;

    case CycleState::FAULT_LATCHED:
      break;

    case CycleState::SLEEP_PREP:
      if (!uiIsBusy()) {
        g_sleep_requested = true;
      }
      break;
  }
}

void notifyButtonPressed() {
  notifyButtonPressedAt(millis());
}

void notifyButtonPressedAt(unsigned long press_started_ms) {
  if (g_fault_latched) {
    return;
  }

  g_button_is_down = true;
  g_button_press_start_ms = press_started_ms;
  g_button_press_started_in_blower_only = false;

  if (g_state == CycleState::READY_IDLE) {
    if (fanIsOff()) {
      enterState(CycleState::ARMING_STAGE_1);
    } else {
      g_button_press_started_in_blower_only = true;
    }
    return;
  }

  if (g_state == CycleState::COUNTDOWN || g_state == CycleState::IGNITION_PROFILE) {
    handleUserAbort();
    return;
  }

  if (g_state == CycleState::POST_CYCLE_HOLD) {
    enterState(CycleState::READY_IDLE);
    return;
  }

  if (g_state == CycleState::MANUAL_CONTROL) {
    enterState(CycleState::READY_IDLE);
  }
}

void notifyButtonReleased() {
  notifyButtonReleasedAt(millis());
}

void notifyButtonReleasedAt(unsigned long release_ms) {
  if (g_fault_latched) {
    return;
  }

  g_button_is_down = false;
  const unsigned long held_ms = release_ms - g_button_press_start_ms;

  if (g_button_press_started_in_blower_only) {
    g_button_press_started_in_blower_only = false;
    setFanOff();
    g_state_entry_ms = release_ms;
    return;
  }

  if (g_state == CycleState::ARMING_STAGE_1) {
    if (held_ms < kArmingFlashStartMs) {
      enterBlowerOnlyMode(release_ms);
      return;
    }

    enterState(CycleState::READY_IDLE);
    return;
  }

  if (g_state == CycleState::ARMING_STAGE_2) {
    enterState(CycleState::READY_IDLE);
    return;
  }

  if (g_state == CycleState::WAIT_FOR_RELEASE) {
    enterState(CycleState::COUNTDOWN);
  }
}

bool requestWebStartCycle() {
  if (!canStartFromIdle()) {
    return false;
  }

  g_button_is_down = false;
  g_button_press_started_in_blower_only = false;
  enterState(CycleState::COUNTDOWN);
  return true;
}

bool requestWebStartBlower() {
  if (!canStartFromIdle()) {
    return false;
  }

  g_button_is_down = false;
  g_button_press_started_in_blower_only = false;
  enterBlowerOnlyMode(millis());
  return true;
}

bool requestWebAbortCycle() {
  if (g_state == CycleState::POST_CYCLE_HOLD) {
    enterState(CycleState::READY_IDLE);
    return true;
  }

  if (g_state != CycleState::COUNTDOWN && g_state != CycleState::IGNITION_PROFILE) {
    return false;
  }

  handleUserAbort();
  return true;
}

bool requestWebStopBlower() {
  if (!blowerModeActive()) {
    return false;
  }

  setFanOff();
  g_state_entry_ms = millis();
  return true;
}

bool requestWebEnterManualControl() {
  if (!canEnterManualControl()) {
    return false;
  }

  g_button_is_down = false;
  g_button_press_started_in_blower_only = false;
  enterState(CycleState::MANUAL_CONTROL);
  return true;
}

bool requestWebSetManualOutputs(float glow_plug_percent, float fan_percent) {
  if (!manualControlActive()) {
    return false;
  }

  if (glow_plug_percent < 0.0f) {
    glow_plug_percent = 0.0f;
  } else if (glow_plug_percent > 100.0f) {
    glow_plug_percent = 100.0f;
  }

  if (fan_percent < 0.0f) {
    fan_percent = 0.0f;
  } else if (fan_percent > 100.0f) {
    fan_percent = 100.0f;
  }

  g_button_is_down = false;
  g_button_press_started_in_blower_only = false;

  setGlowPlugPercent(glow_plug_percent);
  setFanThrottlePercent(fan_percent);

  g_state_entry_ms = millis();
  updateManualUi();

  return true;
}

bool requestWebStopManualOutputs() {
  if (!manualControlActive()) {
    return false;
  }

  enterState(CycleState::READY_IDLE);
  return true;
}

CycleRuntimeStatus getCycleRuntimeStatus() {
  CycleRuntimeStatus status = {};
  const unsigned long now = millis();

  status.glow_plug_active = glowPlugIsOn();
  status.fan_active = !fanIsOff();
  status.can_start_cycle = canStartFromIdle();
  status.can_start_blower = canStartFromIdle();
  status.can_abort_cycle =
      g_state == CycleState::COUNTDOWN || g_state == CycleState::IGNITION_PROFILE ||
      g_state == CycleState::POST_CYCLE_HOLD;
  status.can_stop_blower = blowerModeActive();
  status.can_manual_control = canEnterManualControl();
  status.can_stop_manual_control = manualControlActive();
  status.elapsed_ms = 0;
  status.total_ms = 0;
  status.has_total_ms = false;
  status.is_fault = false;
  status.is_abort_cooling = false;

  if (blowerModeActive()) {
    status.mode = CycleRuntimeMode::BLOWER;
    status.mode_name = "blower";
    status.state_label = "Blower Mode";
    status.elapsed_ms = now - g_state_entry_ms;
    return status;
  }

  switch (g_state) {
    case CycleState::READY_IDLE:
      status.mode = CycleRuntimeMode::READY_IDLE;
      status.mode_name = "ready_idle";
      status.state_label = "Ready Idle";
      break;

    case CycleState::COUNTDOWN:
      status.mode = CycleRuntimeMode::COUNTDOWN;
      status.mode_name = "countdown";
      status.state_label = "Countdown";
      status.elapsed_ms = now - g_state_entry_ms;
      status.total_ms = kCountdownTotalMs;
      status.has_total_ms = true;
      break;

    case CycleState::IGNITION_PROFILE:
      status.mode = CycleRuntimeMode::IGNITION;
      status.mode_name = "ignition";
      status.state_label = "Ignition Cycle";
      status.elapsed_ms = now - g_state_entry_ms;
      status.total_ms = config::getIgnitionProfile().total_duration_ms;
      status.has_total_ms = true;
      break;

    case CycleState::POST_CYCLE_HOLD:
      status.mode = CycleRuntimeMode::POST_CYCLE_HOLD;
      status.mode_name = "post_cycle_hold";
      status.state_label = "Cycle Complete";
      status.elapsed_ms = now - g_state_entry_ms;
      status.total_ms = kPostCycleHoldDurationMs;
      status.has_total_ms = true;
      break;

    case CycleState::MANUAL_CONTROL:
      status.mode = CycleRuntimeMode::MANUAL_CONTROL;
      status.mode_name = "manual_control";
      status.state_label = "Quick Test";
      status.elapsed_ms = now - g_state_entry_ms;
      break;

    case CycleState::COOLDOWN:
      status.mode = CycleRuntimeMode::COOLDOWN;
      status.mode_name = "cooldown";
      status.state_label =
          g_cooldown_reason == CooldownReason::FAULT ? "Fault Cooldown" : "Abort Cooling";
      status.elapsed_ms = now - g_state_entry_ms;
      status.total_ms = g_cooldown_duration_ms;
      status.has_total_ms = true;
      status.is_fault = g_cooldown_reason == CooldownReason::FAULT;
      status.is_abort_cooling = g_cooldown_reason == CooldownReason::USER_ABORT;
      break;

    case CycleState::FAULT_LATCHED:
      status.mode = CycleRuntimeMode::FAULT_LATCHED;
      status.mode_name = "fault_latched";
      status.state_label = "Fault Latched";
      status.is_fault = true;
      break;

    case CycleState::SLEEP_PREP:
      status.mode = CycleRuntimeMode::SLEEP_PREP;
      status.mode_name = "sleep_prep";
      status.state_label = "Sleep Prep";
      break;

    case CycleState::ARMING_STAGE_1:
    case CycleState::ARMING_STAGE_2:
    case CycleState::WAIT_FOR_RELEASE:
      status.mode = CycleRuntimeMode::READY_IDLE;
      status.mode_name = "ready_idle";
      status.state_label = "Ready Idle";
      break;
  }

  return status;
}

void notifyTempFault() {
  if (g_fault_latched) {
    return;
  }

  startFaultCooldown();
}

bool cycleIsInFault() {
  return g_fault_latched || (g_state == CycleState::COOLDOWN &&
                             g_cooldown_reason == CooldownReason::FAULT);
}

bool cycleShouldEnterDeepSleep() {
  return g_sleep_requested;
}

bool cycleIsReadyIdle() {
  return !g_fault_latched && g_state == CycleState::READY_IDLE;
}

void notifyWakeFromSleep() {
  if (g_fault_latched) {
    return;
  }

  g_button_is_down = false;
  g_button_press_started_in_blower_only = false;
  g_sleep_requested = false;
  enterState(CycleState::READY_IDLE);
}
