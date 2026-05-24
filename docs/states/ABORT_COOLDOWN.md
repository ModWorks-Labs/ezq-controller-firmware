# ABORT_COOLDOWN

Mandatory fan-driven cooldown entered specifically when an ignition cycle is aborted during active glow plug drive (soft abort by button, fault abort by global detector). Does not apply to aborts during the fan-only post-cycle phase, since there is no GP to cool. Cannot be skipped or shortened by user input. Routes to READY_IDLE on soft aborts, or FAULT on fault-triggered aborts.

## Enter

- Force GP off (hard guarantee)
- Command fan to cooldown duty
- Record abort reason (soft or fault)
- Set LED to cooldown indicator

## Update

- Run fan for the configured cooldown duration
- Ignore button input

## Exit

- Command fan off
- Soft abort: transition to READY_IDLE
- Fault abort: transition to FAULT
