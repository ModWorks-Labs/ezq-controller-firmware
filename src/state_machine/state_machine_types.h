#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "config_store.h"
#include "control_runtime.h"
#include "sensor_monitor.h"

namespace state_machine {

enum class StateId {
  BOOT,
  READY_IDLE,
  BLOWER_MODE,
  IGNITION_CYCLE,
  ABORT_COOLDOWN,
  SLEEP_IDLE,
  FAULT,
};

enum class EventType {
  NONE,
  BUTTON_PRESSED,
  BUTTON_RELEASED,
  BUTTON_SHORT_PRESS,
  BUTTON_HOLD_STARTED,
  BUTTON_LONG_PRESS,
};

enum class AbortReason {
  NONE,
  SOFT_ABORT,
  FAULT_ABORT,
};

enum class IgnitionPhase {
  COUNTDOWN,
  ACTIVE,
  POST_CYCLE_HOLD,
};

struct Event {
  EventType type;
};

struct StateContext;

struct StateHandler {
  const char *name;
  void (*enter)(StateContext &context);
  void (*update)(StateContext &context);
  void (*exit)(StateContext &context);
};

class Machine {
 public:
  void init();
  void post_event(EventType type);
  void raise_fault(ControlFaultKind fault_kind);
  void tick(uint32_t now_ms,
            const config_store::Settings &settings,
            const config_store::IgnitionProfile &profile,
            const sensor_monitor::Snapshot &sensors);
  ControlRuntimeStatus runtime_status() const;

  StateId current_state = StateId::BOOT;
  StateId next_state = StateId::BOOT;
  bool transition_requested = false;
  uint32_t state_entry_ms = 0;
  uint32_t total_ms = 0;
  bool has_total_ms = false;
  ControlFaultKind fault_kind = ControlFaultKind::NONE;
  AbortReason abort_reason = AbortReason::NONE;
  IgnitionPhase ignition_phase = IgnitionPhase::COUNTDOWN;
  uint32_t phase_entry_ms = 0;
  uint8_t countdown_beeps_sent = 0;
  bool boot_attempted = false;
  bool ready_idle_arming = false;
  bool ready_idle_long_committed = false;
  std::array<Event, 16> events = {};
  std::size_t event_count = 0;
  ControlFaultKind pending_fault = ControlFaultKind::NONE;
  bool transition_time_override_valid = false;
  uint32_t transition_time_override_ms = 0;

  bool dequeue_event(Event &event);
  void request_transition(StateId state);
  void request_transition_at(StateId state, uint32_t transition_time_ms);

 private:
  void apply_fault_transition();
};

struct StateContext {
  Machine &machine;
  const config_store::Settings &settings;
  const config_store::IgnitionProfile &profile;
  const sensor_monitor::Snapshot &sensors;
  uint32_t now_ms;
};

const StateHandler &handler_for(StateId state);
const char *state_name(StateId state);
const char *fault_name(ControlFaultKind fault_kind);

}  // namespace state_machine
