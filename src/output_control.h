#pragma once

#include <cstdint>

namespace output_control {

enum class Indicator {
  OFF,
  BOOT,
  UPDATE_FW,
  READY_IDLE,
  BLOWER_MODE,
  ARMING,
  CYCLE_READY,
  IGNITION_ACTIVE,
  POST_CYCLE,
  COOLDOWN,
  FAULT,
};

bool init();
void tick(uint32_t now_ms);

void set_indicator(Indicator indicator);
Indicator indicator();

void set_fan_off();
void set_fan_percent(float percent);
float fan_percent();
bool fan_active();

void set_glow_off();
void set_glow_percent(float percent);
float glow_percent();
bool glow_active();

void play_startup_jingle();
void play_button_press();
void play_mode_entry_beep();
void play_mode_exit_beep();
void play_cycle_armed_beeps();
void play_countdown_beep();
void play_countdown_go_beep();
void play_phase_complete_chirp();
void play_cycle_complete_jingle();
void start_post_cycle_reminder();
void stop_post_cycle_reminder();
void play_fault_pattern();

void force_safe_outputs();
void prepare_for_light_sleep();
void restore_after_light_sleep();

}  // namespace output_control
