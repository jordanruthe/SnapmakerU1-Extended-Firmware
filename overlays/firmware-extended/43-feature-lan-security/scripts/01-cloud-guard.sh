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

echo ">> Setting up cross-compilation environment..."
export CROSS_COMPILE=aarch64-linux-gnu-

echo ">> Compiling cloud-guard..."
make -C "$CUR_DIR/../apps/cloud-guard" install DESTDIR="$1"

echo ">> Validate binaries..."
stat "$1/usr/local/lib/libcloud-guard.so" >/dev/null

echo ">> cloud-guard installation completed successfully."
