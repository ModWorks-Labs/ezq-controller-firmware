#include "state_machine/state_handlers.h"

#include "button_input.h"
#include "output_control.h"

namespace state_machine {
namespace {

void enter(StateContext &context) {
  context.machine.ready_idle_arming = false;
  context.machine.ready_idle_long_committed = false;
  output_control::set_indicator(output_control::Indicator::READY_IDLE);
  output_control::set_glow_off();
  output_control::set_fan_off();
}

void update(StateContext &context) {
  Event event = {};
  while (context.machine.dequeue_event(event)) {
    switch (event.type) {
      case EventType::BUTTON_PRESSED:
        output_control::play_button_press();
        break;

      case EventType::BUTTON_SHORT_PRESS:
        context.machine.request_transition(StateId::BLOWER_MODE);
        return;

      case EventType::BUTTON_HOLD_STARTED:
        context.machine.ready_idle_arming = true;
        output_control::set_indicator(output_control::Indicator::ARMING);
        break;

      case EventType::BUTTON_LONG_PRESS:
        context.machine.ready_idle_long_committed = true;
        context.machine.request_transition(StateId::IGNITION_CYCLE);
        return;

      case EventType::BUTTON_RELEASED:
        if (context.machine.ready_idle_arming && !context.machine.ready_idle_long_committed) {
          context.machine.ready_idle_arming = false;
          output_control::set_indicator(output_control::Indicator::READY_IDLE);
        }
        break;

      default:
        break;
    }
  }

  if (!button_input::is_pressed() &&
      (context.now_ms - context.machine.state_entry_ms) >= context.settings.idle_sleep_timeout_ms) {
    context.machine.request_transition(StateId::SLEEP_IDLE);
  }
}

void exit(StateContext &) {}

}  // namespace

const StateHandler kReadyIdleState = {
    "READY_IDLE",
    enter,
    update,
    exit,
};

}  // namespace state_machine
