# FAULT

Latched terminal state entered when a critical fault occurs (over-temp, battery undervoltage, critical init failure). All outputs are forced safe, and the device cannot recover without a power cycle. No transitions out.

## Enter

- Force all outputs off (GP, fan)
- Set LED to fault indicator (red)
- Play fault buzzer pattern
- Record fault cause

## Update

- Hold all outputs safe
- Maintain fault LED and buzzer indication
- No transitions evaluated

## Exit

- Never runs, only cleared by power cycle
