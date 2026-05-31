#include "state_machine/state_handlers.h"

#include <algorithm>

#include "output_control.h"

namespace state_machine {
namespace {

float clamp_percent(float value) {
  return std::clamp(value, 0.0f, 100.0f);
}

float find_next_hold_throttle(const std::array<config_store::IgnitionProfileSegment,
                                               config_store::kMaxIgnitionProfileSegments> &segments,
                              std::size_t segment_count,
                              std::size_t start_index) {
  for (std::size_t i = start_index; i < segment_count; ++i) {
    if (!segments[i].is_ramp) {
      return clamp_percent(segments[i].throttle_percent);
    }
  }
  return 0.0f;
}

float evaluate_timeline(const std::array<config_store::IgnitionProfileSegment,
                                         config_store::kMaxIgnitionProfileSegments> &segments,
                        std::size_t segment_count,
                        uint32_t elapsed_ms) {
  float current_throttle = 0.0f;
  uint32_t remaining = elapsed_ms;

  for (std::size_t i = 0; i < segment_count; ++i) {
    const auto &segment = segments[i];
    if (remaining < segment.duration_ms) {
      if (!segment.is_ramp) {
        return clamp_percent(segment.throttle_percent);
      }

      const float start = current_throttle;
      const float end = find_next_hold_throttle(segments, segment_count, i + 1);
      if (segment.duration_ms == 0) {
        return end;
      }
      const float progress =
          static_cast<float>(remaining) / static_cast<float>(segment.duration_ms);
      return clamp_percent(start + ((end - start) * progress));
    }

    remaining -= segment.duration_ms;
    current_throttle = segment.is_ramp
                           ? find_next_hold_throttle(segments, segment_count, i + 1)
                           : clamp_percent(segment.throttle_percent);
  }

  return 0.0f;
}

void apply_profile(const config_store::IgnitionProfile &profile, uint32_t elapsed_ms) {
  output_control::set_glow_percent(
      evaluate_timeline(profile.gp_segments, profile.gp_segment_count, elapsed_ms));
  output_control::set_fan_percent(
      evaluate_timeline(profile.fan_segments, profile.fan_segment_count, elapsed_ms));
}

void start_post_cycle(StateContext &context) {
  context.machine.ignition_phase = IgnitionPhase::POST_CYCLE_HOLD;
  context.machine.phase_entry_ms = context.now_ms;
  context.machine.has_total_ms = context.settings.fan_post_cycle_hold_enabled;
  context.machine.total_ms = context.settings.fan_post_cycle_hold_duration_ms;
  output_control::set_indicator(output_control::Indicator::POST_CYCLE);
  output_control::set_glow_off();
  if (context.settings.fan_post_cycle_hold_enabled) {
    output_control::set_fan_percent(context.settings.fan_post_cycle_hold_throttle_percent);
  } else {
    output_control::set_fan_off();
  }
  output_control::play_cycle_complete_jingle();
  output_control::start_post_cycle_reminder();
}

void enter(StateContext &context) {
  context.machine.abort_reason = AbortReason::NONE;
  context.machine.ignition_phase = IgnitionPhase::COUNTDOWN;
  context.machine.phase_entry_ms = context.now_ms;
  context.machine.countdown_beeps_sent = 0;
  context.machine.has_total_ms = true;
  context.machine.total_ms = context.settings.countdown_total_ms;
  output_control::set_indicator(output_control::Indicator::CYCLE_READY);
  output_control::play_cycle_armed_beeps();
  output_control::set_glow_off();
  output_control::set_fan_off();
}

void update_countdown(StateContext &context) {
  Event event = {};
  while (context.machine.dequeue_event(event)) {
    if (event.type == EventType::BUTTON_PRESSED) {
      context.machine.request_transition(StateId::READY_IDLE);
      return;
    }
  }

  const uint32_t elapsed_ms = context.now_ms - context.machine.phase_entry_ms;
  if (context.machine.countdown_beeps_sent == 0) {
    output_control::play_countdown_beep();
    context.machine.countdown_beeps_sent = 1;
  } else if (context.machine.countdown_beeps_sent == 1 &&
             elapsed_ms >= context.settings.countdown_step_ms) {
    output_control::play_countdown_beep();
    context.machine.countdown_beeps_sent = 2;
  } else if (context.machine.countdown_beeps_sent == 2 &&
             elapsed_ms >= (2 * context.settings.countdown_step_ms)) {
    output_control::play_countdown_go_beep();
    context.machine.countdown_beeps_sent = 3;
  }

  if (elapsed_ms >= context.settings.countdown_total_ms) {
    context.machine.ignition_phase = IgnitionPhase::ACTIVE;
    context.machine.phase_entry_ms = context.now_ms;
    context.machine.has_total_ms = true;
    context.machine.total_ms = context.profile.total_duration_ms;
    output_control::set_indicator(output_control::Indicator::IGNITION_ACTIVE);
    output_control::play_phase_complete_chirp();
  }
}

void update_active(StateContext &context) {
  Event event = {};
  while (context.machine.dequeue_event(event)) {
    if (event.type == EventType::BUTTON_PRESSED && output_control::glow_active()) {
      context.machine.abort_reason = AbortReason::SOFT_ABORT;
      context.machine.request_transition(StateId::ABORT_COOLDOWN);
      return;
    }
  }

  const uint32_t elapsed_ms = context.now_ms - context.machine.phase_entry_ms;
  apply_profile(context.profile, elapsed_ms);
  if (elapsed_ms >= context.profile.total_duration_ms) {
    start_post_cycle(context);
  }
}

void update_post_cycle(StateContext &context) {
  Event event = {};
  while (context.machine.dequeue_event(event)) {
    if (event.type == EventType::BUTTON_PRESSED) {
      context.machine.request_transition(StateId::READY_IDLE);
      return;
    }
  }

  if (!context.settings.fan_post_cycle_hold_enabled ||
      (context.now_ms - context.machine.phase_entry_ms) >=
          context.settings.fan_post_cycle_hold_duration_ms) {
    context.machine.request_transition(StateId::READY_IDLE);
  }
}

void update(StateContext &context) {
  switch (context.machine.ignition_phase) {
    case IgnitionPhase::COUNTDOWN:
      update_countdown(context);
      break;
    case IgnitionPhase::ACTIVE:
      update_active(context);
      break;
    case IgnitionPhase::POST_CYCLE_HOLD:
      update_post_cycle(context);
      break;
  }
}

void exit(StateContext &context) {
  output_control::stop_post_cycle_reminder();
  output_control::set_glow_off();
  if (context.machine.next_state != StateId::ABORT_COOLDOWN) {
    output_control::set_fan_off();
  }
}

}  // namespace

const StateHandler kIgnitionCycleState = {
    "IGNITION_CYCLE",
    enter,
    update,
    exit,
};

}  // namespace state_machine
