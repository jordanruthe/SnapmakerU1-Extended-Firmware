#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

# Runs analyze_gui.py on the stock GUI of every extracted firmware
# (tmp/extracted-*/) and on mutated copies that it must reject. Needs
# python3-capstone 5.x; takes about 2 minutes per firmware.
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
exec python3 -B analyze_gui_test.py "$@"
