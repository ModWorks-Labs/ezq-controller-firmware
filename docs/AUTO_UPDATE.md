# Auto Update

## Overview

Auto firmware update is pull-based and runs during `BOOT` when:

- Wi-Fi credentials are provisioned
- the device reaches STA mode
- the manifest at `dev_config::kUpdateManifestUrl` is reachable

If any part of the update check fails, the firmware continues normal startup and enters
`READY_IDLE`.

## Version Source

The canonical firmware version is stored in `version.txt`.

That value is injected into `esp_app_desc.version` at build time and is the version string used
for cloud update comparison.

Supported format is semver, including prerelease suffixes like:

- `1.1.0`
- `1.1.0-alpha.1`

## Manifest Contract

The device fetches a fixed manifest URL from the default branch:

`https://raw.githubusercontent.com/ModWorks-Labs/ezq-controller-firmware/main/ezq-update-manifest.json`

The expected schema is shown in `manifests/ezq-update-manifest.template.json`.

Resolution order:

1. exact `unit_id` match in `unit_overrides`
2. `board_id` match in `boards`
3. no update target

Only `channel = stable` is supported in v1.

## Boot Flow

`BOOT` now behaves like this:

1. initialize control subsystems
2. if Wi-Fi is not provisioned, skip update check
3. if Wi-Fi is provisioned, wait up to 10 seconds for STA connectivity
4. fetch the manifest
5. compare current semver against the matched target
6. if newer firmware is available, transition to `UPDATE_FW`
7. otherwise continue to `READY_IDLE`

`UPDATE_FW` downloads the new app image into the inactive OTA slot and reboots immediately on
success.

## Rollback Contract

Bootloader rollback is enabled.

After a successful OTA boot, the new image is not confirmed immediately. The firmware confirms it
only after:

- control app initialized successfully
- the device reached `READY_IDLE`
- no fault occurred
- `READY_IDLE` remained stable for 10 seconds

If the new image faults or reboots before that confirmation, the bootloader rolls back to the
previous OTA slot.

## Release Workflow

For each release:

1. update `version.txt`
2. build the firmware image
3. compute SHA-256 of the `.bin`
4. publish the firmware asset
5. generate `ezq-update-manifest.json`
6. commit `ezq-update-manifest.json` to the repo default branch
7. attach the firmware asset to the GitHub release

Suggested firmware asset naming:

`ezq-controller-firmware_EZQ-CTLR-B_<version>.bin`

The v1 updater only modifies the OTA app slots.

It does not touch:

- `web_ui`
- `settings`
- `factory_nvs`
