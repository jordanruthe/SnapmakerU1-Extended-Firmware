# 43-feature-filament-library

Lets Klipper use the user filament library (`extended/filament_library.json`,
managed from the `/filament/` web UI) when it picks temperatures for
load/unload/print. User documentation: `docs/filament_library.md`. Showing the
entries on the touchscreen is done by `44-feature-gui-vendors`.

## Patch

`patches/home/lava/klipper/01-filament-library.patch` changes
`klippy/extras/filament_parameters.py`:

- Reads `<config_dir>/extended/filament_library.json`. The file is re-read
  whenever its mtime changes, so saving from the web UI applies without a
  Klipper restart.
- A matching entry overrides `load_temp`, `unload_temp`, `print_temp` and
  `is_soft`. Anything the entry leaves out falls back to the stock tables.
- `base_type`: if the entry's `type` is not in the stock tables (e.g. `PLA+`)
  the stock lookup finds no `vol_speed` and the stock code discards every
  parameter (load at 250 C). With `base_type` set, missing values are taken
  from that type's stock tables instead.
- Missing file, malformed JSON or an invalid entry value: the library is
  treated as empty and stock behaviour applies (error is logged, no exception).
  Entries without a `type` are ignored.
- Adds `FILAMENT_LIBRARY_QUERY`.

## File format

```json
{ "version": 1,
  "filaments": [
    { "id": "lq3x9a1b", "name": "Overture PETG Grey", "vendor": "Overture",
      "type": "PETG", "sub_type": "", "base_type": "",
      "colors": ["808080"], "alpha": 255,
      "load_temp": 240, "unload_temp": 230, "print_temp": 235, "is_soft": false,
      "spoolman_filament_id": 12 } ] }
```

Only `type` is required. `base_type` only matters when `type` is not in the
stock tables. Klipper reads `type`, `vendor`, `sub_type`, `base_type`, the
three temperatures and `is_soft`; the other fields are for the UI.

## Precedence

Matching is case-insensitive. A blank vendor or sub type behaves like
`generic`. Lookup order, as for the stock tables:

1. exact vendor + type + sub type
2. vendor + type, any sub type
3. generic vendor + type + sub type
4. generic vendor + type, any sub type

No match: stock tables, unchanged.

## G-code

```
FILAMENT_LIBRARY_QUERY VENDOR=<vendor> TYPE=<type> [SUBTYPE=<sub>] [NOZZLE=0.4]
```

`VENDOR` defaults to `Generic`. Reports the matched library entry (or `none`)
and the resulting `load_temp`, `unload_temp`, `print_temp`, `is_soft`, `flow_k`.

## Test

```bash
overlays/firmware-extended/43-feature-filament-library/test/run.sh
```

For every `tmp/extracted-*/` that has a stock `filament_parameters.py`, the
test copies it to a temp dir, applies the patch with `patch -F 0` (as the
build does) and runs the unit tests against a stub printer. Versions without
an extracted rootfs are skipped. Needs only `python3` and `patch`.
