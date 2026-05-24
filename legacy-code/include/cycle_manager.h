#ifndef CYCLE_MANAGER_H
#define CYCLE_MANAGER_H

enum class CycleRuntimeMode {
  READY_IDLE,
  COUNTDOWN,
  IGNITION,
  BLOWER,
  POST_CYCLE_HOLD,
  MANUAL_CONTROL,
  COOLDOWN,
  FAULT_LATCHED,
  SLEEP_PREP,
};

struct CycleRuntimeStatus {
  CycleRuntimeMode mode;
  const char *mode_name;
  const char *state_label;
  unsigned long elapsed_ms;
  unsigned long total_ms;
  bool has_total_ms;
  bool can_start_cycle;
  bool can_start_blower;
  bool can_abort_cycle;
  bool can_stop_blower;
  bool can_manual_control;
  bool can_stop_manual_control;
  bool glow_plug_active;
  bool fan_active;
  bool is_fault;
  bool is_abort_cooling;
};

void setupCycleManager();
void updateCycleManager();
void notifyButtonPressed();
void notifyButtonReleased();
void notifyButtonPressedAt(unsigned long press_started_ms);
void notifyButtonReleasedAt(unsigned long release_ms);
bool requestWebStartCycle();
bool requestWebStartBlower();
bool requestWebAbortCycle();
bool requestWebStopBlower();
bool requestWebEnterManualControl();
bool requestWebSetManualOutputs(float glow_plug_percent, float fan_percent);
bool requestWebStopManualOutputs();
CycleRuntimeStatus getCycleRuntimeStatus();
void notifyTempFault();
void notifyWakeFromSleep();
bool cycleIsInFault();
bool cycleShouldEnterDeepSleep();
bool cycleIsReadyIdle();

#endif
