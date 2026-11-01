#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

# Minimal stand-in for klippy's unix socket (webhooks API), so Moonraker and
# the stock touchscreen GUI see a "ready" printer under QEMU, where the real
# Klipper cannot start (no MCUs). Serves a canned object tree with four
# loaded extruders and applies SET_PRINT_FILAMENT_CONFIG to print_task_config.
#
# Runs on the emulated printer (python3 stdlib only):
#   fake-klippy.py /home/lava/printer_data/comms/klippy.sock

import copy, json, os, socket, sys, threading, time

N = 4
ETX = b"\x03"

def extruder(i):
    return {"temperature": 25.0, "target": 0.0, "can_extrude": False, "state": "IDLE",
            "activating_move": False, "switch_count": 0, "nozzle_diameter": 0.4,
            "nozzle_volume_type": "Standard", "extruder_offset": [0.0, 0.0, 0.0],
            "active_pin": False, "grab_valid_pin": False, "pressure_advance": 0.0,
            "smooth_time": 0.04}

PTC = {
    "filament_vendor": ["Generic"] * N, "filament_type": ["PLA"] * N,
    "filament_sub_type": ["Basic"] * N, "filament_color": [0xFFFFFFFF] * N,
    "filament_color_rgba": ["FF0000FF"] * N,
    "filament_color_multi": [{"nums": 1, "alpha": 255, "mode": 0, "colors": ["FF0000"]} for _ in range(N)],
    "filament_official": [False] * N, "filament_sku": [0] * N,
    "filament_edit": [True] * N, "filament_exist": [True] * N, "filament_soft": [False] * N,
    "extruder_map_table": list(range(N)) + [0] * 28, "extruders_used": [False] * N,
    "extruders_replenished": list(range(N)), "time_lapse_camera": False,
    "auto_bed_leveling": False, "flow_calibrate": False, "flow_calib_extruders": [True] * N,
    "shaper_calibrate": False, "auto_replenish_filament": True, "replenish_ignore_color": False,
    "filament_entangle_detect": False, "filament_entangle_sen": "medium",
    "end_led_turn_off": False, "end_unload_filament": [False] * N,
    "reprint_info": {"auto_bed_leveling": False, "flow_calibrate": False,
                     "flow_calib_extruders": [True] * N, "time_lapse_camera": False,
                     "extruder_map_table": list(range(N)) + [0] * 28,
                     "extruders_used": [False] * N},
}

STATUS = {
    "webhooks": {"state": "ready", "state_message": "Printer is ready"},
    "print_stats": {"filename": "", "total_duration": 0.0, "print_duration": 0.0,
                    "filament_used": 0.0, "state": "standby", "message": "", "info": {}},
    "gcode_move": {"speed_factor": 1.0, "position": [0, 0, 0, 0], "gcode_position": [0, 0, 0, 0]},
    "toolhead": {"homed_axes": "", "position": [0, 0, 0, 0], "extruder": "extruder"},
    "heater_bed": {"temperature": 25.0, "target": 0.0},
    "virtual_sdcard": {"progress": 0.0, "file_position": 0, "file_size": 0, "pl_env_valid": False},
    "fan": {"speed": 0.0, "rpm": 0},
    "print_task_config": PTC,
    "filament_detect": {"info": [{} for _ in range(N)], "state": [0] * N, "config": {}},
    "machine_state_manager": {"main_state": "IDLE", "action_code": "IDLE"},
    "configfile": {"config": {}, "settings": {}, "warnings": [], "save_config_pending": False},
}
for i in range(N):
    STATUS["extruder" if i == 0 else "extruder%d" % i] = extruder(i)
    STATUS["filament_motion_sensor e%d_filament" % i] = {"filament_detected": True, "enabled": True}
for side in ("left", "right"):
    STATUS["filament_feed " + side] = {"module_exist": False, "disable_auto": True,
                                       "filament_detected": False, "channel_state": 0}

lock = threading.Lock()

def query(objects):
    out = {}
    for name, fields in (objects or {}).items():
        if name not in STATUS:
            continue
        obj = STATUS[name]
        out[name] = copy.deepcopy(obj) if not fields else {k: copy.deepcopy(obj[k]) for k in fields if k in obj}
    return out

def gcode(script):
    for line in script.splitlines():
        parts = line.split()
        if not parts or parts[0].upper() != "SET_PRINT_FILAMENT_CONFIG":
            continue
        args = {k.upper(): v for k, v in (p.split("=", 1) for p in parts[1:] if "=" in p)}
        e = int(args["CONFIG_EXTRUDER"])
        keys = {"VENDOR": "filament_vendor", "FILAMENT_TYPE": "filament_type",
                "FILAMENT_SUBTYPE": "filament_sub_type", "FILAMENT_COLOR_RGBA": "filament_color_rgba"}
        for a, k in keys.items():
            if a in args:
                PTC[k][e] = args[a].strip("'\"")   # klippy unquotes the value
        if "SOFT" in args:
            PTC["filament_soft"][e] = bool(int(args["SOFT"]))
        sys.stderr.write("fake-klippy: %s\n" % line)
        notify({"print_task_config": copy.deepcopy(PTC)})

SUBSCRIBERS = []   # (connection, response_template) from objects/subscribe

def notify(status):
    """Push a status update to subscribers, as klippy does (Moonraker -> GUI)."""
    for conn, tmpl in list(SUBSCRIBERS):
        msg = dict(tmpl)
        msg["params"] = {"status": status, "eventtime": time.monotonic()}
        try:
            conn.sendall(json.dumps(msg).encode() + ETX)
        except OSError:
            pass

def handle(method, params):
    if method == "info":
        return {"state": "ready", "state_message": "Printer is ready", "hostname": "fake",
                "klipper_path": "/home/lava/klipper", "python_path": "/usr/bin/python3",
                "process_id": os.getpid(), "user_id": os.getuid(), "group_id": os.getgid(),
                "log_file": "/tmp/fake-klippy.log", "config_file": "/home/lava/printer_data/config/printer.cfg",
                "software_version": "fake"}
    if method == "list_endpoints":
        # Moonraker registers printer.objects.* etc. from this list.
        return {"endpoints": ["info", "emergency_stop", "objects/list", "objects/query",
                              "objects/subscribe", "gcode/help", "gcode/script",
                              "gcode/restart", "gcode/firmware_restart",
                              "gcode/subscribe_output", "query_endpoints/list"]}
    if method == "objects/list":
        return {"objects": sorted(STATUS)}
    if method in ("objects/query", "objects/subscribe"):
        return {"status": query(params.get("objects")), "eventtime": time.monotonic()}
    if method == "gcode/script":
        gcode(params.get("script", ""))
        return "ok"
    if method == "gcode/help":
        return {}
    if method == "gcode/subscribe_output":
        return {}
    return {}

def serve(conn):
    buf = b""
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                return
            buf += data
            while ETX in buf:
                raw, buf = buf.split(ETX, 1)
                try:
                    req = json.loads(raw)
                except ValueError:
                    continue
                with lock:
                    res = handle(req.get("method"), req.get("params") or {})
                    if req.get("method") == "objects/subscribe":
                        SUBSCRIBERS.append((conn, (req.get("params") or {}).get("response_template") or {}))
                if req.get("id") is not None:
                    conn.sendall(json.dumps({"id": req["id"], "result": res}).encode() + ETX)
    except OSError:
        pass
    finally:
        conn.close()

def main():
    path = sys.argv[1]
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    os.chmod(path, 0o777)
    srv.listen(8)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=serve, args=(conn,), daemon=True).start()

main()
