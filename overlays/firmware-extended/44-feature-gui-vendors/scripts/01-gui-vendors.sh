#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

CUR_DIR="$(realpath "$(dirname "$0")")"

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <rootfs-dir>"
  exit 1
fi

set -eo pipefail

echo ">> Analysing /usr/bin/gui..."
mkdir -p "$1/usr/local/share/gui-vendors"
python3 -I "$CUR_DIR/analyze_gui.py" "$1/usr/bin/gui" -o "$1/usr/local/share/gui-vendors/gui.patch"

echo ">> Setting up cross-compilation environment..."
export CROSS_COMPILE=aarch64-linux-gnu-

echo ">> Compiling gui-vendors..."
make -C "$CUR_DIR/../apps/gui-vendors" clean
make -C "$CUR_DIR/../apps/gui-vendors" install DESTDIR="$1"
make -C "$CUR_DIR/../apps/gui-vendors" clean

echo ">> Validate binaries..."
stat "$1/usr/local/lib/libgui-vendors.so" >/dev/null
stat "$1/usr/local/share/gui-vendors/gui.patch" >/dev/null
grep -q '^gui_sha256 ' "$1/usr/local/share/gui-vendors/gui.patch"

echo ">> gui-vendors installation completed successfully."
