# Auto Update Test Checklist

Use this after release automation and cloud OTA are working to validate reliability, not just happy-path behavior.

## Good Path

- [x] Publish a new version with `python utils/prepare_release.py <version>`
- [x] Push `release/version.txt`
- [x] Push `release/ezq-update-manifest.json`
- [x] Push the staged firmware `.bin` in `release/`
- [x] Reboot a board that is on the previous version
- [x] Confirm it downloads, reboots, and reports the new version
- [x] Confirm `pending_confirmation=no` after stable boot
- [x] Confirm `rollback_armed=no` after stable boot

## Sequential Updates

- [x] Perform at least 3 version bumps in a row
- [x] Confirm each update advances one version at a time
- [x] Confirm no manual OTA flash is needed between versions

## Offline / Network Failure

- [ ] Boot with Wi-Fi unavailable
- [ ] Confirm normal startup still reaches `READY_IDLE`
- [ ] Restore Wi-Fi and reboot
- [ ] Confirm update works afterward

## Manifest Failure

- [ ] Point manifest URL target at a missing manifest or bad path
- [ ] Confirm device fails open and still boots normally
- [ ] Restore valid manifest and confirm recovery on next boot

- [ ] Publish a malformed manifest
- [ ] Confirm device fails open and still boots normally
- [ ] Restore valid manifest and confirm recovery on next boot

## Binary Failure

- [ ] Point manifest at a missing firmware binary
- [ ] Confirm device fails open and does not brick normal startup

- [ ] Point manifest at a binary with wrong metadata or mismatched content
- [ ] Confirm update is rejected cleanly

## Rollback

- [ ] Publish an intentionally bad firmware version
- [ ] Confirm device boots into the new slot and fails before confirmation
- [ ] Confirm bootloader rolls back to the previous version
- [ ] Confirm previous version still reaches `READY_IDLE`

## Power Interruption

- [ ] Remove power during firmware download
- [ ] Restore power
- [ ] Confirm device still boots into a valid image
- [ ] Confirm it can retry update on a later boot

## Timing / Propagation

- [x] Push a new release and reboot the board immediately
- [x] Note whether GitHub raw propagation delays the update
- [x] Reboot again after a short wait
- [x] Confirm device picks up the new version once files are reachable

## Multi-Board Sanity

- [ ] Test at least 2 boards
- [ ] Confirm both detect the same board-level release
- [ ] Confirm `unit_id` remains unchanged across updates
- [ ] Confirm `settings` remain intact across updates
- [ ] Confirm `factory_nvs` identity remains intact across updates
