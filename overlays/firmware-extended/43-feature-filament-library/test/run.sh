#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

# Applies the patch to the stock filament_parameters.py of every extracted
# firmware (tmp/extracted-*/) and runs the tests. No printer needed.
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
exec python3 -B filament_library_test.py "$@"
