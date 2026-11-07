#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

# Restore printer_data/config (and printer_data/database, when present) from a
# .tgz made by the firmware-config backup action, or by
# `tar czf backup.tgz -C /home/lava/printer_data config database`.
# Each replaced folder is kept as <folder>.pre-restore-<timestamp>.
# Klipper and Moonraker must be stopped while this runs.

import os
import posixpath
import pwd
import shutil
import sys
import tarfile
import time

PRINTER_DATA = "/home/lava/printer_data/"
FOLDERS = ("config", "database")


def fail(msg):
    print(f"ERROR: {msg}")
    sys.exit(1)


def validate(tar):
    members = tar.getmembers()
    names = set()
    links = set()
    for m in members:
        name = posixpath.normpath(m.name)
        if name.startswith("/") or name == ".." or name.startswith("../"):
            fail(f"unsafe path in archive: {m.name}")
        if name.split("/")[0] not in FOLDERS:
            fail(f"entry outside {'/, '.join(FOLDERS)}/: {m.name}")
        if not (m.isfile() or m.isdir() or m.issym()):
            fail(f"unsupported entry type: {m.name}")
        if m.issym():
            links.add(name)
        names.add(name)
        m.name = name

    # A symlink inside the archive must not be used as a directory by a later
    # entry, or the extraction could write outside printer_data
    for name in names:
        parent = posixpath.dirname(name)
        while parent:
            if parent in links:
                fail(f"entry below a symlink: {name}")
            parent = posixpath.dirname(parent)

    if "config/printer.cfg" not in names:
        fail("not a config backup (config/printer.cfg missing)")
    return members


def main():
    if len(sys.argv) != 2:
        fail(f"usage: {sys.argv[0]} <backup.tgz>")

    try:
        tar = tarfile.open(sys.argv[1], "r:*")
    except (tarfile.TarError, OSError) as e:
        fail(f"cannot read archive: {e}")

    with tar:
        members = validate(tar)
        folders = sorted({m.name.split("/")[0] for m in members})
        print(f"Archive OK: {len(members)} entries in {', '.join(folders)}")

        stamp = time.strftime('%Y%m%d-%H%M%S')
        moved = []
        for folder in folders:
            path = os.path.join(PRINTER_DATA, folder)
            if os.path.lexists(path):
                previous = f"{path}.pre-restore-{stamp}"
                os.rename(path, previous)
                moved.append((path, previous))
                print(f"Previous {folder} moved to {previous}")

        # Paths are checked by validate(); newer Pythons also want a filter
        kwargs = {"filter": "fully_trusted"} if hasattr(tarfile, "fully_trusted_filter") else {}
        try:
            for m in members:
                tar.extract(m, PRINTER_DATA, **kwargs)
        except Exception as e:
            for folder in folders:
                shutil.rmtree(os.path.join(PRINTER_DATA, folder), ignore_errors=True)
            for path, previous in moved:
                os.rename(previous, path)
            fail(f"extraction failed, previous files put back: {e}")

    lava = pwd.getpwnam("lava")
    for folder in folders:
        for root, dirs, files in os.walk(os.path.join(PRINTER_DATA, folder)):
            for name in [root] + [os.path.join(root, n) for n in dirs + files]:
                os.lchown(name, lava.pw_uid, lava.pw_gid)

    print(f"Restored {', '.join(folders)}")


if __name__ == "__main__":
    main()
