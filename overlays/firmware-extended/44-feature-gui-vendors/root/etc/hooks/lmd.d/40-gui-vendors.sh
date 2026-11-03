# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

# Preload the vendor loader into `lmd` (inherited by the `gui` it spawns); the
# library only acts inside `/usr/bin/gui`. HelixScreen replaces the GUI, so skip it then.
if [ "$1" = start ]; then
    EXTENDED_CFG="/home/lava/printer_data/config/extended/extended2.cfg"
    GUI_SELECTED=$(/usr/local/bin/extended-config.py get "$EXTENDED_CFG" components gui snapmaker 2>/dev/null)
    if [ "$GUI_SELECTED" != helixscreen ] && [ -f /usr/local/lib/libgui-vendors.so ]; then
        export LD_PRELOAD="/usr/local/lib/libgui-vendors.so${LD_PRELOAD:+:$LD_PRELOAD}"
    fi
    unset EXTENDED_CFG GUI_SELECTED
fi
