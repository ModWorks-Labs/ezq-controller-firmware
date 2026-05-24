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

void update(StateContext &) {}

void exit(StateContext &) {}

}  // namespace

const StateHandler kFaultState = {
    "FAULT",
    enter,
    update,
    exit,
};

}  // namespace state_machine
