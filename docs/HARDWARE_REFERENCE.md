# EZQ Controller Board Rev B Hardware Notes

## ADC / Battery Sense

- `VSYS_SNS` on `GPIO0`
- Divider: `200k / 51k`
- `Vadc = Vsys * (51 / 251)`
- `Vsys = Vadc * (251 / 51)`
- Divider ratio back to system voltage: `~4.9216x`
- VSYS estimate from ADC reading: `vsys_est_v = raw * 0.00344001 + 0.24763`
- ADC averaging/filtering: `TBD`
- Tune/calibrate for `3S LiPo` range
- Low-voltage thresholds: `TBD`

## Fan Control

- `BM_PWM` on `GPIO1`
- Filtered and scaled to `0-5 V` analog control
- Rev A digital level-shifted PWM control did not work well with the ESC
- Rev A testing found a PWM frequency that the ESC input naturally low-passed enough for rough analog control
- Rev B uses intentional PWM-to-analog conversion
- Output stage uses an op-amp instead of a buffer
- `Rf = 51k`
- `Rg = 100k`
- Gain: `1.51x`
- PWM frequency: `TBD`
- Command-to-output scaling: `TBD`
- Minimum stable drive level: `TBD`
- Startup behavior / wake pulse: `TBD`

## Buzzer

- `BUZZ_PWM` on `GPIO19`
- Magnetic buzzer
- Drives low-side NMOS gate
- Standard tone frequency: `2.7 kHz` from the datasheet
- Duty / volume mapping: `TBD`

## Temperature

- `TEMP_INT` on `GPIO4`
- Open-drain, pulled up externally
- Digital temperature sensor on I2C bus
- Same temperature sensor handling as Rev A
- Same part and same pin usage as Rev A

## I2C

- `SCL = GPIO5`
- `SDA = GPIO6`
- Temperature sensor handling matches Rev A

## Glow Plug

- `GP_PWM` on `GPIO18`
- Controls low-side gate driver
- Same as Rev A

## RGB LED

- `LED_PWM` on `GPIO3`
- Addressable LED control

## Button

- `UI_BUTTON_MCU` on `GPIO10`
- Pulled high by default
- Low when pressed

## UART

- `UART0_RX` on `GPIO20`
- `UART0_TX` on `GPIO21`
- Tag-Connect programming / debug interface


## Still Need To Confirm

- Fan PWM frequency
- Fan analog scaling behavior
- Exact duty-to-throttle conversion
