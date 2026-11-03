#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

"""Checks that the restart-screen action merges into firmware-config's actions."""

import importlib.util
import os
import sys
import tempfile
import shutil

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../'))
FC = os.path.join(ROOT, '02-firmware-config/root/usr/local')
GUI = os.path.join(ROOT, '44-feature-gui-vendors/root/usr/local/share/firmware-config/functions')

spec = importlib.util.spec_from_file_location('firmware_config', os.path.join(FC, 'bin/firmware-config.py'))
fc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fc)  # the server only starts under __main__

with tempfile.TemporaryDirectory() as tmp:
    for src in (os.path.join(FC, 'share/firmware-config/functions'), GUI):
        for name in os.listdir(src):
            if name.endswith('.yaml'):
                shutil.copy(os.path.join(src, name), tmp)
    items = fc.load_functions_from_dir(tmp)['actions']['services']['items']

assert {'restart-klipper', 'restart-moonraker', 'restart-screen'} <= set(items), list(items)
action = items['restart-screen']
assert action['confirm'] is True and action['cmd'] == ['sh', '-c', 'pkill -x gui || true'], action
assert set(action) == set(items['restart-klipper']), (set(action), set(items['restart-klipper']))
print('OK')
