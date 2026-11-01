#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

# Write printer_data/config and printer_data/database to a .tgz that
# config-restore.py accepts. Moonraker keeps its database open, so SQLite
# files are copied with SQLite's online backup instead of read as plain files.

import os
import sqlite3
import sys
import tarfile
import tempfile

PRINTER_DATA = "/home/lava/printer_data/"


def add_database(tar, folder):
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        arcroot = os.path.relpath(root, PRINTER_DATA)
        tar.add(root, arcroot, recursive=False)
        for name in sorted(files):
            path = os.path.join(root, name)
            if not name.endswith(".db"):
                if not name.endswith(("-wal", "-shm", "-journal")):
                    tar.add(path, os.path.join(arcroot, name))
                continue
            with tempfile.NamedTemporaryFile(dir="/userdata", suffix=".db") as tmp:
                src = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
                dst = sqlite3.connect(tmp.name)
                with dst:
                    src.backup(dst)
                src.close()
                dst.close()
                info = tar.gettarinfo(tmp.name, os.path.join(arcroot, name))
                st = os.stat(path)
                info.uid, info.gid, info.mode = st.st_uid, st.st_gid, st.st_mode & 0o7777
                with open(tmp.name, "rb") as f:
                    tar.addfile(info, f)
            print(f"Backed up {os.path.join(arcroot, name)}")


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <backup.tgz>")
        sys.exit(1)

    with tarfile.open(sys.argv[1], "w:gz") as tar:
        tar.add(os.path.join(PRINTER_DATA, "config"), "config")
        print("Backed up config")
        database = os.path.join(PRINTER_DATA, "database")
        if os.path.isdir(database):
            add_database(tar, database)
    print(f"Wrote {sys.argv[1]} ({os.path.getsize(sys.argv[1])} bytes)")


if __name__ == "__main__":
    main()
