# SLEEP_IDLE

Low-power standby entered after inactivity in READY_IDLE. Device is in light sleep with the button configured as a wake source. Button press interrupt wakes the device back to READY_IDLE.

## Implementation Notes

- Rev B requires the buzzer, fan PWM, and glow plug PWM pins to be actively held low through sleep and restored on wake.
- The fan control analog path can otherwise drift into a biased state during sleep and raise quiescent current significantly even though the state machine is still in `SLEEP_IDLE`.
- Wake must always return to `READY_IDLE`. The wake press itself is discarded until the button has been released and the post-wake settle window has elapsed.

## Enter

- Turn LED off
- Configure button as wake source (GPIO interrupt)
- Force buzzer / fan / glow outputs low and hold them there through sleep
- Enter light sleep

## Update

- Dormant while in light sleep
- On button wake interrupt, transition to READY_IDLE

## Exit

- Restore buzzer / fan / glow pin control
- Re-enable any peripherals that were disabled by light sleep
