# BOOT

The initial state on power-up. Runs once per boot to configure hardware, initialize peripherals, and check for a firmware update before handing off to normal operation. Transitions to UPDATE_FW if an update is available, READY_IDLE otherwise, or FAULT if a critical init step fails.

## Enter

- Configure GPIO pin directions and initial states
- Configure PWM channels for glow plug, fan, LED, and buzzer
- Initialize I2C and verify temperature sensor presence
- Initialize NVS and load saved config
- Play boot jingle, set LED to boot indicator

## Update

- Bring up WiFi (if provisioned) and run SNTP time sync
- Check OTA manifest for available firmware update
- Validate temperature sensor reading
- Validate battery voltage reading
- On update available, transition to UPDATE_FW
- On all checks passing and no update available, transition to READY_IDLE
- On critical failure, transition to FAULT

## Exit

- No teardown needed, hand off to next state