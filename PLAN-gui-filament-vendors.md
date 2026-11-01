# Plan: User filaments on the stock U1 touchscreen

Status: draft for hand-off · 2026-11-01 · do not commit this file

## Goal

Let users add filaments (e.g. "Overture PETG") that appear in the **stock
Snapmaker touchscreen** filament editor as **additional** vendor buttons and
material entries — nothing stock is removed or replaced — and that load/unload
at the right temperatures. Filaments are managed in the `/filament/` web UI,
optionally imported from Spoolman.

Patching is per firmware release and fully scripted: the build analyses
`/usr/bin/gui` and fails loudly if the GUI changed in a way the analyser does
not recognise.

## Ground rules for every agent

- Repo: `/home/jordan/projects/SnapmakerU1-Extended-Firmware`. Work on a new
  branch `gui-filament-vendors` off `develop` (current branch
  `extended-nav-links` is unrelated). Never commit `u1-backup-*.tgz` or this
  plan file.
- **Commits and PRs must not contain `Co-Authored-By` or "Generated with"
  lines** (repo rule). Commit only when the orchestrator says so.
- Match surrounding code: SPDX headers on every new file, overlay layout
  (`overlays/firmware-extended/NN-name/{root,patches,scripts,apps}`), comment
  density of neighbouring files.
- Never `pkill -f` a pattern that appears in the same command line; find PIDs
  with `pgrep -x`/anchored patterns and kill by PID (relevant for QEMU).
- Python that reads extracted firmware runs with `-I`; scripts live outside
  `tmp/`.
- Stock rootfs for analysis: `tmp/extracted-2.0.0/rootfs` (build target, see
  `vars.mk`) and `tmp/extracted-2.1.0/rootfs` (next release).
- Report: what changed (paths), how it was verified (commands + output), and
  anything unverified. Do not claim a step works without running it.

## Already done (uncommitted, in working tree)

| Path | State |
|---|---|
| `overlays/firmware-extended/43-feature-filament-library/patches/home/lava/klipper/01-filament-library.patch` | Done. Patches `klippy/extras/filament_parameters.py`: reads `<config>/extended/filament_library.json` (mtime-reloaded), entries override `load_temp`/`unload_temp`/`print_temp`/`is_soft` by vendor/type/subtype (case-insensitive, stock precedence: exact → any subtype → generic vendor → generic+any), falls back to stock tables then to `base_type` (fixes unknown types like `PLA+` losing all params because `vol_speed` is missing). Adds `FILAMENT_LIBRARY_QUERY VENDOR= TYPE= [SUBTYPE=] [NOZZLE=]`. Verified: applies with `patch -F 0` to 2.0.0 and 2.1.0, behaviour tested against real stock tables. **No committed test yet.** |
| `overlays/firmware-extended/68-app-filament-ui/root/usr/local/filament-ui/html/library.js` (+ edits to `index.html`, `script.js`, `style.css`) | Mostly written: library modal (list/search/add/edit/delete), per-extruder "📚 Library" apply (`SET_PRINT_FILAMENT_CONFIG … FORCE=1`), Spoolman import (`server.spoolman.proxy` `/v1/filament`). Saves via Moonraker `POST /server/files/upload` (root=config, path=extended). **Playwright test fails**: after clicking "Filament library" the list never shows "No saved filaments yet" — undiagnosed. |
| Test harnesses (scratchpad) | `/tmp/claude-1000/-home-jordan-projects-SnapmakerU1-Extended-Firmware/c1b44c02-bf5a-46e6-b0f8-09bea3e742cc/scratchpad/ui/test_ui.py` (Playwright + mocked Moonraker WS/HTTP; run with plain `python3`, Playwright is in user site-packages), `…/scratchpad/t/run.py` (filament_parameters harness with stub printer), `…/scratchpad/disasm.py`, `…/scratchpad/xref.py`, `…/scratchpad/edit_view.s` (2.0.0 disassembly 0x151c00–0x155000), capstone at `…/scratchpad/pylib` (`PYTHONPATH`). |

### Library file contract (`extended/filament_library.json`)

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
stock tables. This file is the single source of truth for the web UI, Klipper
and the GUI loader.

## Reverse-engineering facts (GUI 2.0.0, `/usr/bin/gui`, stripped PIE aarch64, LVGL v8 statically linked)

All addresses are link-time vaddrs (add the PIE load base at runtime).

- **Vendor table** `const char *T[3][24]` at `0x69b520` in `.data.rel.ro`
  (R_AARCH64_RELATIVE relocated). Row stride `0xc0`. `T[v][0]` = vendor name,
  `T[v][1..23]` = entries, NULL-terminated. Entry syntax: `TYPE`,
  `TYPE#Subtype` (e.g. `PLA#Matte`), or `Subtype*TYPE` (e.g. `PolyLite*PLA`).
  Rows: Snapmaker (19 entries, 4 free), Polymaker (5, 18 free), Generic (23,
  full).
- Directly after it, `0x69b760`: a different table (pairs used for the 0.2 mm
  nozzle filter, iterated `0x69b7f0..0x69b850` / `0x69b760..`). Code reaches it
  **relative to the vendor table base** (`base+0x240`, `+0x2d0`, `+0x330`).
- **Edit-view state** global at `0x26ecdb0` (`.bss`): `+0x0` int8 vendor
  index, `+0x1` int8 type index, `+0x40` int8 colour index, `+0x60..+0x77`
  **3 vendor button pointers**, `+0x78` type container `lv_obj_t*`,
  `+0x80..+0x137` 23 type button pointers, `+0x230` and others in use.
- `select_vendor(int8 idx)` at `0x1527e0`: stores idx, loops 3 buttons to set
  CHECKED, cleans type container, bound `cmp w20,#2; b.gt`, builds type
  buttons from `T[idx][1..]` (loop stops at NULL or 23), skipping
  0.2-nozzle-forbidden types.
- Vendor buttons created in a loop at `0x1538e0`–`0x1539f4`:
  `ldr x25,[x28],#0xc0` walks rows; each button 109×52 at `y = 4 + 56*i`
  (`add w22,w22,#0x38`), label `T[i][0]`, `lv_obj_set_user_data(btn, i)`
  (`bl 0x8ed20`), click cb `0x152d80` (`LV_EVENT_CLICKED`=7 →
  `lv_obj_get_user_data` → tail-call `select_vendor`). Stored to
  `state+0x60+i*8`. Loop bound `cmp x27,#3`.
- **Sites to patch** (2.0.0):

  | Kind | Address | Instruction | Patch |
  |---|---|---|---|
  | bound (max index, unsigned) | `0x152030` | `cmp w0,#2; b.hi` | `#N-1` |
  | bound (count) | `0x15289c` | `cmp x19,#3` | `#N` |
  | bound (max index) | `0x152970` | `cmp w20,#2; b.gt` | `#N-1` |
  | bound (max index) | `0x152d40` | `cmp w20,#2; b.gt` | `#N-1` |
  | bound (count) | `0x1539f0` | `cmp x27,#3` | `#N` |
  | table ref | `0x1520fc`, `0x15215c`, `0x1522fc`, `0x1529b4`, `0x152fcc`, `0x1538e4` (+ any others the analyser finds) | `add Xd, Xn, #0x520` (Xn = adrp page `0x69b000`) | redirect to new table |
  | nozzle-table ref (**do not patch**) | `0x152bd0`, `0x154008` | `add #0x520` then `+0x240/+0x2d0/+0x330` | leave pointing at stock memory |
  | button array ref | `0x152870` (`add x21,x22,#0x60`), `0x1539e4` (`add x0,x25,#0x60`) | | redirect to new N-slot array |

  `cmp w0,#3` at `0x15322c` / `0x153d2c` are **unrelated** (return value of
  `0x132b30`).
- **2.1.0 differs**: table at `0x63c6e0`, Snapmaker row has 23 entries and the
  row stride is apparently no longer `0xc0` (rows 1+ misparse at stride `0xc0`).
  Must be re-derived, not assumed.
- Existing precedent for injection: `overlays/firmware-extended/40-feature-upgrade-firmware/`
  — `apps/firmware-imposter` (C, `CROSS_COMPILE=aarch64-linux-gnu-`, built by
  `scripts/01-firmware-imposter.sh` into `/usr/local/lib`), injected by
  `root/etc/hooks/lmd.d/10-firmware-imposter.sh` exporting `LD_PRELOAD` to
  `lmd` (which spawns `gui`), library self-gates on `/proc/self/exe`.
- Build container (`.github/dev/Dockerfile`, Debian trixie) has
  `gcc-aarch64-linux-gnu` and `python3` but **not capstone** — the analyser
  must either vendor a minimal AArch64 decoder for the handful of encodings it
  needs (adrp/add/ldr/cmp/b), or the Dockerfile adds `python3-capstone`
  (available in Debian). Prefer the Dockerfile route; confirm with the
  orchestrator.

## Design

```
build time                                   runtime (printer)
──────────                                   ─────────────────
scripts/NN-gui-vendors.sh                    lmd.d hook: LD_PRELOAD+=libgui-vendors.so
  └ analyze_gui.py rootfs/usr/bin/gui          └ gui starts → constructor in libgui-vendors.so
      → /usr/local/share/gui-vendors/             1. /proc/self/exe == /usr/bin/gui? else return
          gui-<sha256>.patch (descriptor)         2. load descriptor matching the binary; verify
      → fails build if any pattern                   every site's original instruction word
        count/shape is unexpected                 3. read extended/filament_library.json
  └ cross-compile libgui-vendors.so               4. build rows: stock rows (+ free-slot appends
                                                     for stock vendors) + one row per new vendor
                                                  5. mmap near the exe (≤128 MB away): new table
                                                     (stride preserved), N-slot button array,
                                                     trampolines
                                                  6. mprotect text RW, write `b tramp_i` at ref
                                                     sites and new cmp immediates, restore RX,
                                                     __builtin___clear_cache
                                                  any failure → log + leave GUI untouched
```

**Trampoline per ref site**: site `add Xd, Xn, #imm` → `b tramp_i`;
`tramp_i: movz/movk Xd, #new_addr (4 insns); b site+4`. `add` does not set
flags, so this is semantics-preserving. The analyser must refuse any ref site
that is not this exact form.

**Descriptor** (text, one record per line, generated):

```
gui_sha256 <hex>
table <vaddr> rows <n> stride <bytes> max_entries <k>
buttons <vaddr> slots <n>
max_vendors <m>            # cap; vendor index is int8, UI space limited
site table  <vaddr> <orig_insn_hex> <xd>
site buttons <vaddr> <orig_insn_hex> <xd>
site bound_count <vaddr> <orig_insn_hex>
site bound_maxidx <vaddr> <orig_insn_hex>
```

**Row building rules**: entry string = `TYPE#Sub` if `sub_type` else `TYPE`
(type upper-cased). Stock vendors (case-insensitive match on `T[v][0]`) get
new entries appended into free slots only; overflow is logged and dropped.
Every other vendor becomes a new row after the stock rows, in library order,
≤ `max_entries` each; dedupe. Total rows capped by `max_vendors`.

**Restart**: the GUI reads the table at startup, so library changes need a GUI
restart. Mechanism to be determined in Task 4 (does `lmd` respawn `gui` when
killed? how can the web UI trigger it — firmware-config action, Moonraker, or a
Klipper command?).

## Tasks

Model key: **Opus** = reverse engineering, binary patching, anything where a
wrong guess crashes the GUI. **Sonnet** = well-specified implementation,
harnesses, UI, docs. The orchestrator (Opus, main session) spawns agents with
the `model` parameter, reviews each report, and owns the go/no-go between
phases.

### Phase 0 — Audit (Opus, orchestrator or Opus agent) · blocks Phase 2

0.1 Scan the **whole** `.text` (not just edit_view) for every access to the
    state global `0x26ecdb0` with effective offsets `0x60..0x77`, and every
    use of `adrp 0x69b000` + `#0x520`. Any individual-slot access
    (e.g. `ldr x0,[xS,#0x68]`) changes the design (must also be redirected).
0.2 Trace how `show_filament_edit_view` sets `current_filament_maker_index`
    from the extruder's vendor string — is it a loop over 3 rows (another
    bound to patch) or a hardcoded mapping?
0.3 Trace the save path (`save_type`) to confirm the vendor sent to Klipper
    is `T[v][0]` and the type/subtype split of `#`/`*` entries.
0.4 Check the vendor-button parent container: is it scrollable (LVGL default
    `LV_OBJ_FLAG_SCROLLABLE`) or does the view clear the flag? Determines
    whether >4 vendors need a scroll fix.
0.5 Freeze the descriptor format above (amend if 0.1–0.4 require it).

Deliverable: short audit note appended to this plan + final site list.

### Phase 1 — parallel

**Task 1 · Klipper patch tests (Sonnet)**
- Add `overlays/firmware-extended/43-feature-filament-library/test/` with a
  runnable test (model on `overlays/firmware-extended/13-patch-rfid/test/`
  and the scratchpad `t/run.py`): applies the patch to the extracted
  `filament_parameters.py` for each available firmware version and asserts
  the cases (PLA+ with base PLA, HIPS base ABS, PolyTerra override, stock
  Snapmaker unchanged, unknown type unchanged, malformed entries ignored,
  mtime reload).
- Add `README.md` to the overlay.
- Accept: test passes on 2.0.0 and 2.1.0; `patch -F 0` clean.

**Task 2 · Finish web library (Sonnet)**
- Diagnose and fix the failing Playwright test (start by dumping
  `#library-list` innerHTML and console errors after opening the modal; check
  the `fetch` 404 path and that `initializeLibrary` ran).
- Make `test_ui.py` pass end-to-end; move it into
  `overlays/firmware-extended/68-app-filament-ui/test/` with run instructions.
- Add, in the edit dialog, a line showing how the entry will appear on the
  touchscreen (vendor button + `TYPE#Sub`), and a warning when a vendor would
  exceed the per-vendor or vendor-count caps (values from Task 3 descriptor:
  23 entries, cap TBD).
- Add the "Restart screen" button once Task 4 defines the mechanism.
- Accept: Playwright test green, screenshots at 1200px and 390px reviewed.

**Task 3 · Analyzer (Opus)**
- `overlays/firmware-extended/NN-feature-gui-vendors/scripts/analyze_gui.py`:
  locate the table (pointer-relocation scan for a `Snapmaker` row whose
  next slot is an entry string), derive stride and row count from code
  (`mov #stride`/`madd`/post-index `#stride`), find all ref sites, classify
  table vs nozzle-table refs by dataflow (result used with further
  `+0x240/+0x2d0/+0x330`), find button-array refs and the bound compares in the
  functions that use the table. Emit the descriptor.
- Strict expectations: exact counts per site kind, exact instruction shapes;
  any deviation → non-zero exit with a clear message.
- `--dry-run-apply`: apply the patch statically to a copy (trampolines placed
  in a fake segment) and re-disassemble every site to prove encodings.
- Run on 2.0.0 now; 2.1.0 is Task 7.
- Accept: descriptor for 2.0.0 matches the Phase 0 site list exactly.

**Task 4 · QEMU harness (Sonnet)**
- Script `scripts/dev/qemu-gui-test.sh` (or under the overlay `test/`):
  build `./dev.sh make build PROFILE=extended-qemu` (find where the rootfs
  image for `scripts/dev/qemu.sh` comes from), boot in the background, wait
  for SSH (`localhost:2222`) and HTTP (`localhost:2280`), enable Remote Screen
  if needed, and expose helpers: `snap <file>` (`GET /screen/snapshot`),
  `tap x y` (`POST /screen/touch?x=&y=`), `ssh` command runner, clean
  shutdown (kill by PID).
- Record a tap sequence that opens the filament **Edit** dialog for
  extruder 1 from the home screen; save reference screenshots.
- Determine the restart mechanism: does killing `gui` make `lmd` respawn it?
  What user does Klipper/Moonraker run as; can firmware-config
  (`overlays/firmware-extended/02-firmware-config`) expose an action? Propose
  one.
- Accept: script boots stock-extended firmware and produces a screenshot of
  the filament edit dialog with Snapmaker/Polymaker/Generic visible.

**Task 5 · Loader skeleton (Sonnet)**
- `overlays/firmware-extended/NN-feature-gui-vendors/apps/gui-vendors/`
  (`main.c`, `Makefile`, `README.md`) modelled on `firmware-imposter`:
  constructor, `/proc/self/exe` gating, logging to stderr/syslog with a
  `GUI_VENDORS_DEBUG` env, descriptor parser, JSON library parser (vendor a
  small MIT single-header parser such as jsmn; no new device libs), row
  builder implementing the rules above. Patch engine is a stub
  (`apply_patches()` returning "not implemented").
- Host unit tests for descriptor parsing and row building (build natively
  with `gcc`, run on x86).
- Build script `scripts/NN-gui-vendors.sh` (runs analyser → descriptor,
  cross-compiles library, installs both); `root/etc/hooks/lmd.d/NN-gui-vendors.sh`
  prepends `LD_PRELOAD` (skip when `[components] gui` is `helixscreen`).
- Accept: host tests pass; cross-compile succeeds in `./dev.sh`.

### Phase 2 — Patch engine (Opus) · needs Phase 0, Tasks 3 and 5

- Implement `apply_patches()`: find load base (`getauxval(AT_PHDR)` /
  `dl_iterate_phdr` for the main object), verify every site's original word,
  `mmap` with `MAP_FIXED_NOREPLACE` at candidate hints within ±128 MB of the
  sites, build table (copy stock rows verbatim, then new rows, preserve
  stride), N-slot button array, trampolines; patch text under `mprotect`
  RW→RX; clear icache. All-or-nothing: verify everything before writing
  anything.
- Accept: library loads in QEMU (Task 6) with an empty library and the GUI is
  byte-for-byte behaviourally unchanged (3 stock vendors).

### Phase 3 — Integration

**Task 6 · End-to-end in QEMU (Sonnet, escalate crashes to Opus)**
- Using Task 4 harness, with library entries for 2 new vendors (one with a
  subtype) + 1 appended Snapmaker entry:
  1. Edit dialog shows 5 vendor buttons; new vendor shows its entries.
  2. Selecting "Overture → PETG#HF" and saving sets `print_task_config`
     vendor/type/subtype (check via Moonraker
     `printer.objects.query?print_task_config`).
  3. `FILAMENT_LIBRARY_QUERY VENDOR=Overture TYPE=PETG SUBTYPE=HF` reports the
     library temps.
  4. Stock vendors still work; with 8+ vendors the column scrolls (or the fix
     from 0.4 is applied).
  5. Corrupt descriptor / mismatched instruction → GUI starts unpatched and
     logs why.
- Any GUI crash, hang or wrong-row behaviour → stop and hand the core dump /
  log / screenshots to Opus.

**Task 7 · 2.1.0 support (Opus)**
- Make the analyser derive 2.1.0's layout (stride, rows, sites); run Task 6
  against a 2.1.0-based build once `vars.mk` moves or by overriding
  `FIRMWARE_VERSION`.

### Phase 4 — Finish

**Task 8 · Docs (Sonnet)** — `docs/filament_library.md` (web library,
Spoolman import, touchscreen vendors, restart, limits: 23 entries/vendor,
vendor cap, temps shared per vendor/type/subtype — first entry wins), link
from `docs/index.md` (Klipper Customization list) and `docs/spoolman.md`;
overlay READMEs with the RE notes above.

**Task 9 · Review (Opus)** — `/code-review` on the branch diff, fix
findings, then **hardware check on the user's U1** (user installs the build;
agent verifies via SSH + `/screen/` snapshots). Commit and PR only on user
approval.

## Orchestration

```
Phase 0 (Opus) ──────────────┐
Task 1 (Sonnet) ─────────────┼──────────────────────────────┐
Task 2 (Sonnet) ─────────────┤                              │
Task 3 (Opus)   ───────┐     │                              │
Task 4 (Sonnet) ───────┼─────┼──> Task 6 (Sonnet) ──> Task 7 (Opus)
Task 5 (Sonnet) ───────┴─> Phase 2 (Opus) ──┘               │
                                                             └─> Task 8 (Sonnet) ─> Task 9 (Opus)
```

- Run Phase 0 and Tasks 1, 2, 4, 5 in parallel; Task 3 after Phase 0.
- Parallel agents touching different overlays can share the working tree;
  use `isolation: "worktree"` only for agents that run `./dev.sh make build`
  concurrently (shared `tmp/` caches — prefer serialising builds instead).
- Each agent prompt must include: this plan's path, its task section, the
  ground rules, and the exact acceptance check to run.
- Escalate to Opus whenever a Sonnet agent's fix involves instruction
  encodings, struct offsets, or an unexplained GUI crash.

## Audit (Phase 0)

Done 2026-11-01 on GUI 2.0.0 (`bd2d80e9…ebda4`), cross-checked on 2.1.0.
**Nothing invalidates the design.** Method: CFG-following constant
propagation from every `adrp` of the relevant pages over all of `.text`
(not only edit_view), plus an independent linear sweep, plus a scan of all
`R_AARCH64_RELATIVE` relocations; results agree.

### 0.1 Sites (whole `.text`)

- Button slots `0x26ece10..0x26ece27` (state+0x60): reached **only** through
  `0x152870 add x21,x22,#0x60` → `ldr x0,[x21,x19,lsl #3]` (select_vendor
  CHECKED loop) and `0x1539e4 add x0,x25,#0x60` → `str x20,[x0,x27,lsl #3]`
  (creation loop). No individual-slot load/store, no stack spill of the
  array pointer, no data pointer into the struct range, no memset of the
  struct (type-button array +0x80 is cleared by select_vendor, not +0x60).
  Calls receiving state pointers (`0x152a0c` x4, `0x154128` x1, `0x152fec` x1)
  are stale registers (callees overwrite them); `0x1535b8` passes state+0x280
  (unrelated field). Separate 3-slot array at `0x26ecd00` (`cmp w2,#2` at
  `0x151c8c`) is a callback stack, unrelated.
- Vendor table `0x69b520..0x69b760`: only the 8 `add #0x520` sites below; no
  `adr`, literal load, other offset, or relocated data pointer into it (only
  `.got 0x69f198 → 0x69b850`, end of the next table).

Final 2.0.0 site table (exactly what `analyze_gui.py` emits):

| Kind | Address | Word | Instruction |
|---|---|---|---|
| table | `0x1520fc` | `911482a4` | `add x4, x21, #0x520` (save_type log) |
| table | `0x15215c` | `911482a0` | `add x0, x21, #0x520` (save_type entry) |
| table | `0x1522fc` | `911482b5` | `add x21, x21, #0x520` (save_type vendor) |
| table | `0x1529b4` | `91148000` | `add x0, x0, #0x520` (select_vendor rows) |
| table | `0x152fcc` | `91148021` | `add x1, x1, #0x520` (type click label) |
| table | `0x1538e4` | `911480bc` | `add x28, x5, #0x520` (button creation walk) |
| buttons | `0x152870` | `910182d5` | `add x21, x22, #0x60` |
| buttons | `0x1539e4` | `91018320` | `add x0, x25, #0x60` |
| bound_count | `0x15289c` | `f1000e7f` | `cmp x19, #3` |
| bound_count | `0x1539f0` | `f1000f7f` | `cmp x27, #3` |
| bound_maxidx | `0x152030` | `7100081f` | `cmp w0, #2` (`b.hi`, unsigned) |
| bound_maxidx | `0x152970` | `71000a9f` | `cmp w20, #2` (`b.gt`) |
| bound_maxidx | `0x152d40` | `71000a9f` | `cmp w20, #2` (`b.gt`) |
| keep (nozzle) | `0x152bd0` | `91148013` | `add x19, x0, #0x520` → +0x240/+0x2d0 |
| keep (nozzle) | `0x154008` | `91148339` | `add x25, x25, #0x520` → +0x2d0/+0x330 |
| unrelated | `0x15322c`, `0x153d2c` | `71000c1f` | `cmp w0, #3` after `bl 0x132b30` |

Row stride evidence: `mov #0xc0` + `mul/madd` at `0x152110`, `0x15230c`,
`0x1529c8`, `0x152fd8`; post-index `#0xc0` at `0x1538e8`. Entry-count loop
bound `cmp x20,#0x17` at `0x152b04` (23 = stride/8 − 1); save_type also
checks the type index `cmp w0,#0x16; b.ls` (`0x152044`) — per-row entries are
not changed by the loader, so neither is patched.

### 0.2 `show_filament_edit_view` (`0x153e90`)

No string→index lookup exists. It sets vendor/type/colour indices
(state+0/+1/+0x40) to −1, compares the current vendor/type strings only
against `"NONE"`/`"Support"` and the 0.2 mm nozzle pair table
(`base+0x2d0..+0x330`, the `0x154008` keep-site), shows the current filament
as text (`"%s %s %s"`), then **always calls `select_vendor(0)`**. The
`current_filament_maker_index…` log is in save_type and just prints
state+0/+1. No additional bound.

### 0.3 Save path (`save_type`, `0x152000`)

Bounds `vendor ≤ 2` (`0x152030`) and `type ≤ 22`; `e = strdup(T[v][t+1])`;
if `e` contains `*`: `strtok(e,"*")` → subtype = token1, type = token2
(`PolyLite*PLA` → type `PLA`, subtype `PolyLite`); else if it contains `#`:
type = token1, subtype = token2 (`PLA#Matte`); else type = `e`, subtype "".
Both copied with `strncpy(…, 31)` (**31-char limit** for type and subtype).
Then calls the callback stored at state+8 (set by `0x1543d0` from
`0x17fda4` → `0x17ef50`) with `(state+0x10 extruder, T[v][0], type, subtype)`,
which tail-calls `0x1b89b0`:
`SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=%u VENDOR=%s FILAMENT_TYPE=%s FILAMENT_SUBTYPE='%s'`.
Vendor = `T[v][0]` confirmed.

**Design consequence (row builder / web UI validation):** Klipper's
`extended_r` stops the argument list at the first `#`, `*` or `;`, and
VENDOR / FILAMENT_TYPE are unquoted. So vendor and type must not contain
whitespace, `#`, `*`, `;`, `'` or `"`; subtype must not contain `#*;'"`
(spaces are fine — it is quoted); type and subtype ≤ 31 chars. The loader
must skip (log) library entries that violate this, and the web UI should
reject them.

### 0.4 Vendor-button container

Built at `0x153718`: `lv_obj_create(screen)` (`0x8d0d0`),
`lv_obj_remove_style_all` (`0x92510`), one style, size **117×265**, aligned
at (8, 0). Buttons 109×52 at (4, 4+56·i), so **4 rows fit**, a 5th shows
37 px. No `lv_obj_clear_flag` (`0x8d240`, verified `bic [obj+0x40]`) is
called anywhere in the view builder, so the container keeps LVGL's default
`LV_OBJ_FLAG_SCROLLABLE`; buttons are not scrollable, so drags on them
scroll the column. Scrollbar styling was removed → scrolling is expected to
work but without a visible scrollbar. To confirm in QEMU (Task 6 step 4);
no fix planned unless that fails. (`lv_obj_add_flag` = `0x8e930`,
`lv_obj_set_size` = `0x888d0`, `lv_obj_align` = `0x88dc0`,
`lv_btn_create` = `0xc8740`, `lv_label_create` = `0xe3e30`,
`lv_label_set_text` = `0xe56a0`, `lv_obj_set_user_data` = `0x8ed20`,
`lv_obj_get_user_data` = `0x8ed30`, `lv_obj_add_event_cb` = `0x866c0`,
`lv_obj_add_state/clear_state(bool)` = `0x8dae0`, `lv_obj_has_flag` = `0x8db00`.)

Other runtime notes: the vendor index is int8 (`sxtb`/`ldrsb`), so
`max_vendors` is 32 (descriptor) — well under 127 and the loader's 64 cap.
After patching, state+0x60..0x77 is never touched again.

### 0.5 Descriptor (frozen; unchanged from the proposal)

Exactly the proposed records; `#` starts a comment (the loader parser
already ignores them). Addresses `0x`-hex, words 8 hex digits, `xN`
register. The analyzer adds informational comments (vendors, nozzle
keep-sites, unrelated compares). Example (2.0.0):

```
gui_sha256 bd2d80e9084c6709161fd9ca4130f06c57e89b00ffa596bc6c5706c4f1aebda4
table 0x69b520 rows 3 stride 192 max_entries 23
buttons 0x26ece10 slots 3
max_vendors 32
site table 0x1520fc 911482a4 x4
...
site bound_maxidx 0x152d40 71000a9f
```

`max_entries` differs per release (23 on 2.0.0, 26 on 2.1.0); the web UI cap
should be read from the descriptor or use the minimum (23).
Verified the 2.0.0 descriptor parses with
`apps/gui-vendors/core.c:gv_descriptor_parse` (13 sites).

### Task 3 analyzer (done)

`overlays/firmware-extended/44-feature-gui-vendors/scripts/analyze_gui.py`
(`python3-capstone` added to `.github/dev/Dockerfile`; ~20 s per binary).

```
python3 -I analyze_gui.py <rootfs>/usr/bin/gui [-o gui.patch] [--dry-run-apply [--dry-run-rows N]] [--expect KIND=N]
```

Locates the table by relocation scan, derives stride (mul constants +
post-index), button array (store in the row-walk loop), rows (loop bounds),
vendor index variable (loads feeding the stride multiply), classifies refs
(value only used with `+off ≥ rows·stride` → keep), proves each site's base
register is a single constant over all predecessor paths, rejects any other
computation/access into the table or button array, explains every
`cmp #rows-1|#rows` in the involved functions, and enforces counts
(table 6, nozzle 2, buttons 2, bound_count 2, bound_maxidx 3).
`--dry-run-apply` patches an in-memory copy (trampolines in a synthetic
segment after the image), re-disassembles every site + trampoline (b
targets, movz/movk value, cmp register/imm) and checks no other byte
changed. Negative tests on mutated copies all fail with exit 1: table ref
with offset `#0x528`, count bound `#4`, `ldr x0,[x19,#0x68]` slot access,
a maxidx compare changed to `#1`.

### 2.1.0 (preview for Task 7)

Analyzer passes unchanged on 2.1.0 (`2cf51b70…e0472`), dry-run OK: table
`0x63c6e0`, **stride 216 (`0xd8`, 27 slots, max_entries 26)**, rows 3
(Snapmaker 25 entries, Polymaker 5, Generic 23), 8-byte pad then the nozzle
table (keep-sites `0x126a30` +0x290/+0x370, `0x127e68` +0x370/+0x3d0),
state `0x267ed00`, buttons `0x267ed60`. Sites: table `0x125f5c 0x125fbc
0x12615c 0x126814 0x126e2c 0x127744`; buttons `0x1266d0 0x127844`;
bound_count `0x1266fc 0x127850`; bound_maxidx `0x125e90 0x1267d0 0x126ba0`;
unrelated `cmp w0,#3` at `0x12708c 0x127b8c`. The 2.1.0 audit beyond the
analyzer's own checks (show_filament_edit_view, save path, container
geometry) has not been repeated.

### Phase 2 patch engine (done, qemu-user verified)

`apps/gui-vendors/patch.c`: load base via `dl_iterate_phdr` (entry with
`dlpi_phdr == getauxval(AT_PHDR)`); stock rows read from the relocated table
(pointers validated to be NUL-terminated strings inside the image). Apply
verifies all sites (in `R E` segment, word == descriptor, shape `add Xd,Xn,#imm`
with descriptor Xd / `cmp Rn,#rows|#rows-1`), rows (stock prefix pointer-equal,
≤ max_vendors) before writing. Region placement: free gaps from
`/proc/self/maps` (below the image first; above it avoiding the brk side),
then blind `MAP_FIXED_NOREPLACE` hints; result address checked (old-kernel
hint semantics). Blind hints alone failed ~1/3 of runs on 2.1.0 under
qemu-user (libs packed directly around the image); the maps search fixed it
(50/50 runs OK). Layout: table, `max_vendors` button slots, string pool, R-X
trampoline page. Text pages RW → write → R-X, icache cleared.
Self-test (`GUI_VENDORS_SELFTEST=1`, exit 0 applied / 2 untouched / 1 bad)
also diffs the whole `R E` segment against the file: exactly the 13 sites
differ when applied, 0 otherwise. Verified on 2.0.0 and 2.1.0 with the real
stock `gui`; negative cases (bad sha, one wrong orig word, wrong Xd, empty /
missing library, 2.1.0 descriptor on 2.0.0) leave 0 text words changed.
core.c: empty vendor → `Generic`; vendor/type reject whitespace and `#*;'"`,
sub_type rejects `#*;'"`; type/sub_type ≤ 31 bytes (`GV_SUBTYPE_LEN` 32).
Constructor cost is dominated by SHA-256 of the 7 MB binary (51 ms native
x86, ~0.4–1.1 s under qemu-user TCG); on-device time still to measure.
`scripts/01-gui-vendors.sh` (renamed from `.wip`) verified standalone in the
build container (capstone 5.0.7): installs `gui.patch` identical to the host
analyser output and an aarch64 `libgui-vendors.so`.
