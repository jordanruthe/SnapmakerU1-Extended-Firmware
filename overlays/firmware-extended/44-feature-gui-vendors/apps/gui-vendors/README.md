# gui-vendors

An `LD_PRELOAD` library that adds user-defined filament vendors and types to
the stock Snapmaker touchscreen GUI (`/usr/bin/gui`) from
`extended/filament_library.json`. Nothing stock is removed. See the overlay
[README](../../README.md) for the overall picture and
`docs/filament_library.md` for the user documentation.

## How it works

- Injected into `lmd` by `root/etc/hooks/lmd.d/40-gui-vendors.sh` (skipped when
  `[components] gui = helixscreen`); `lmd` passes it on to `gui`.
- The constructor returns immediately unless `/proc/self/exe` is `/usr/bin/gui`.
- It loads the build-time descriptor (`/usr/local/share/gui-vendors/gui.patch`,
  produced by `scripts/analyze_gui.py`) and verifies `gui_sha256` against the
  running binary.
- It parses the library (vendored [jsmn](https://github.com/zserge/jsmn), MIT)
  and builds the vendor rows: entries are `TYPE` or `TYPE#Sub` (type
  upper-cased); an empty vendor means `Generic`; stock vendors (matched
  case-insensitively) get entries appended into free slots only, any other
  vendor becomes a new row after the stock rows, deduplicated, capped by the
  descriptor's `max_entries` and `max_vendors`. Overflow is logged and dropped.
- Entries the GUI cannot pass to Klipper are skipped and logged. The GUI sends
  `SET_PRINT_FILAMENT_CONFIG ... VENDOR=%s FILAMENT_TYPE=%s FILAMENT_SUBTYPE='%s'`,
  so vendor and type must not contain whitespace or `#*;'"`, sub_type must not
  contain `#*;'"`, and type/sub_type are limited to 31 bytes (the GUI
  `strncpy`s them).
- If the library adds nothing the GUI is left untouched (set
  `GUI_VENDORS_FORCE_APPLY=1` to patch anyway).

### Patch engine (`patch.c`)

Constructors run after the dynamic linker has relocated the GUI, so the stock
table (`const char *T[rows][stride/8]` in `.data.rel.ro`) already holds
runtime pointers. The load base comes from `dl_iterate_phdr` (the entry whose
program headers are `getauxval(AT_PHDR)`).

1. **Verify** (nothing is written before this passes): every site lies in the
   executable's `R E` segment, holds exactly the descriptor's original word and
   has the expected shape (`add Xd, Xn, #imm` with `Xd` from the descriptor;
   `cmp Rn, #rows` / `#rows-1`); the rows start with the stock rows (same
   pointers); the row count is within `max_vendors`.
2. **Region**: one anonymous mapping placed with `MAP_FIXED_NOREPLACE` at hints
   below the GUI image (then above it, from the far end down to leave room for
   the brk heap), such that the trampolines are within the ±128 MB reach of
   `b` from every site (1 MB margin). A kernel that ignores the flag returning a
   different address is handled (unmap, try the next hint). Layout:
   - new table, `rows × stride` bytes: stock rows copied verbatim, appended
     entries in their free slots, new rows after them, NULL-terminated;
   - button array, `max_vendors` zeroed slots (the GUI stores its vendor
     button pointers here, so it stays RW);
   - strings of new entries/vendors (the region does not depend on the heap);
   - trampolines on their own page(s), made `R-X`: per table/buttons site
     `movz/movk Xd, #<64-bit address>` (4 instructions) and `b site+4`.
3. **Text**: the pages holding the sites are made `RW`, each table/buttons site
   becomes `b tramp_i`, each `bound_count` compare gets `#rows` and each
   `bound_maxidx` compare `#rows-1` (only imm12 changes), then the pages go back
   to `R-X` and `__builtin___clear_cache` is run over every written range. The
   GUI is still single-threaded at constructor time. `add` sets no flags, so
   replacing it with a branch to `mov Xd, #imm` is semantics-preserving.

Any failure before step 3 unmaps the region and leaves the GUI untouched.

## Files

| File | Purpose |
|---|---|
| `main.c` | constructor, gating, orchestration, self-test hook |
| `core.c` | descriptor parser, SHA-256, library parser, row builder (host-testable) |
| `patch.c` | read stock table, apply patches, self-test |
| `gui_vendors.h` | descriptor/row structs and the patch engine interface |
| `jsmn.h` | vendored JSON tokenizer |
| `test/test_main.c` | host unit tests |

## Environment

| Variable | Default | Meaning |
|---|---|---|
| `GUI_VENDORS_DEBUG` | `0` | debug logging, incl. constructor timing (errors are always logged) |
| `GUI_VENDORS_DESCRIPTOR` | `/usr/local/share/gui-vendors/gui.patch` | descriptor path |
| `GUI_VENDORS_LIBRARY` | `/home/lava/printer_data/config/extended/filament_library.json` | library path |
| `GUI_VENDORS_FORCE_APPLY` | `0` | patch even if the library adds nothing |
| `GUI_VENDORS_SELFTEST` | `0` | test-only: verify the patched process, print a summary, `_exit` before `main` |
| `GUI_VENDORS_EXE` | `/usr/bin/gui` | test-only: executable path the constructor acts on (qemu-user) |

Logs go to stderr and syslog (`[gui-vendors]`).

## Build and test

```bash
make test                                  # native host unit tests
./dev.sh make -C overlays/firmware-extended/44-feature-gui-vendors/apps/gui-vendors CROSS_COMPILE=aarch64-linux-gnu-
```

### Analyzer

`scripts/analyze_gui.py` (python3-capstone 5.x only) fails the build on
anything it cannot explain:

- loop-carried pointers are tracked as value sets bounded by the loop's trip
  count, and an unbounded one that may reach the table or button array is an
  error;
- every use of a table reference must stay inside the table (patched) or past
  its end (the nozzle table, kept), never both and never below it;
- the button-array pointer may only be used for scaled slot accesses in a loop
  counted up by one from zero to the row count (or saved as a callee-saved
  register);
- vendor-index compares with `rows-1` must be upper bounds (`b.hi/gt/ls/le`);
- site counts must match the known layout.

The build also runs `--dry-run-apply`, which patches a copy in memory and
re-disassembles every site. `test/run.sh` runs the analyzer on every
`tmp/extracted-*` GUI and on mutated copies it must reject (about 2 minutes
per firmware).

### Self-test with the real GUI under qemu-user

No display is needed: the self-test exits before the GUI's `main`.

```bash
R=tmp/extracted-2.0.0/rootfs
python3 -I overlays/firmware-extended/44-feature-gui-vendors/scripts/analyze_gui.py $R/usr/bin/gui -o /tmp/gui.patch
qemu-aarch64 -L $R \
  -E LD_PRELOAD=$PWD/overlays/firmware-extended/44-feature-gui-vendors/apps/gui-vendors/libgui-vendors.so \
  -E GUI_VENDORS_SELFTEST=1 -E GUI_VENDORS_EXE=$(realpath $R/usr/bin/gui) \
  -E GUI_VENDORS_DESCRIPTOR=/tmp/gui.patch -E GUI_VENDORS_LIBRARY=/tmp/filament_library.json \
  $R/usr/bin/gui
```

The self-test re-reads every site and trampoline (branch targets, `movz/movk`
value and register, return branch, compare immediates, page permissions from
`/proc/self/maps`), checks the new table against the rows (stock pointers kept,
strings, NULL termination, zeroed button array) and compares the whole
executable segment with the file on disk: only the sites may differ when
applied, nothing when not. Exit status: `0` applied and verified, `2` not
applied and GUI untouched (reason logged above the summary), `1` inconsistency.
