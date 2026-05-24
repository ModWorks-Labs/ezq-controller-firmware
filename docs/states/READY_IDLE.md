# READY_IDLE

The default standby state. Device is fully operational and waiting for user input. Branches to BLOWER_MODE, IGNITION_CYCLE, or SLEEP_IDLE depending on button input or inactivity timeout.

## Enter

- Set LED to ready indicator (green)
- Start inactivity timer

## Update

- Monitor button: short press, transition to BLOWER_MODE
- Monitor button: long press (with hold feedback), transition to IGNITION_CYCLE
- On inactivity timeout, transition to SLEEP_IDLE

## Exit

- Stop inactivity timer
