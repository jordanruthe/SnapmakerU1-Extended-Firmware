# 44-feature-gui-vendors

Adds the user filament library (`extended/filament_library.json`, see
`43-feature-filament-library` and `docs/filament_library.md`) to the stock
touchscreen GUI as extra vendor buttons and materials. Nothing stock is
removed. The stock `/usr/bin/gui` is never replaced on disk: it is patched in
memory at startup.

## Parts

| Part | Role |
|---|---|
| `scripts/analyze_gui.py` | Build-time analyser. Disassembles `/usr/bin/gui`, locates the vendor table and every code site that depends on its size, and writes a descriptor (`gui.patch`, keyed by the GUI's SHA-256). Fails the build on any unexpected shape. |
| `scripts/01-gui-vendors.sh` | Build script: runs the analyser, cross-compiles the loader, installs `/usr/local/share/gui-vendors/gui.patch` and `/usr/local/lib/libgui-vendors.so`. |
| `apps/gui-vendors/` | The `LD_PRELOAD` loader (`libgui-vendors.so`): verifies the descriptor, builds the extended table from the library and patches the running GUI. Details in its [README](apps/gui-vendors/README.md). |
| `root/etc/hooks/lmd.d/40-gui-vendors.sh` | Adds the loader to `LD_PRELOAD` for `lmd`, which spawns `gui`. Skipped when `[components] gui = helixscreen`. The library only acts inside `/usr/bin/gui`. |
| `root/usr/local/share/firmware-config/functions/23_actions_restart_screen.yaml` | firmware-config action `restart-screen` (`pkill -x gui`); `lmd` respawns the GUI. Used by the **Restart screen** button in the Filament Manager. |

## Per-release regeneration

Nothing is hardcoded per firmware release. On every build the analyser runs
against the GUI of the rootfs being built and emits a fresh descriptor
(table address, row stride, entries per row, and each patched instruction with
its original word). The analyser enforces exact site counts and instruction
shapes, so a GUI change it does not recognise fails the build instead of
shipping a broken screen. At runtime the loader additionally checks the GUI's
SHA-256 and every original instruction word, and leaves the GUI untouched on
any mismatch (the reason is logged as `[gui-vendors]`).

Known layouts: 2.0.0 has a stride of 192 bytes (23 materials per vendor);
2.1.0 has 216 bytes (26 per vendor) and a different free-slot count per stock
vendor.

## Reloading

The GUI reads the table once at startup, so library changes need a GUI
restart (firmware-config **Restart Screen**, or kill `gui`).

## Tests

```bash
# Analyser on an extracted rootfs, including a static apply + re-disassembly
# (the build runs the same with --dry-run-rows 32)
python3 -I overlays/firmware-extended/44-feature-gui-vendors/scripts/analyze_gui.py \
  tmp/extracted-2.0.0/rootfs/usr/bin/gui -o /tmp/gui.patch --dry-run-apply

# Analyser on every tmp/extracted-* GUI plus mutated copies it must reject
overlays/firmware-extended/44-feature-gui-vendors/test/run.sh

# Loader host unit tests
make -C overlays/firmware-extended/44-feature-gui-vendors/apps/gui-vendors test
```

The loader's self-test against the real GUI under qemu-user is described in
the loader README. Needs `python3-capstone` 5.x (in the dev container).
