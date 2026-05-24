# EZQ Controller Board Internal MCU Pinout

## Microcontroller

- Revision A: `ESP32-C3-MINI-1U-N4X`
- Revision B: `ESP32-C3-MINI-1-N4X`

Note: These ESP32-C3 variants use the same pinout. The only difference here is external versus integrated antenna.

## Pinout - Rev A

| GPIO | Function | Schematic Net | Notes |
| --- | --- | --- | --- |
| GPIO0 | NC | - | - |
| GPIO1 | RGB LED | `LED_PWM` | Addressable LED, standard NeoPixel control. |
| GPIO2 | Strapping pin | `BOOT2` | Unused aside from the external pull-up. |
| GPIO3 | Buzzer | `BUZZER_PWM` | Magnetic buzzer; this pin drives the gate of a low-side NMOS. |
| GPIO4 | Temperature sensor interrupt | `TEMP_INT` | Open-drain, pulled up externally. |
| GPIO5 | I2C SCL | `I2C0_SCL` | - |
| GPIO6 | I2C SDA | `I2C0_SDA` | - |
| GPIO7 | Accelerometer interrupt | `ACCEL_INT` | Push-pull, low by default. |
| GPIO8 | Strapping pin | `BOOT8` | Unused aside from the external pull-up. |
| GPIO9 | Strapping pin | `BOOT9` | Unused aside from the external pull-up. |
| GPIO10 | Button | `UI_BUTTON_MCU` | User-facing button for primary device control; pulled high by default and low when pressed. |
| GPIO18 | Glow plug drive | `GP_PWM` | Controls the low-side gate driver that switches power to the glow plug. |
| GPIO19 | Fan control | `BM_PWM` | Controls fan throttle; level-shifted to 5 V from 3.3 V logic. |
| GPIO20 | UART RX | `UART0_RX` | Tied to the Tag-Connect footprint for flashing and serial debugging; unused during standard device operation. |
| GPIO21 | UART TX | `UART0_TX` | Tied to the Tag-Connect footprint for flashing and serial debugging; unused during standard device operation. |

## Pinout - Rev B

| GPIO | Function | Schematic Net | Notes |
| --- | --- | --- | --- |
| GPIO0 | Battery voltage sense | `VSYS_SNS` | Tied to an external 200k/51k divider and scaled for ADC measurement. |
| GPIO1 | Fan control | `BM_PWM` | Controls fan throttle; filtered and scaled to 0-5 V for analog control with variable duty cycle. |
| GPIO2 | Strapping pin | `BOOT2` | Unused aside from the external pull-up. |
| GPIO3 | RGB LED | `LED_PWM` | Addressable LED, standard NeoPixel control. |
| GPIO4 | Temperature sensor interrupt | `TEMP_INT` | Open-drain, pulled up externally. |
| GPIO5 | I2C SCL | `I2C0_SCL` | - |
| GPIO6 | I2C SDA | `I2C0_SDA` | - |
| GPIO7 | NC | - | - |
| GPIO8 | Strapping pin | `BOOT8` | Unused aside from the external pull-up. |
| GPIO9 | Strapping pin | `BOOT9` | Unused aside from the external pull-up. |
| GPIO10 | Button | `UI_BUTTON_MCU` | User-facing button for primary device control; pulled high by default and low when pressed. |
| GPIO18 | Glow plug drive | `GP_PWM` | Controls the low-side gate driver that switches power to the glow plug. |
| GPIO19 | Buzzer | `BUZZ_PWM` | Magnetic buzzer; this pin drives the gate of a low-side NMOS. |
| GPIO20 | UART RX | `UART0_RX` | Tied to the Tag-Connect footprint for flashing and serial debugging; unused during standard device operation. |
| GPIO21 | UART TX | `UART0_TX` | Tied to the Tag-Connect footprint for flashing and serial debugging; unused during standard device operation. |
