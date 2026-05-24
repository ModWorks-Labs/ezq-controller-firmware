#include "state_machine/state_handlers.h"

#include "output_control.h"

namespace state_machine {
namespace {

void enter(StateContext &) {
  output_control::set_indicator(output_control::Indicator::BLOWER_MODE);
  output_control::play_mode_entry_beep();
  output_control::set_fan_percent(100.0f);
  output_control::set_glow_off();
}

void update(StateContext &context) {
  Event event = {};
  while (context.machine.dequeue_event(event)) {
    if (event.type == EventType::BUTTON_SHORT_PRESS) {
      context.machine.request_transition(StateId::READY_IDLE);
      return;
    }
  }
}

void exit(StateContext &) {
  output_control::set_fan_off();
  output_control::play_mode_exit_beep();
}

}  // namespace

const StateHandler kBlowerModeState = {
    "BLOWER_MODE",
    enter,
    update,
    exit,
};

}  // namespace state_machine
