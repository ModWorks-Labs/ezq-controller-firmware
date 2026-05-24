# IGNITION_CYCLE

The core function of the device. Runs the full ignition sequence from countdown through GP/fan execution to post-cycle wind-down, driven by the active profile loaded from storage. Internal phase logic (countdown, active cycle, post-cycle) lives inside this state.

## Enter

- Set LED to ignition indicator (orange)
- Load active profile from storage
- Run start-of-cycle countdown sequence (countdown beeps, high-pitched start beep)
- Initialize internal cycle phase tracking

## Update

- Advance through cycle phases per active profile (GP drive, fan throttle, overlap handling)
- Drive GP and fan outputs per phase requirements
- Monitor button: any press during countdown, cancel back to READY_IDLE (no cooldown needed, GP has not fired)
- Monitor button: any press during GP-active phase, transition to ABORT_COOLDOWN
- After main cycle complete, enter post-cycle phase (fan continues, periodic reminder beeps)
- Post-cycle button press, transition to READY_IDLE

## Exit

- Ensure GP is off
- Ensure fan is off (unless transitioning to ABORT_COOLDOWN, which will manage fan itself)
