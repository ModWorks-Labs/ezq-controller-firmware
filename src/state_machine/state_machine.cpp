#include "state_machine/state_machine_types.h"

#include "debug_console.h"
#include "ota_service.h"
#include "output_control.h"
#include "state_machine/state_handlers.h"

namespace state_machine {
namespace {

Event make_event(EventType type) {
  return {type};
}

const StateHandler *kHandlers[] = {
    &kBootState,
    &kUpdateFwState,
    &kReadyIdleState,
    &kBlowerModeState,
    &kIgnitionCycleState,
    &kAbortCooldownState,
    &kSleepIdleState,
    &kFaultState,
};

const char *kStateNames[] = {
    "BOOT",
    "UPDATE_FW",
    "READY_IDLE",
    "BLOWER_MODE",
    "IGNITION_CYCLE",
    "ABORT_COOLDOWN",
    "SLEEP_IDLE",
    "FAULT",
};

}  // namespace

void Machine::init() {
  current_state = StateId::BOOT;
  next_state = StateId::BOOT;
  transition_requested = false;
  state_entry_ms = 0;
  total_ms = 0;
  has_total_ms = false;
  fault_kind = ControlFaultKind::NONE;
  abort_reason = AbortReason::NONE;
  ignition_phase = IgnitionPhase::COUNTDOWN;
  phase_entry_ms = 0;
  countdown_beeps_sent = 0;
  boot_attempted = false;
  update_attempted = false;
  boot_update_wait_started = false;
  boot_update_wait_start_ms = 0;
  ready_idle_arming = false;
  ready_idle_long_committed = false;
  ready_idle_console_connected = false;
  event_count = 0;
  pending_fault = ControlFaultKind::NONE;
  transition_time_override_valid = false;
  transition_time_override_ms = 0;
}

void Machine::post_event(EventType type) {
  if (event_count < events.size()) {
    events[event_count++] = make_event(type);
  }
}

void Machine::raise_fault(ControlFaultKind kind) {
  if (kind != ControlFaultKind::NONE && pending_fault == ControlFaultKind::NONE) {
    pending_fault = kind;
  }
}

bool Machine::dequeue_event(Event &event) {
  if (event_count == 0) {
    return false;
  }

  event = events[0];
  for (std::size_t i = 1; i < event_count; ++i) {
    events[i - 1] = events[i];
  }
  --event_count;
  return true;
}

void Machine::request_transition(StateId state) {
  next_state = state;
  transition_requested = true;
  transition_time_override_valid = false;
}

void Machine::request_transition_at(StateId state, uint32_t transition_time_ms) {
  next_state = state;
  transition_requested = true;
  transition_time_override_valid = true;
  transition_time_override_ms = transition_time_ms;
}

void Machine::apply_fault_transition() {
  if (pending_fault == ControlFaultKind::NONE || current_state == StateId::FAULT) {
    pending_fault = ControlFaultKind::NONE;
    return;
  }

  fault_kind = pending_fault;
  pending_fault = ControlFaultKind::NONE;

  if (current_state == StateId::IGNITION_CYCLE &&
      ignition_phase == IgnitionPhase::ACTIVE &&
      output_control::glow_active()) {
    abort_reason = AbortReason::FAULT_ABORT;
    request_transition(StateId::ABORT_COOLDOWN);
    return;
  }

  abort_reason = AbortReason::NONE;
  request_transition(StateId::FAULT);
}

void Machine::tick(uint32_t now_ms,
                   const config_store::Settings &settings,
                   const config_store::IgnitionProfile &profile,
                   const sensor_monitor::Snapshot &sensors) {
  StateContext context{*this, settings, profile, sensors, now_ms};
  if (state_entry_ms == 0) {
    state_entry_ms = now_ms;
    phase_entry_ms = now_ms;
    handler_for(current_state).enter(context);
  }

  apply_fault_transition();
  if (!transition_requested) {
    handler_for(current_state).update(context);
  }

  if (!transition_requested) {
    return;
  }

  handler_for(current_state).exit(context);
  current_state = next_state;
  const uint32_t transition_time_ms =
      transition_time_override_valid ? transition_time_override_ms : now_ms;
  state_entry_ms = transition_time_ms;
  total_ms = 0;
  has_total_ms = false;
  transition_requested = false;
  transition_time_override_valid = false;
  phase_entry_ms = transition_time_ms;
  handler_for(current_state).enter(context);
}

ControlRuntimeStatus Machine::runtime_status() const {
  ControlRuntimeStatus status = {};
  status.state = static_cast<ControlStateId>(current_state);
  status.fault_kind = fault_kind;
  status.state_name = state_name(current_state);
  status.detail_name = "";
  status.elapsed_ms = 0;
  status.total_ms = total_ms;
  status.has_total_ms = has_total_ms;
  status.can_start_cycle = current_state == StateId::READY_IDLE && !ready_idle_arming;
  status.can_start_blower = current_state == StateId::READY_IDLE && !ready_idle_arming;
  status.can_abort_cycle = current_state == StateId::IGNITION_CYCLE;
  status.can_sleep = current_state == StateId::READY_IDLE &&
                     !debug_console::client_connected() &&
                     !ota_service::maintenance_active();
  status.glow_plug_active = output_control::glow_active();
  status.fan_active = output_control::fan_active();
  status.fault_latched = current_state == StateId::FAULT;
  status.temp_valid = false;
  status.battery_valid = false;
  status.temp_c = 0.0f;
  status.battery_v = 0.0f;
  return status;
}

const StateHandler &handler_for(StateId state) {
  return *kHandlers[static_cast<int>(state)];
}

const char *state_name(StateId state) {
  return kStateNames[static_cast<int>(state)];
}

const char *fault_name(ControlFaultKind fault_kind) {
  switch (fault_kind) {
    case ControlFaultKind::NONE:
      return "none";
    case ControlFaultKind::INIT_FAILURE:
      return "init_failure";
    case ControlFaultKind::SENSOR_FAILURE:
      return "sensor_failure";
    case ControlFaultKind::OVERTEMPERATURE:
      return "overtemperature";
    case ControlFaultKind::BATTERY_UNDERVOLTAGE:
      return "battery_undervoltage";
    default:
      return "unknown";
  }
}

}  // namespace state_machine
