# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

if [ "$1" = start ]; then
    EXTENDED_CFG="/home/lava/printer_data/config/extended/extended2.cfg"
    CLOUD_MODE=$(/usr/local/bin/extended-config.py get "$EXTENDED_CFG" security snapmaker_cloud blocked)

    if [ "$CLOUD_MODE" = "blocked" ]; then
        echo "Starting lmd with Snapmaker cloud blocked!"

        # Keep unisrv in LAN mode, whatever the touchscreen was last set to
        python3 - /home/lava/printer_data/mqtt/client.json <<'PY'
import json, os, sys
path = sys.argv[1]
try:
    with open(path) as f:
        cfg = json.load(f)
except (OSError, ValueError):
    cfg = {}
cur = cfg.get("link_mode")
want = 1 if isinstance(cur, int) and not isinstance(cur, bool) else "lan"
if cur != want and str(cur).lower() != "lan":
    cfg["link_mode"] = want
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f)
    os.replace(tmp, path)
    print("Forced link_mode to LAN")
PY

        export LD_PRELOAD="/usr/local/lib/libcloud-guard.so${LD_PRELOAD:+:$LD_PRELOAD}"
        export CLOUD_GUARD_BLOCK=1
        # The firmware-upgrade imposter's rewrite host (components upgrade != none)
        export CLOUD_GUARD_ALLOW_HOSTS="snapmakeru1-extended-firmware.pages.dev"
    fi
fi
