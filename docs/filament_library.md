---
title: Filament Library
---

# Filament Library

Save your own filaments (e.g. "Overture PETG") once and use them everywhere:

- Apply a saved filament to an extruder from the web in one step.
- Have it show up on the **stock touchscreen** as extra brands and materials,
  next to the built-in ones. Nothing stock is removed.
- Load and unload at the right temperatures, including for materials the
  printer does not know.

The library is managed from the [Filament Manager](spoolman.md) and can be
filled from [Spoolman](spoolman.md).

## Opening the Library

Open **Filament Manager** (`/filament/`, also in the Fluidd/Mainsail menu) and
click **Filament library** in the header. The list is searchable. Clicking an
entry edits it; **Add filament** creates a new one.

Alternatively, click **Library** on an extruder card to pick a filament for
that extruder.

## Adding and Editing

| Field | Meaning |
|---|---|
| Name | Free text, for your own reference |
| Brand | Shown as a brand button on the touchscreen. Blank means `Generic` |
| Material | e.g. `PETG`, `PLA+`. Required |
| Subtype | Optional, e.g. `HF`, `Matte` |
| Behaves like | Only shown for materials the printer does not know (e.g. `PLA+`): which built-in material to borrow flow and speed values from |
| Colors | One or more colours |
| Load / Unload / Print (&deg;C) | Leave blank to use the printer's built-in value for the material |
| Flexibility | Rigid or soft (TPU-like); default follows the material |

The edit dialog shows how the entry will appear on the touchscreen, and warns
when it cannot be shown (see [Limits](#limits)).

Changes are saved to `extended/filament_library.json` in the printer config and apply to load/unload
temperatures straight away, with no Klipper restart.

## Applying to an Extruder

Click **Library** on an extruder card and pick a filament. It sets the
extruder's brand, material, subtype and colours, replacing whatever was there
(you are asked first if it came from an RFID tag or a Spoolman spool).

## Importing from Spoolman

With [Spoolman](spoolman.md) enabled, click **Import from Spoolman** in the
library dialog and import single filaments or **Import all shown**.

| Spoolman | Library |
|---|---|
| Vendor | Brand (`Generic` if none) |
| Material | Material |
| Variant (extra field) | Subtype |
| Colour / multi-colour | Colours |
| Extruder temperature | Print temperature |

Spoolman has no load/unload temperatures, so those are left blank. Importing
the same Spoolman filament again updates the existing entry and keeps the
load/unload temperatures you set by hand.

## Touchscreen

After a library change the dialog offers **Restart screen**. The touchscreen
reads the library only when it starts, so restart it to see changes. The
screen is unavailable for up to a minute; prints are not affected. The same
action is under **Restart Screen** in [firmware-config](firmware_config.md)
(Troubleshooting).

In the filament editor on the printer:

- Brands that are not Snapmaker, Polymaker or Generic appear as extra brand
  buttons **below** those three. Brands beyond the fourth need scrolling the
  brand column (being verified on hardware).
- Materials for Snapmaker and Polymaker are added to their lists where the
  stock list has free slots.
- Subtypes appear as `MATERIAL#Subtype`.

Choosing an entry on the touchscreen sets the same brand, material and
subtype as applying it from the web, so load/unload uses your temperatures.

### Limits

The touchscreen only shows entries it can pass on to Klipper. Others stay in
the library and still work from the web, but are marked "not shown on
touchscreen".

- **Brand and material** must not contain spaces or `# * ; ' "`.
- **Subtype** must not contain `# * ; ' "` (spaces are fine).
- Material and subtype are at most 31 characters.
- At most **23 materials per brand** on firmware 2.0.0, **26** on 2.1.0.
  Extra entries are dropped (first ones win).
- Snapmaker, Polymaker and Generic can only take materials in their free
  slots, and the number of free slots depends on the firmware version.
  Generic has none on 2.0.0, so entries with brand `Generic` do not appear
  on the touchscreen there.
- At most 32 brands in total, including the three stock ones.

### Same Brand, Material and Subtype

Load, unload and print temperatures are shared by all entries with the same
brand, material and subtype: the **first** one in the library is used. Use a
different subtype if two filaments need different temperatures.

## Checking Temperatures

Run in the Fluidd/Mainsail console:

```gcode
FILAMENT_LIBRARY_QUERY VENDOR=Overture TYPE=PETG SUBTYPE=HF
```

It reports the matching library entry (or `none`) and the resulting
`load_temp`, `unload_temp`, `print_temp`, `is_soft` and `flow_k`. `SUBTYPE`
and `NOZZLE` (default `0.4`) are optional.

## HelixScreen

[HelixScreen](helixscreen.md) is not affected: the touchscreen additions are
disabled while it is selected. The library still works for the web UI and for
Klipper's temperatures.

## Troubleshooting

- **Touchscreen unchanged**: click **Restart screen**, then check the entry is
  not marked "not shown on touchscreen" and that its brand has free slots (see
  [Limits](#limits)). Unsupported firmware versions are left unmodified, so
  stock options only appear.
- **Wrong load temperature**: use `FILAMENT_LIBRARY_QUERY` and check for an
  earlier entry with the same brand, material and subtype.
- **Logs**: the loader logs why it skipped an entry or did not patch the
  screen under `[gui-vendors]`. Use **Collect System Logs** in
  [firmware-config](firmware_config.md), or `/var/log/messages` when `syslog`
  logging is enabled.
