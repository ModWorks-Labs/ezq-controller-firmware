#include "state_machine/state_handlers.h"

#include "output_control.h"

namespace state_machine {
namespace {

void enter(StateContext &context) {
  context.machine.has_total_ms = true;
  context.machine.total_ms = context.settings.abort_blower_duration_ms;
  output_control::set_indicator(output_control::Indicator::COOLDOWN);
  output_control::set_glow_off();
  output_control::set_fan_percent(100.0f);
}

void update(StateContext &context) {
  const uint32_t elapsed_ms = context.now_ms - context.machine.state_entry_ms;
  if (elapsed_ms < context.settings.abort_blower_duration_ms) {
    return;
  }

  if (context.machine.abort_reason == AbortReason::FAULT_ABORT) {
    context.machine.request_transition(StateId::FAULT);
  } else {
    context.machine.request_transition(StateId::READY_IDLE);
  }
}

void exit(StateContext &) {
  output_control::set_fan_off();
}

}  // namespace

const StateHandler kAbortCooldownState = {
    "ABORT_COOLDOWN",
    enter,
    update,
    exit,
};

}  // namespace state_machine
