# Skip Up-to-Date MCUs on Upgrade

Stock `systemUpgrade.sh upgrade all` flashes the main MCU and every connected
print head MCU on each upgrade, even when they already run the MCU firmware
in the upgrade file.

This overlay patches `upgrade_all` to read each MCU's running version first
(the same USB `iProduct` check stock uses after flashing) and to skip the
flash when it already matches the first line of the upgrade file's
`MCU_DESC`. An MCU that differs, or whose version can't be read, is flashed,
verified and reverted on failure exactly as in stock.

The upgrade is run by the `systemUpgrade.sh` of the firmware being upgraded
from, so the skip takes effect from the upgrade after the one that installs
this overlay.

`systemUpgrade.sh upgrade mcu0|head0..3|headall <file>` still flashes
unconditionally, and the boot-time `check-restore` in `S60klipper` still
reflashes any MCU that does not match the installed firmware.
