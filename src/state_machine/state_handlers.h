#pragma once

#include "state_machine/state_machine_types.h"

namespace state_machine {

extern const StateHandler kBootState;
extern const StateHandler kUpdateFwState;
extern const StateHandler kReadyIdleState;
extern const StateHandler kBlowerModeState;
extern const StateHandler kIgnitionCycleState;
extern const StateHandler kAbortCooldownState;
extern const StateHandler kSleepIdleState;
extern const StateHandler kFaultState;

}  // namespace state_machine
