#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

# Drive the stock touchscreen GUI of an extended-qemu firmware from scripts.
# Boots scripts/dev/qemu.sh in the background and uses Remote Screen
# (/screen/snapshot, /screen/touch) plus SSH to look at and tap the GUI.

ROOT_DIR="$(realpath "$(dirname "$0")/../..")"
STATE_DIR="${GUI_TEST_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/snapmaker-u1-gui-test}"
PID_FILE="$STATE_DIR/qemu.pid"
SERIAL_LOG="$STATE_DIR/serial.log"
INPUT="$STATE_DIR/console.in"
HOLDER_PID_FILE="$STATE_DIR/holder.pid"
START_TIMEOUT="${START_TIMEOUT:-420}"
SSH_PORT=2222
HTTP_PORT=2280
SSH_PASS="${SSH_PASS:-snapmaker}"
export PATH="$PATH:/usr/sbin:/sbin"   # parted, mkfs.ext4 for qemu.sh

die() { echo "[!] $*" >&2; exit 1; }

usage() {
  cat >&2 <<EOF
usage: $0 <command> [args]

  start <rootfs.img>        boot QEMU in the background, enable Remote Screen,
                            wait for SSH and HTTP (START_TIMEOUT=${START_TIMEOUT}s)
  snap <out.png>            save a screenshot of the touchscreen
  tap <x> <y>               tap at panel coordinates (480x320)
  swipe <x1> <y1> <x2> <y2> drag from (x1,y1) to (x2,y2) (Remote Screen down/move/up)
  ssh <cmd...>              run a command as root on the emulated printer
  open-filament-edit [n]    from the home screen, open the filament Edit dialog
                            of extruder n (1-4, default 1)
  restart-gui               kill the GUI; lmd respawns it (waits until ready)
  stop                      kill QEMU

State (disk image, serial log, pid) lives in \$GUI_TEST_DIR (default: ~/.cache/snapmaker-u1-gui-test),
including the downloaded kernel.
EOF
  exit 1
}

qemu_pid() {
  local pid
  [[ -f "$PID_FILE" ]] || return 1
  pid="$(cat "$PID_FILE")"
  # Verify the pid is still our qemu, not a recycled one.
  # (qemu.sh execs qemu, so the cmdline is qemu.sh briefly, then qemu-system.)
  grep -qa qemu "/proc/$pid/cmdline" 2>/dev/null || return 1
  echo "$pid"
}

ssh_cmd() {
  sshpass -p "$SSH_PASS" ssh -p "$SSH_PORT" \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR -o ConnectTimeout=5 root@127.0.0.1 "$@"
}

http_ready() {
  curl -fs -o /dev/null --max-time 5 "http://127.0.0.1:$HTTP_PORT/"
}

# The QEMU kernel has no zstd squashfs support, but the build output is zstd.
# Repack as gzip (needs root, so run in the dev container) next to the image.
gzip_rootfs() {
  local img="$1" comp out
  comp="$(od -An -tu2 -j20 -N2 "$img" | tr -d ' ')"
  if [[ "$comp" != 6 ]]; then
    echo "$img"
    return
  fi
  out="$STATE_DIR/rootfs-gzip.img"
  if [[ ! "$out" -nt "$img" ]]; then
    echo ">> Repacking zstd rootfs as gzip (needs docker + snapmaker-u1-dev image)" >&2
    docker run --rm -v "$(dirname "$img"):/in:ro" -v "$STATE_DIR:/out" snapmaker-u1-dev bash -c \
      "unsquashfs -d /tmp/r /in/$(basename "$img") >/dev/null &&
       mksquashfs /tmp/r /out/rootfs-gzip.img -comp gzip -noappend >/dev/null &&
       chown $(id -u):$(id -g) /out/rootfs-gzip.img" >&2 || die "gzip repack failed"
  fi
  echo "$out"
}

GUI_LOG=/oem/printer_data/logs/gui.log
# Last startup step of the GUI; it blocks on MQTT retries for a few more seconds after.
GUI_READY_RE='Protocol initialization completed'

# Block until the (re)started GUI has finished its startup dialog.
# $1 = number of "startup done" lines already in the log before the restart.
wait_gui() {
  local before="$1" deadline=$((SECONDS + 240)) n
  while (( SECONDS < deadline )); do
    n="$(ssh_cmd "grep -ac '$GUI_READY_RE' $GUI_LOG" 2>/dev/null)" || n=0
    if (( n > before )); then
      sleep 25
      return 0
    fi
    sleep 3
  done
  die "GUI did not finish starting, see $GUI_LOG on the printer"
}

gui_ready_count() {
  ssh_cmd "grep -ac '$GUI_READY_RE' $GUI_LOG 2>/dev/null" || true
}

# lmd (the process supervisor) restarts gui about 2 seconds after it dies.
cmd_restart_gui() {
  local before
  before="$(gui_ready_count)"; before="${before:-0}"
  ssh_cmd 'kill $(pgrep -x gui)' || die "no running gui process"
  wait_gui "$before"
}

# The real Klipper cannot start in QEMU (no MCUs), which leaves the GUI on
# "Klipper failed to start" dialogs and without any filament data. Run a fake
# klippy socket instead, and mark the first-run wizard as done. Also turn on
# Advanced Mode (the .fluidd flag), which makes Moonraker's config root
# writable and starts firmware-config, as on a printer using the web UIs.
setup_gui() {
  local before deadline=$((SECONDS + 120))
  echo ">> Setting up fake klippy and GUI config"
  ssh_cmd 'cat > /tmp/fake-klippy.py && chmod 755 /tmp/fake-klippy.py' \
    < "$ROOT_DIR/scripts/dev/fake-klippy.py" || die "failed to copy fake-klippy.py"
  ssh_cmd '/etc/init.d/S60klipper stop >/dev/null 2>&1
    for p in $(pgrep -u lava -x python3); do
      grep -qa klippy.py /proc/$p/cmdline && kill $p
    done
    rm -f /home/lava/printer_data/comms/klippy.sock
    su lava -s /bin/sh -c "setsid python3 /tmp/fake-klippy.py /home/lava/printer_data/comms/klippy.sock >/tmp/fake-klippy.log 2>&1 &"
    printf "%s\n" "{\"system\":{\"guide_pass\":1,\"language\":\"en\",\"offset_pass\":1,\"bedmesh_pass\":1,\"shaper_pass\":1,\"motor_denoise_pass\":1,\"full_test_pass\":1,\"top_cover_guide\":1}}" > /home/lava/printer_data/config/gui_config.json
    chown lava:lava /home/lava/printer_data/config/gui_config.json
    touch /oem/printer_data/.fluidd
    sleep 3
    /etc/init.d/S61moonraker restart >/dev/null 2>&1
    /etc/init.d/S99firmware-config restart >/dev/null 2>&1' || die "failed to set up fake klippy"
  until curl -fs --max-time 5 "http://127.0.0.1:$HTTP_PORT/server/info" 2>/dev/null | grep -q '"klippy_state": "ready"'; do
    (( SECONDS < deadline )) || die "Moonraker did not connect to the fake klippy"
    sleep 3
  done
  # Restart the GUI only now, so it starts against a ready Moonraker.
  before="$(gui_ready_count)"; before="${before:-0}"
  ssh_cmd 'kill $(pgrep -x gui)' || true
  wait_gui "$before"
}

cmd_start() {
  [[ $# -eq 1 ]] || usage
  local rootfs
  rootfs="$(realpath -e "$1")" || die "rootfs image not found: $1"
  command -v sshpass >/dev/null || die "sshpass is required"
  command -v curl >/dev/null || die "curl is required"

  if pid="$(qemu_pid)"; then
    echo "QEMU already running (pid $pid)"
    return 0
  fi
  if (echo > /dev/tcp/127.0.0.1/$SSH_PORT) 2>/dev/null; then
    die "port $SSH_PORT already in use by something that is not this harness's QEMU"
  fi

  mkdir -p "$STATE_DIR"
  rootfs="$(gzip_rootfs "$rootfs")"
  : > "$SERIAL_LOG"
  rm -f "$INPUT"; : > "$INPUT"
  echo ">> Booting $rootfs (serial log: $SERIAL_LOG)"
  # stdin is fed by `tail -f` on a file (never EOF); the serial
  # console is an auto-login root shell that we drive through it.
  setsid tail -n 0 -f "$INPUT" 2>/dev/null | \
    QEMU_TMP_DIR="$STATE_DIR" DISK_IMG="$STATE_DIR/disk.img" setsid \
    "$ROOT_DIR/scripts/dev/qemu.sh" "$rootfs" > "$SERIAL_LOG" 2>&1 &
  echo $! > "$PID_FILE"
  sleep 1
  pgrep -f "^tail -n 0 -f $INPUT" | head -n1 > "$HOLDER_PID_FILE" || true

  # SSH and Remote Screen are disabled by default, so enable them over the
  # serial console (mirrors the firmware-config actions).
  local deadline=$((SECONDS + START_TIMEOUT))
  # Wait for the shell prompt first: input sent earlier fills the emulated
  # UART FIFO before the guest drains it and QEMU then stops reading stdin.
  until grep -qa 'root@lava:/# ' "$SERIAL_LOG"; do
    qemu_pid >/dev/null || die "QEMU exited during boot, see $SERIAL_LOG"
    (( SECONDS < deadline )) || { cmd_stop; die "timeout waiting for serial shell, see $SERIAL_LOG"; }
    sleep 3
  done
  printf 'echo HARNESS-UP\n' >> "$INPUT"
  until grep -qa '^HARNESS-UP' "$SERIAL_LOG"; do
    qemu_pid >/dev/null || die "QEMU exited, see $SERIAL_LOG"
    (( SECONDS < deadline )) || { cmd_stop; die "serial shell does not answer, see $SERIAL_LOG"; }
    sleep 2
  done
  echo ">> Serial shell is up, enabling SSH and Remote Screen"
  printf '%s\n' "E=/home/lava/printer_data/config/extended; C=/usr/local/bin/extended-config.py;" \
    "\$C add \$E/extended2.cfg remote_access ssh true;" \
    "\$C add \$E/extended2.cfg web remote_screen true;" \
    "/etc/init.d/S50dropbear restart; /etc/init.d/S99fb-http restart; echo HARNESS-DONE" >> "$INPUT"
  until grep -qa '^HARNESS-DONE' "$SERIAL_LOG"; do
    qemu_pid >/dev/null || die "QEMU exited during boot, see $SERIAL_LOG"
    (( SECONDS < deadline )) || { cmd_stop; die "timeout enabling SSH over serial, see $SERIAL_LOG"; }
    sleep 2
  done
  until ssh_cmd true 2>/dev/null; do
    qemu_pid >/dev/null || die "QEMU exited during boot, see $SERIAL_LOG"
    (( SECONDS < deadline )) || { cmd_stop; die "timeout waiting for SSH, see $SERIAL_LOG"; }
    sleep 3
  done
  echo ">> SSH is up"

  setup_gui

  until http_ready; do
    qemu_pid >/dev/null || die "QEMU exited during boot, see $SERIAL_LOG"
    (( SECONDS < deadline )) || { cmd_stop; die "timeout waiting for HTTP, see $SERIAL_LOG"; }
    sleep 3
  done
  until curl -fs -o /dev/null --max-time 10 "http://127.0.0.1:$HTTP_PORT/screen/snapshot"; do
    (( SECONDS < deadline )) || { cmd_stop; die "timeout waiting for /screen/snapshot"; }
    sleep 3
  done
  echo ">> Ready (pid $(cat "$PID_FILE"))"
}

cmd_snap() {
  [[ $# -eq 1 ]] || usage
  curl -fs --max-time 20 -o "$1.tmp" "http://127.0.0.1:$HTTP_PORT/screen/snapshot" \
    || { rm -f "$1.tmp"; die "snapshot failed (is it started? Remote Screen enabled?)"; }
  mv "$1.tmp" "$1"
}

# Number of touch-down events the GUI has logged. Used to confirm that a tap
# arrived: the emulated GUI is sometimes too busy and drops one.
gui_taps() {
  ssh_cmd "grep -ac 'event_handler_indev} down' $GUI_LOG 2>/dev/null" || true
}

cmd_tap() {
  [[ $# -eq 2 ]] || usage
  local before after try i
  for try in 1 2 3; do
    before="$(gui_taps)"; before="${before:-0}"
    curl -fs --max-time 10 -o /dev/null -X POST \
      "http://127.0.0.1:$HTTP_PORT/screen/touch?x=$1&y=$2" || die "tap failed"
    for i in 1 2 3 4 5; do
      sleep 1
      after="$(gui_taps)"; after="${after:-0}"
      (( after > before )) && { sleep "${TAP_DELAY:-1.5}"; return 0; }
    done
    echo "[!] tap $1,$2 not seen by the GUI, retrying" >&2
  done
  die "GUI ignored tap $1,$2"
}

# Drag with Remote Screen's a=down/move/up (in ~8 px steps, so LVGL sees a scroll).
cmd_swipe() {
  [[ $# -eq 4 ]] || usage
  local x1=$1 y1=$2 x2=$3 y2=$4 n i x y base="http://127.0.0.1:$HTTP_PORT/screen/touch"
  local dx=$((x2 - x1)) dy=$((y2 - y1)) len
  len=$(( ${dx#-} > ${dy#-} ? ${dx#-} : ${dy#-} ))
  n=$(( len / 8 + 1 ))
  curl -fs --max-time 10 -o /dev/null -X POST "$base?a=down&x=$x1&y=$y1" || die "swipe failed"
  sleep 0.3
  for i in $(seq "$n"); do
    x=$((x1 + dx * i / n)); y=$((y1 + dy * i / n))
    curl -fs --max-time 10 -o /dev/null -X POST "$base?a=move&x=$x&y=$y" || die "swipe failed"
    sleep 0.1
  done
  sleep 0.3
  curl -fs --max-time 10 -o /dev/null -X POST "$base?a=up&x=$x2&y=$y2" || die "swipe failed"
  sleep "${TAP_DELAY:-1.5}"
}

# Recorded from the 2.0.0 stock GUI at 480x320 (reference screenshots in
# docs/development.md). Start from the home screen:
#   home -> extruder n circle -> Filament -> pencil under extruder n
#        -> Filament Details -> Type row -> vendor list (Snapmaker/Polymaker/Generic)
cmd_open_filament_edit() {
  local n="${1:-1}"
  [[ "$n" =~ ^[1-4]$ ]] || die "extruder must be 1-4"
  local i=$((n - 1))
  cmd_tap $((178 + 80 * i)) 104
  cmd_tap $((93 + 98 * i)) 167
  cmd_tap 240 88
}

cmd_stop() {
  local pid
  if pid="$(qemu_pid)"; then
    kill "$pid"
    for _ in $(seq 20); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.5
    done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid"
    echo ">> Stopped QEMU (pid $pid)"
  else
    echo ">> QEMU not running"
  fi
  rm -f "$PID_FILE"
  if [[ -f "$HOLDER_PID_FILE" ]]; then
    pid="$(cat "$HOLDER_PID_FILE")"
    grep -qa "tail" "/proc/$pid/cmdline" 2>/dev/null && kill "$pid"
    rm -f "$HOLDER_PID_FILE"
  fi
}

[[ $# -ge 1 ]] || usage
cmd="$1"; shift
case "$cmd" in
  start) cmd_start "$@" ;;
  snap) cmd_snap "$@" ;;
  tap) cmd_tap "$@" ;;
  swipe) cmd_swipe "$@" ;;
  ssh) ssh_cmd "$@" ;;
  open-filament-edit) cmd_open_filament_edit "$@" ;;
  restart-gui) cmd_restart_gui ;;
  stop) cmd_stop ;;
  *) usage ;;
esac
