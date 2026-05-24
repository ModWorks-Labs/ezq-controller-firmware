# State Machine Architecture

## Overview

The EZQ firmware is built around a single state machine that owns all device behavior. It runs in the main control task and is the only place that commands outputs (glow plug, fan, buzzer, LED). All other systems (WiFi, sensors, dashboard) are external to it and communicate with it through events rather than directly modifying device state.

The state machine is modeled after a classical enter/update/exit pattern. Each state is a discrete, self-contained unit with three lifecycle functions. Logic is never shared across states - a state only checks transition criteria that are valid from within itself, making invalid transitions structurally impossible.

## State Lifecycle

**Enter** - Runs once when the state machine transitions into a state. Used for one-time setup: setting outputs, starting timers, playing audio/LED cues, loading data.

**Update** - Runs repeatedly on a fixed tick while the state is active. Used for continuous checks: monitoring button input, advancing timers, driving output changes over time, evaluating transition criteria.

**Exit** - Runs once when the state machine is leaving a state. Used for one-time teardown: stopping outputs, clearing timers, any cleanup before the next state takes over.

## Transitions

A state requests a transition by setting a target state. The state machine framework completes the transition at the end of the current update tick - it does not switch mid-update. This means exit() of the current state and enter() of the next state always run at a clean boundary, never in the middle of an update.

Transition flow:
- Current state update() sets next state
- Current state exit() runs
- Next state enter() runs
- Next state update() begins on the following tick

## Fault Handling

Faults (over-temperature, battery undervoltage) are detected globally outside the state machine by dedicated sensor monitoring. They do not belong to any individual state and are not checked inside update(). When a fault is detected, it is posted as an event to the state machine which forces a transition regardless of the current state. This means no state needs to defensively poll for faults - the fault system interrupts the state machine from the outside.

The one exception is ABORT_COOLDOWN, which continues to monitor for escalating faults during the cooldown run since the device is in a sensitive thermal condition.

## File Structure

Each state lives in its own source file. All states implement the same three functions. A central state machine module owns the current state, processes the event queue, calls the lifecycle functions, and executes transitions.

```
src/
  state_machine/
    state_machine.cpp       - framework, tick loop, transition logic, event queue
    state_machine.h
    states/
      boot.cpp / .h
      update_fw.cpp / .h
      ready_idle.cpp / .h
      sleep_idle.cpp / .h
      blower_mode.cpp / .h
      ignition_cycle.cpp / .h
      abort_cooldown.cpp / .h
      fault.cpp / .h
```

## State Categories

**Operational** - The normal device flow the user interacts with.
- BOOT
- READY_IDLE
- SLEEP_IDLE
- BLOWER_MODE
- IGNITION_CYCLE
- ABORT_COOLDOWN

**Service** - Abnormal or developer-facing states outside the normal flow.
- UPDATE_FW
- FAULT

## Context

Each state receives a shared context struct on every lifecycle call. This context holds references to everything a state might need - hardware driver handles, config, sensor readings, and the event queue. States do not reach for globals - everything comes through context.

## IGNITION_CYCLE Sub-State Machine

IGNITION_CYCLE is the most complex state and contains its own internal sub-state machine to manage the phases of an ignition sequence - countdown, glow plug drive, fan ramp, overlap handling, and post-cycle wind-down. From the outside the top-level state machine only sees IGNITION_CYCLE as a single state. The sub-machine is an implementation detail internal to that state's files.