# develop-custom Features

Changes on the `develop-custom` branch that are not in upstream `develop`.
Builds are published to the
[`rolling-custom`](https://github.com/jordanruthe/SnapmakerU1-Extended-Firmware/releases/tag/rolling-custom)
pre-release on every push.

Add one line per feature at the end of the list. This file is merged with
`merge=union` (see `.gitattributes`), so lines added on different branches
are kept without conflicts. Edit an existing line on one branch only, or
both versions of it are kept.

- HTTPS for the web interfaces with self-signed, manual or ACME certificates and optional mTLS ([docs](docs/https.md))
- Links to the extended pages from the Fluidd and Mainsail menus
- Filament library in Filament Manager, with user temperatures read by Klipper ([docs](docs/filament_library.md))
- Filament library vendors and materials on the stock touchscreen
- Asks which filament each tool uses when printing from Fluidd or Mainsail ([docs](docs/print_filament_mapping.md))
- QEMU harness for testing the touchscreen GUI
- Config backup and restore in firmware-config ([docs](docs/firmware_config.md))
- Rolling `rolling-custom` pre-release built from every push to `develop-custom`
- Build caches removed from the rootfs in CI builds (about 12 MB smaller)
- Firmware upgrades skip MCUs that already run the new MCU firmware ([docs](docs/firmware_upgrade.md#mcu-firmware))
- LAN security: login or API key for the web interface and camera, localhost-only Moonraker/MQTT, key-only SSH, and Snapmaker cloud blocked ([docs](docs/lan_security.md))
