#include "state_machine/state_handlers.h"

#include "debug_console.h"
#include "output_control.h"

namespace state_machine {
namespace {

constexpr char kTag[] = "state_fault";

void enter(StateContext &context) {
  DEV_LOGE(kTag, "FAULT enter: %s", fault_name(context.machine.fault_kind));
  output_control::force_safe_outputs();
  output_control::set_indicator(output_control::Indicator::FAULT);
  output_control::play_fault_pattern();
}

void update(StateContext &context) {
  if (context.settings.fault_latch_enabled) {
    return;
  }

  Event event = {};
  while (context.machine.dequeue_event(event)) {
    if (event.type == EventType::BUTTON_PRESSED || event.type == EventType::BUTTON_SHORT_PRESS ||
        event.type == EventType::BUTTON_LONG_PRESS) {
      context.machine.fault_kind = ControlFaultKind::NONE;
      context.machine.request_transition(StateId::READY_IDLE);
      return;
    }
  }
}

void exit(StateContext &) {}

}  // namespace

const StateHandler kFaultState = {
    "FAULT",
    enter,
    update,
    exit,
};

}  // namespace state_machine
