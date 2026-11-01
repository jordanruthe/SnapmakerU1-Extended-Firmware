// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

#ifndef GUI_VENDORS_H
#define GUI_VENDORS_H

#include <stddef.h>
#include <stdint.h>

/* ---- limits ---------------------------------------------------------- */

#define GV_MAX_SITES        64
#define GV_MAX_ENTRIES_CAP  64     /* hard cap on entries per vendor row */
#define GV_MAX_ROWS_CAP     64     /* hard cap on vendor rows */
#define GV_VENDOR_LEN       64     /* max bytes of a vendor name (incl. NUL) */
#define GV_TYPE_LEN         32
#define GV_SUBTYPE_LEN      32     /* GUI strncpy()s type/subtype to 31 chars */
#define GV_LIBRARY_MAX_SIZE (1024 * 1024)

/* ---- logging --------------------------------------------------------- */

extern int gv_debug;     /* GUI_VENDORS_DEBUG: enables GV_DEBUG */
extern int gv_quiet;     /* tests: suppress stderr/syslog output */
void gv_log(int is_error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define GV_DEBUG(fmt, ...) do { if (gv_debug) gv_log(0, fmt, ##__VA_ARGS__); } while (0)
#define GV_INFO(fmt, ...)  gv_log(0, fmt, ##__VA_ARGS__)
#define GV_ERROR(fmt, ...) gv_log(1, fmt, ##__VA_ARGS__)

/* ---- descriptor ------------------------------------------------------ */
/*
 * Generated at build time by analyze_gui.py; text, one record per line,
 * '#' starts a comment, blank lines ignored, unknown records are an error:
 *
 *   gui_sha256 <hex>
 *   table <vaddr> rows <n> stride <bytes> max_entries <k>
 *   buttons <vaddr> slots <n>
 *   max_vendors <m>
 *   site table       <vaddr> <orig_insn_hex> <xd>
 *   site buttons     <vaddr> <orig_insn_hex> <xd>
 *   site bound_count <vaddr> <orig_insn_hex>
 *   site bound_maxidx <vaddr> <orig_insn_hex>
 *
 * vaddr/orig_insn accept 0x-prefixed or bare hex; xd is "x5" or "5".
 * All vaddrs are link-time (add the PIE load base at runtime).
 */
enum gv_site_kind {
    GV_SITE_TABLE = 1,       /* add Xd, Xn, #imm  -> table base */
    GV_SITE_BUTTONS,         /* add Xd, Xn, #imm  -> button array */
    GV_SITE_BOUND_COUNT,     /* cmp x, #3 -> #N   (count)   */
    GV_SITE_BOUND_MAXIDX,    /* cmp w, #2 -> #N-1 (max idx) */
};

struct gv_site {
    enum gv_site_kind kind;
    uint64_t vaddr;
    uint32_t orig_insn;      /* expected instruction word at vaddr */
    int xd;                  /* destination register (TABLE/BUTTONS), else -1 */
};

struct gv_descriptor {
    char gui_sha256[65];     /* lower-case hex */
    uint64_t table_vaddr;
    unsigned table_rows;     /* stock vendor rows */
    unsigned table_stride;   /* bytes per row */
    unsigned max_entries;    /* entries per row, excluding the vendor name */
    uint64_t buttons_vaddr;
    unsigned button_slots;   /* stock button slots */
    unsigned max_vendors;    /* cap on total vendor rows */
    struct gv_site sites[GV_MAX_SITES];
    unsigned n_sites;
};

int gv_descriptor_parse(const char *text, struct gv_descriptor *d,
                        char *err, size_t errlen);
int gv_descriptor_load(const char *path, struct gv_descriptor *d,
                       char *err, size_t errlen);
/* 0 if sha256(file at exe_path) == d->gui_sha256, else -1. */
int gv_descriptor_verify(const struct gv_descriptor *d, const char *exe_path,
                         char *err, size_t errlen);

/* SHA-256 of a file as 64 lower-case hex chars + NUL. 0 on success. */
int gv_sha256_file(const char *path, char hex[65]);
void gv_sha256_buf(const void *data, size_t len, char hex[65]);

/* ---- filament library ------------------------------------------------ */

struct gv_lib_entry {
    char vendor[GV_VENDOR_LEN];
    char type[GV_TYPE_LEN];
    char sub_type[GV_SUBTYPE_LEN];
};

struct gv_library {
    struct gv_lib_entry *entries;
    size_t n;
};

/* Parse filament_library.json text. An empty vendor means "Generic".
 * Entries without a type, or whose vendor/type/sub_type the GUI cannot pass
 * to Klipper (see field rules in core.c), are skipped and logged. Returns 0 and fills *lib (possibly
 * empty), or -1 on malformed JSON/structure (lib left empty). */
int gv_library_parse(const char *json, size_t len, struct gv_library *lib,
                     char *err, size_t errlen);
/* Missing file is not an error (empty library). */
int gv_library_load(const char *path, struct gv_library *lib,
                    char *err, size_t errlen);
void gv_library_free(struct gv_library *lib);

/* ---- rows ------------------------------------------------------------ */

/* One vendor row of the GUI table: vendor button label + entry strings
 * ("TYPE", "TYPE#Sub", or stock "Sub*TYPE"). */
struct gv_row {
    const char *vendor;
    const char *entries[GV_MAX_ENTRIES_CAP];
    unsigned n_entries;
};

struct gv_rows {
    struct gv_row v[GV_MAX_ROWS_CAP];
    unsigned n;
    unsigned n_stock;        /* v[0..n_stock) are the stock rows, same order */
    unsigned n_added_entries;/* entries appended into stock rows */
    unsigned n_new_vendors;  /* rows beyond n_stock */
    /* strings allocated by gv_build_rows; freed by gv_rows_free */
    char **owned;
    size_t n_owned;
};

/* Build the final table from stock rows + library (see plan "Row building
 * rules"). `stock` strings are borrowed and must outlive *out. */
int gv_build_rows(const struct gv_descriptor *d, const struct gv_library *lib,
                  const struct gv_rows *stock, struct gv_rows *out);
void gv_rows_free(struct gv_rows *r);

/* ---- patch engine (patch.c) ------------------------------------------ */

/*
 * Read the stock vendor table out of the running process into `stock`
 * (vendor/entry pointers point straight into the GUI's memory: the table
 * is in .data.rel.ro, already relocated when constructors run).
 * Returns 0 on success, -1 on failure (reason logged).
 */
int gv_read_stock_rows(const struct gv_descriptor *d, struct gv_rows *stock);

/*
 * Apply the descriptor's patch sites so the GUI uses `rows` (n rows,
 * layout/stride as in the descriptor, stock rows first in stock order) with
 * a button array of at least rows->n slots and raised bounds. Contract:
 *  - all-or-nothing: verify everything (every site's original word) before
 *    writing anything; on any failure return -1 with the process untouched;
 *  - the loader has already verified gui_sha256 and built `rows`;
 *  - strings of `rows` that are not in the GUI image are copied, so `rows`
 *    may be freed afterwards;
 *  - return 0 on success.
 */
int apply_patches(const struct gv_descriptor *d, const struct gv_rows *rows);

/*
 * GUI_VENDORS_SELFTEST: re-read and decode every patched site, trampoline
 * and the new table, and compare the executable's code with `exe` on disk
 * (only the sites may differ; nothing may differ when not applied). `d` and
 * `rows` are NULL when nothing was applied. Prints a summary to stderr.
 * Returns 0 = applied and verified, 2 = not applied and GUI untouched,
 * 1 = inconsistency.
 */
int gv_selftest(const struct gv_descriptor *d, const struct gv_rows *rows, const char *exe,
                double total_ms, double sha_ms);

#endif /* GUI_VENDORS_H */
