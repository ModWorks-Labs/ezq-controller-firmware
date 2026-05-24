# UPDATE_FW

Service state entered from BOOT only when the OTA manifest check found a newer firmware version. Downloads the new firmware image, writes it to the inactive OTA slot, and reboots into it. Auto-rollback handles a failed boot of the new image at the bootloader level, outside this state.

## Enter

- Set LED to update indicator (TBD pattern, distinct from boot/ready)
- Begin HTTPS download of new firmware image
- Optional: play update-start beep

## Update

- Stream firmware to inactive OTA slot
- Verify image integrity on completion
- On verified write, mark new slot as boot target and trigger reboot
- On download or verification failure, transition to READY_IDLE (current firmware remains active)

## Exit

- Reboot in the success path (this state ends with a chip reset, not a normal transition)
- On failure path, no teardown needed
