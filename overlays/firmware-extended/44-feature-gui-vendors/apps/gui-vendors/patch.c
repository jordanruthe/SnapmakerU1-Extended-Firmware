// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

// Patch engine: reads the stock vendor table out of the running GUI and
// redirects the GUI to a larger table/button array via trampolines.
//
// Every table/button reference site is `add Xd, Xn, #imm` (no flags). It is
// replaced by `b tramp_i`, and tramp_i is `movz/movk Xd, #<new address>`
// (4 instructions) followed by `b site+4`. Bound compares
// (`cmp Rn, #imm12`) get a new imm12. Trampolines live in one mapping
// placed within +-128 MB of the sites (range of `b`).

#define _GNU_SOURCE
#include <errno.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gui_vendors.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define B_RANGE        (128u << 20)    /* b imm26: +-128 MB */
#define B_MARGIN       (1u << 20)      /* stay well inside it */
#define HINT_STEP      (1u << 20)
#define HINT_TRIES     96
#define TRAMP_INSNS    5               /* movz, movk x3, b */
#define MAX_SEGS       16
#define MAX_STR        128             /* sanity limit for stock strings */

/* ---- main executable layout ------------------------------------------ */

struct seg {
    uintptr_t lo, hi;      /* runtime [lo, hi) */
    uint64_t vaddr, off, filesz;
    unsigned flags;        /* PF_R/PF_W/PF_X */
};

static struct {
    int ok;
    uintptr_t base;
    uintptr_t lo, hi;      /* whole image, page aligned */
    struct seg seg[MAX_SEGS];
    unsigned nseg;
} g_exe;

/* State of a successful apply_patches, for the self-test. */
static struct {
    int applied;
    uintptr_t region, region_size, tramp, table, buttons;
    unsigned button_slots;
    uintptr_t site_tramp[GV_MAX_SITES];
    uint32_t site_word[GV_MAX_SITES];
} g_patch;

static int exe_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    (void)data;
    if ((uintptr_t)info->dlpi_phdr != (uintptr_t)getauxval(AT_PHDR))
        return 0;
    g_exe.base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        struct seg *s;
        if (ph->p_type != PT_LOAD)
            continue;
        if (g_exe.nseg == MAX_SEGS)
            return -1;
        s = &g_exe.seg[g_exe.nseg++];
        s->lo = info->dlpi_addr + ph->p_vaddr;
        s->hi = s->lo + ph->p_memsz;
        s->vaddr = ph->p_vaddr;
        s->off = ph->p_offset;
        s->filesz = ph->p_filesz;
        s->flags = ph->p_flags;
    }
    return 1;
}

static int exe_init(void)
{
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);

    if (g_exe.ok)
        return 0;
    memset(&g_exe, 0, sizeof(g_exe));
    if (dl_iterate_phdr(exe_cb, NULL) != 1 || !g_exe.nseg) {
        GV_ERROR("cannot locate the main executable's program headers");
        return -1;
    }
    g_exe.lo = UINTPTR_MAX;
    for (unsigned i = 0; i < g_exe.nseg; i++) {
        if (g_exe.seg[i].lo < g_exe.lo)
            g_exe.lo = g_exe.seg[i].lo;
        if (g_exe.seg[i].hi > g_exe.hi)
            g_exe.hi = g_exe.seg[i].hi;
    }
    g_exe.lo &= ~(page - 1);
    g_exe.hi = (g_exe.hi + page - 1) & ~(page - 1);
    g_exe.ok = 1;
    GV_DEBUG("gui loaded at %#lx (image %#lx-%#lx)", (unsigned long)g_exe.base,
             (unsigned long)g_exe.lo, (unsigned long)g_exe.hi);
    return 0;
}

/* Segment containing [addr, addr+len) with all of `flags`, or NULL. */
static const struct seg *exe_seg(uintptr_t addr, size_t len, unsigned flags)
{
    for (unsigned i = 0; i < g_exe.nseg; i++) {
        const struct seg *s = &g_exe.seg[i];
        if (addr >= s->lo && addr <= s->hi && len <= s->hi - addr &&
            (s->flags & flags) == flags)
            return s;
    }
    return NULL;
}

/* A NUL-terminated string inside a readable segment of the executable. */
static int exe_str_ok(const char *p)
{
    const struct seg *s = exe_seg((uintptr_t)p, 1, PF_R);
    if (!s)
        return 0;
    for (size_t i = 0; (uintptr_t)p + i < s->hi && i < MAX_STR; i++)
        if (!p[i])
            return i > 0;
    return 0;
}

static int in_image(const void *p)
{
    return (uintptr_t)p >= g_exe.lo && (uintptr_t)p < g_exe.hi;
}

/* ---- AArch64 encodings ----------------------------------------------- */

#define IS_ADD_IMM64(w)    (((w) & 0xffc00000u) == 0x91000000u)   /* add Xd, Xn, #imm12 */
#define IS_CMP_IMM(w)      (((w) & 0x7fc0001fu) == 0x7100001fu)   /* subs {w,x}zr, Rn, #imm12 */
#define IMM12(w)           (((w) >> 10) & 0xfff)
#define SET_IMM12(w, v)    (((w) & ~(0xfffu << 10)) | ((uint32_t)(v) << 10))

static int enc_b(uintptr_t pc, uintptr_t target, uint32_t *out)
{
    int64_t off = (int64_t)(target - pc);
    if ((off & 3) || off < -(int64_t)B_RANGE || off >= (int64_t)B_RANGE)
        return -1;
    *out = 0x14000000u | ((uint32_t)(off >> 2) & 0x03ffffffu);
    return 0;
}

static int dec_b(uint32_t w, uintptr_t pc, uintptr_t *target)
{
    int64_t off;
    if ((w & 0xfc000000u) != 0x14000000u)
        return -1;
    off = (int64_t)((int32_t)(w << 6) >> 6) * 4;
    *target = pc + (uintptr_t)off;
    return 0;
}

static void enc_mov64(uint32_t *p, int rd, uint64_t v)
{
    p[0] = 0xd2800000u | (uint32_t)(v & 0xffff) << 5 | (uint32_t)rd;   /* movz */
    for (unsigned hw = 1; hw < 4; hw++)                                /* movk */
        p[hw] = 0xf2800000u | hw << 21 | (uint32_t)((v >> (16 * hw)) & 0xffff) << 5 | (uint32_t)rd;
}

static int dec_mov64(const uint32_t *p, int *rd, uint64_t *v)
{
    *rd = (int)(p[0] & 0x1f);
    *v = 0;
    for (unsigned hw = 0; hw < 4; hw++) {
        uint32_t want = (hw ? 0xf2800000u : 0xd2800000u) | hw << 21;
        if ((p[hw] & 0xffe00000u) != want || (int)(p[hw] & 0x1f) != *rd)
            return -1;
        *v |= (uint64_t)((p[hw] >> 5) & 0xffff) << (16 * hw);
    }
    return 0;
}

static int is_tramp_site(const struct gv_site *s)
{
    return s->kind == GV_SITE_TABLE || s->kind == GV_SITE_BUTTONS;
}

static unsigned bound_value(const struct gv_site *s, unsigned rows)
{
    return s->kind == GV_SITE_BOUND_COUNT ? rows : rows - 1;
}

static const char *kind_name(enum gv_site_kind k)
{
    switch (k) {
    case GV_SITE_TABLE: return "table";
    case GV_SITE_BUTTONS: return "buttons";
    case GV_SITE_BOUND_COUNT: return "bound_count";
    case GV_SITE_BOUND_MAXIDX: return "bound_maxidx";
    }
    return "?";
}

/* ---- stock table ----------------------------------------------------- */

static const char *const *stock_row(const struct gv_descriptor *d, unsigned r)
{
    return (const char *const *)(g_exe.base + d->table_vaddr + (uintptr_t)r * d->table_stride);
}

/* Number of entries in a stock row (up to the first NULL). */
static unsigned stock_count(const struct gv_descriptor *d, unsigned r)
{
    const char *const *row = stock_row(d, r);
    unsigned n = 0;
    while (n < d->max_entries && row[1 + n])
        n++;
    return n;
}

int gv_read_stock_rows(const struct gv_descriptor *d, struct gv_rows *stock)
{
    memset(stock, 0, sizeof(*stock));
    if (exe_init() < 0)
        return -1;
    if (!d->table_rows || d->table_rows > GV_MAX_ROWS_CAP ||
        d->max_entries > GV_MAX_ENTRIES_CAP ||
        (size_t)d->max_entries * 8 + 8 > d->table_stride) {
        GV_ERROR("descriptor table geometry out of range");
        return -1;
    }
    if (!exe_seg(g_exe.base + d->table_vaddr, (size_t)d->table_rows * d->table_stride, PF_R) ||
        ((g_exe.base + d->table_vaddr) & 7)) {
        GV_ERROR("vendor table %#lx is not inside the executable",
                 (unsigned long)d->table_vaddr);
        return -1;
    }
    for (unsigned r = 0; r < d->table_rows; r++) {
        const char *const *row = stock_row(d, r);
        struct gv_row *out = &stock->v[r];

        if (!exe_str_ok(row[0])) {
            GV_ERROR("stock row %u: bad vendor pointer %p", r, (const void *)row[0]);
            return -1;
        }
        out->vendor = row[0];
        out->n_entries = stock_count(d, r);
        for (unsigned i = 0; i < out->n_entries; i++) {
            if (!exe_str_ok(row[1 + i])) {
                GV_ERROR("stock row %u entry %u: bad pointer %p", r, i, (const void *)row[1 + i]);
                return -1;
            }
            out->entries[i] = row[1 + i];
        }
        GV_DEBUG("stock row %u: %s (%u entries)", r, out->vendor, out->n_entries);
    }
    stock->n = stock->n_stock = d->table_rows;
    return 0;
}

/* ---- trampoline region ----------------------------------------------- */

/* [lo, hi) can host trampolines for every site in [slo, shi]. */
static int in_reach(uintptr_t lo, uintptr_t hi, uintptr_t slo, uintptr_t shi)
{
    uintptr_t lim = B_RANGE - B_MARGIN;
    if (lo >= shi)
        return hi - slo <= lim;
    if (hi <= slo)
        return shi - lo <= lim;
    return 0;
}

static void *try_map(uintptr_t hint, size_t size)
{
    void *p;
    if (hint < 0x10000)
        return NULL;
    p = mmap((void *)hint, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    if ((uintptr_t)p != hint) {
        /* kernel without MAP_FIXED_NOREPLACE treated it as a hint */
        munmap(p, size);
        return NULL;
    }
    return p;
}

#define MAX_CANDS 64

/*
 * Candidate start addresses from the gaps in /proc/self/maps, best first:
 * gaps below the image (closest first; nothing grows there), then gaps above
 * it (closest first). In a gap right after the image or [heap] (where brk
 * grows) the region goes to the top end of the reachable part instead.
 */
static unsigned maps_candidates(size_t size, size_t tramp_size, uintptr_t slo, uintptr_t shi,
                                uintptr_t *out)
{
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t below[MAX_CANDS], above[MAX_CANDS], prev_hi = 0x10000;
    unsigned nb = 0, na = 0, n = 0;
    int prev_grows = 0;
    char line[512];
    FILE *f = fopen("/proc/self/maps", "r");

    if (!f)
        return 0;
    for (int last = 0; !last;) {
        unsigned long lo, hi;
        int grows;
        if (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%lx-%lx", &lo, &hi) != 2)
                continue;
            grows = strstr(line, "[heap]") != NULL || hi == g_exe.hi;
        } else {
            lo = hi = UINTPTR_MAX & ~(page - 1);
            grows = 0;
            last = 1;
        }
        if (lo > prev_hi && lo - prev_hi >= size) {
            uintptr_t c;
            if (lo <= g_exe.lo) {
                c = (lo - size) & ~(page - 1);
                if (nb < MAX_CANDS && in_reach(c + size - tramp_size, c + size, slo, shi))
                    below[nb++] = c;
            } else if (prev_hi >= g_exe.hi) {
                if (prev_grows) {
                    uintptr_t top = slo + (B_RANGE - B_MARGIN);
                    c = ((lo < top ? lo : top) - size) & ~(page - 1);
                } else {
                    c = prev_hi;
                }
                if (na < MAX_CANDS && c >= prev_hi && c + size <= lo &&
                    in_reach(c + size - tramp_size, c + size, slo, shi))
                    above[na++] = c;
            }
        }
        if (hi > prev_hi)
            prev_hi = hi;
        prev_grows = grows;
    }
    fclose(f);
    for (unsigned i = nb; i-- > 0;)
        out[n++] = below[i];
    for (unsigned i = 0; i < na; i++)
        out[n++] = above[i];
    return n;
}

/*
 * Map `size` bytes so that the last `tramp_size` bytes are within reach of
 * every site. Free gaps from /proc/self/maps first; blind hints (below the
 * image, then above it from the far end down) if that finds nothing.
 */
static void *map_near(size_t size, size_t tramp_size, uintptr_t slo, uintptr_t shi)
{
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t cand[2 * MAX_CANDS], hint, top;
    unsigned nc = maps_candidates(size, tramp_size, slo, shi, cand);

    for (unsigned i = 0; i < nc; i++) {
        void *p = try_map(cand[i], size);
        if (p)
            return p;
    }
    GV_DEBUG("no usable gap in /proc/self/maps (%u candidates), trying hints", nc);
    for (unsigned k = 0; k < HINT_TRIES; k++) {
        if (g_exe.lo < size + (uintptr_t)k * HINT_STEP)
            break;
        hint = (g_exe.lo - size - (uintptr_t)k * HINT_STEP) & ~(page - 1);
        if (!in_reach(hint + size - tramp_size, hint + size, slo, shi))
            break;
        void *p = try_map(hint, size);
        if (p)
            return p;
    }
    top = slo + (B_RANGE - B_MARGIN) - size;
    top &= ~(HINT_STEP - 1);
    for (unsigned k = 0; k < HINT_TRIES; k++) {
        hint = top - (uintptr_t)k * HINT_STEP;
        if (hint < g_exe.hi)
            break;
        if (!in_reach(hint + size - tramp_size, hint + size, slo, shi))
            continue;
        void *p = try_map(hint, size);
        if (p)
            return p;
    }
    return NULL;
}

/* ---- apply ----------------------------------------------------------- */

static int check_sites(const struct gv_descriptor *d, unsigned rows)
{
    for (unsigned i = 0; i < d->n_sites; i++) {
        const struct gv_site *s = &d->sites[i];
        uintptr_t a = g_exe.base + s->vaddr;
        uint32_t w;

        if ((a & 3) || !exe_seg(a, 4, PF_R | PF_X)) {
            GV_ERROR("site %s %#lx is not in the executable's code",
                     kind_name(s->kind), (unsigned long)s->vaddr);
            return -1;
        }
        for (unsigned j = 0; j < i; j++) {
            if (d->sites[j].vaddr == s->vaddr) {
                GV_ERROR("duplicate site %#lx", (unsigned long)s->vaddr);
                return -1;
            }
        }
        w = *(const volatile uint32_t *)a;
        if (w != s->orig_insn) {
            GV_ERROR("site %s %#lx: found %08x, expected %08x", kind_name(s->kind),
                     (unsigned long)s->vaddr, w, s->orig_insn);
            return -1;
        }
        if (is_tramp_site(s)) {
            if (!IS_ADD_IMM64(w) || (int)(w & 0x1f) != s->xd || s->xd == 31) {
                GV_ERROR("site %s %#lx: %08x is not add x%d, xN, #imm",
                         kind_name(s->kind), (unsigned long)s->vaddr, w, s->xd);
                return -1;
            }
        } else {
            if (!IS_CMP_IMM(w) || IMM12(w) != bound_value(s, d->table_rows)) {
                GV_ERROR("site %s %#lx: %08x is not cmp rN, #%u", kind_name(s->kind),
                         (unsigned long)s->vaddr, w, bound_value(s, d->table_rows));
                return -1;
            }
            if (bound_value(s, rows) > 0xfff)
                return -1;
        }
    }
    return 0;
}

/* rows must be the stock rows (same pointers, same order) plus appends. */
static int check_rows(const struct gv_descriptor *d, const struct gv_rows *rows)
{
    if (rows->n < d->table_rows || rows->n > d->max_vendors || rows->n > GV_MAX_ROWS_CAP ||
        rows->n_stock != d->table_rows) {
        GV_ERROR("bad row count %u (stock %u, cap %u)", rows->n, d->table_rows, d->max_vendors);
        return -1;
    }
    for (unsigned r = 0; r < rows->n; r++) {
        const struct gv_row *row = &rows->v[r];
        if (!row->vendor || row->n_entries > d->max_entries) {
            GV_ERROR("bad row %u", r);
            return -1;
        }
        for (unsigned i = 0; i < row->n_entries; i++) {
            if (!row->entries[i] || !row->entries[i][0]) {
                GV_ERROR("row %u entry %u is empty", r, i);
                return -1;
            }
        }
        if (r < d->table_rows) {
            const char *const *srow = stock_row(d, r);
            unsigned sn = stock_count(d, r);
            if (row->vendor != srow[0] || row->n_entries < sn) {
                GV_ERROR("row %u does not start with the stock row", r);
                return -1;
            }
            for (unsigned i = 0; i < sn; i++) {
                if (row->entries[i] != srow[1 + i]) {
                    GV_ERROR("row %u entry %u differs from stock", r, i);
                    return -1;
                }
            }
        } else if (!row->n_entries) {
            GV_ERROR("new row %u has no entries", r);
            return -1;
        }
    }
    return 0;
}

/* Size of the strings that must be copied into the region. */
static size_t pool_size(const struct gv_rows *rows)
{
    size_t n = 0;
    for (unsigned r = 0; r < rows->n; r++) {
        if (!in_image(rows->v[r].vendor))
            n += strlen(rows->v[r].vendor) + 1;
        for (unsigned i = 0; i < rows->v[r].n_entries; i++)
            if (!in_image(rows->v[r].entries[i]))
                n += strlen(rows->v[r].entries[i]) + 1;
    }
    return n;
}

static const char *pool_str(char **pool, const char *s)
{
    size_t n;
    char *p;
    if (in_image(s))
        return s;
    n = strlen(s) + 1;
    p = *pool;
    memcpy(p, s, n);
    *pool += n;
    return p;
}

static void fill_table(const struct gv_descriptor *d, const struct gv_rows *rows,
                       char *table, char *pool)
{
    for (unsigned r = 0; r < rows->n; r++) {
        const char **slot = (const char **)(table + (size_t)r * d->table_stride);
        const struct gv_row *row = &rows->v[r];
        unsigned first = 0;

        if (r < d->table_rows) {
            memcpy(slot, stock_row(d, r), d->table_stride);    /* verbatim */
            first = stock_count(d, r);
        } else {
            slot[0] = pool_str(&pool, row->vendor);
        }
        for (unsigned i = first; i < row->n_entries; i++)
            slot[1 + i] = pool_str(&pool, row->entries[i]);
        if (row->n_entries < d->max_entries)
            slot[1 + row->n_entries] = NULL;
    }
}

static void restore_text(uintptr_t *pages, unsigned n, size_t page)
{
    for (unsigned i = 0; i < n; i++)
        if (mprotect((void *)pages[i], page, PROT_READ | PROT_EXEC) < 0)
            GV_ERROR("mprotect R-X %#lx failed: %s", (unsigned long)pages[i], strerror(errno));
}

int apply_patches(const struct gv_descriptor *d, const struct gv_rows *rows)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint32_t words[GV_MAX_SITES];
    uintptr_t pages[GV_MAX_SITES];
    unsigned npages = 0, ntramp = 0, nrows = rows->n;
    uintptr_t slo = UINTPTR_MAX, shi = 0;
    size_t table_size, buttons_size, data_size, tramp_size, size;
    unsigned button_slots;
    char *region, *table, *pool;
    uint32_t *tramp;

    memset(&g_patch, 0, sizeof(g_patch));
    if (exe_init() < 0)
        return -1;

    /* 1. verify everything; nothing is written before this passes */
    if (check_rows(d, rows) < 0 || check_sites(d, nrows) < 0)
        return -1;
    if (!exe_seg(g_exe.base + d->table_vaddr, (size_t)d->table_rows * d->table_stride, PF_R) ||
        !exe_seg(g_exe.base + d->buttons_vaddr, (size_t)d->button_slots * 8, PF_R | PF_W)) {
        GV_ERROR("table/buttons not inside the executable");
        return -1;
    }
    for (unsigned i = 0; i < d->n_sites; i++) {
        uintptr_t a = g_exe.base + d->sites[i].vaddr;
        if (a < slo)
            slo = a;
        if (a > shi)
            shi = a;
        if (is_tramp_site(&d->sites[i]))
            ntramp++;
    }

    /* 2. region: table | buttons | strings | (page) trampolines */
    button_slots = d->max_vendors > d->button_slots ? d->max_vendors : d->button_slots;
    if (button_slots < nrows)
        button_slots = nrows;
    table_size = (size_t)nrows * d->table_stride;
    buttons_size = (size_t)button_slots * 8;
    data_size = (table_size + buttons_size + pool_size(rows) + page - 1) & ~(page - 1);
    tramp_size = ((size_t)ntramp * TRAMP_INSNS * 4 + page - 1) & ~(page - 1);
    size = data_size + tramp_size;
    region = map_near(size, tramp_size, slo, shi + 4);
    if (!region) {
        GV_ERROR("no free address within 128 MB of the GUI code for trampolines");
        if (gv_debug) {
            char line[512];
            FILE *f = fopen("/proc/self/maps", "r");
            while (f && fgets(line, sizeof(line), f))
                GV_DEBUG("maps: %s", strtok(line, "\n"));
            if (f)
                fclose(f);
        }
        return -1;
    }
    table = region;
    pool = region + table_size + buttons_size;
    tramp = (uint32_t *)(region + data_size);
    GV_DEBUG("region %p (%zu bytes), trampolines %p", (void *)region, size, (void *)tramp);

    fill_table(d, rows, table, pool);
    /* button array: mmap'd zero */

    for (unsigned i = 0, t = 0; i < d->n_sites; i++) {
        const struct gv_site *s = &d->sites[i];
        uintptr_t a = g_exe.base + s->vaddr;

        if (is_tramp_site(s)) {
            uint32_t *tp = tramp + t * TRAMP_INSNS;
            uintptr_t target = (uintptr_t)(s->kind == GV_SITE_TABLE ? table : table + table_size);
            enc_mov64(tp, s->xd, target);
            if (enc_b((uintptr_t)&tp[4], a + 4, &tp[4]) < 0 ||
                enc_b(a, (uintptr_t)tp, &words[i]) < 0) {
                GV_ERROR("trampoline for %#lx out of branch range", (unsigned long)s->vaddr);
                munmap(region, size);
                return -1;
            }
            g_patch.site_tramp[i] = (uintptr_t)tp;
            t++;
        } else {
            words[i] = SET_IMM12(s->orig_insn, bound_value(s, nrows));
            g_patch.site_tramp[i] = 0;
        }
    }
    __builtin___clear_cache((char *)tramp, (char *)tramp + tramp_size);
    if (mprotect(tramp, tramp_size, PROT_READ | PROT_EXEC) < 0) {
        GV_ERROR("mprotect trampolines R-X failed: %s", strerror(errno));
        munmap(region, size);
        return -1;
    }

    /* 3. text: all pages writable first, then write, then back to R-X */
    for (unsigned i = 0; i < d->n_sites; i++) {
        uintptr_t pg = (g_exe.base + d->sites[i].vaddr) & ~(uintptr_t)(page - 1);
        unsigned j;
        for (j = 0; j < npages && pages[j] != pg; j++)
            ;
        if (j < npages)
            continue;
        if (mprotect((void *)pg, page, PROT_READ | PROT_WRITE) < 0) {
            GV_ERROR("mprotect RW %#lx failed: %s", (unsigned long)pg, strerror(errno));
            restore_text(pages, npages, page);
            munmap(region, size);
            return -1;
        }
        pages[npages++] = pg;
    }
    for (unsigned i = 0; i < d->n_sites; i++) {
        uintptr_t a = g_exe.base + d->sites[i].vaddr;
        *(volatile uint32_t *)a = words[i];
        __builtin___clear_cache((char *)a, (char *)a + 4);
        g_patch.site_word[i] = words[i];
    }
    restore_text(pages, npages, page);

    g_patch.applied = 1;
    g_patch.region = (uintptr_t)region;
    g_patch.region_size = size;
    g_patch.tramp = (uintptr_t)tramp;
    g_patch.table = (uintptr_t)table;
    g_patch.buttons = (uintptr_t)(table + table_size);
    g_patch.button_slots = button_slots;
    return 0;
}

/* ---- self-test ------------------------------------------------------- */

static int st_fail;

#define ST_CHECK(cond, ...) do { if (!(cond)) { st_fail++; \
    fprintf(stderr, "selftest: FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* Permissions of the mapping containing addr from /proc/self/maps. */
static int maps_perms(uintptr_t addr, char perms[5])
{
    char line[512];
    FILE *f = fopen("/proc/self/maps", "r");
    int found = -1;
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) == 3 && addr >= lo && addr < hi) {
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

/*
 * Compare every executable segment with the file on disk. Returns the
 * number of differing words; their addresses go to diffs[0..max).
 */
static int text_diff(const char *exe, uintptr_t *diffs, unsigned max)
{
    FILE *f = fopen(exe, "rb");
    static uint8_t buf[1 << 16];
    int n = 0;

    if (!f)
        return -1;
    for (unsigned i = 0; i < g_exe.nseg; i++) {
        const struct seg *s = &g_exe.seg[i];
        if (!(s->flags & PF_X))
            continue;
        if (fseek(f, (long)s->off, SEEK_SET) < 0) {
            fclose(f);
            return -1;
        }
        for (uint64_t pos = 0; pos < s->filesz;) {
            size_t want = s->filesz - pos > sizeof(buf) ? sizeof(buf) : (size_t)(s->filesz - pos);
            if (fread(buf, 1, want, f) != want) {
                fclose(f);
                return -1;
            }
            const uint8_t *mem = (const uint8_t *)(s->lo + pos);
            if (memcmp(mem, buf, want)) {
                for (size_t k = 0; k + 4 <= want; k += 4) {
                    if (memcmp(mem + k, buf + k, 4)) {
                        if ((unsigned)n < max)
                            diffs[n] = s->lo + pos + k;
                        n++;
                    }
                }
            }
            pos += want;
        }
    }
    fclose(f);
    return n;
}

static void st_print_rows(const struct gv_descriptor *d, uintptr_t table, unsigned n)
{
    for (unsigned r = 0; r < n; r++) {
        const char *const *slot = (const char *const *)(table + (size_t)r * d->table_stride);
        fprintf(stderr, "selftest:   row %u%s %-12s:", r, r < d->table_rows ? " " : "+", slot[0]);
        for (unsigned i = 1; i <= d->max_entries && slot[i]; i++)
            fprintf(stderr, " %s", slot[i]);
        fputc('\n', stderr);
    }
}

static void st_check_table(const struct gv_descriptor *d, const struct gv_rows *rows)
{
    for (unsigned r = 0; r < rows->n; r++) {
        const char *const *slot = (const char *const *)(g_patch.table + (size_t)r * d->table_stride);
        const struct gv_row *row = &rows->v[r];

        ST_CHECK(slot[0] && !strcmp(slot[0], row->vendor), "row %u vendor", r);
        if (r < d->table_rows) {
            const char *const *srow = stock_row(d, r);
            unsigned sn = stock_count(d, r);
            ST_CHECK(slot[0] == srow[0], "row %u vendor pointer is not stock", r);
            for (unsigned i = 0; i < sn; i++)
                ST_CHECK(slot[1 + i] == srow[1 + i], "row %u entry %u pointer is not stock", r, i);
        }
        for (unsigned i = 0; i < row->n_entries; i++)
            ST_CHECK(slot[1 + i] && !strcmp(slot[1 + i], row->entries[i]),
                     "row %u entry %u: '%s' != '%s'", r, i, slot[1 + i] ? slot[1 + i] : "(null)",
                     row->entries[i]);
        if (row->n_entries < d->max_entries)
            ST_CHECK(!slot[1 + row->n_entries], "row %u not NULL-terminated", r);
        for (unsigned i = 0; i <= row->n_entries; i++) {
            const char *p = slot[i];
            ST_CHECK(in_image(p) || (uintptr_t)p - g_patch.region < g_patch.region_size,
                     "row %u slot %u points outside the image and the region", r, i);
        }
    }
    for (unsigned i = 0; i < g_patch.button_slots; i++)
        ST_CHECK(!((const uintptr_t *)g_patch.buttons)[i], "button slot %u not zero", i);
}

static void st_check_sites(const struct gv_descriptor *d, unsigned nrows, int64_t *dmin, int64_t *dmax)
{
    char perms[5];
    *dmin = INT64_MAX;
    *dmax = INT64_MIN;
    for (unsigned i = 0; i < d->n_sites; i++) {
        const struct gv_site *s = &d->sites[i];
        uintptr_t a = g_exe.base + s->vaddr;
        uint32_t w = *(const uint32_t *)a;

        if (is_tramp_site(s)) {
            uintptr_t t, back = 0;
            uint64_t v;
            int rd;
            uintptr_t want = s->kind == GV_SITE_TABLE ? g_patch.table : g_patch.buttons;
            if (dec_b(w, a, &t) < 0) {
                ST_CHECK(0, "%#lx: %08x is not b", (unsigned long)s->vaddr, w);
                continue;
            }
            ST_CHECK(t >= g_patch.tramp && t < g_patch.region + g_patch.region_size,
                     "%#lx: b target %#lx outside trampolines", (unsigned long)s->vaddr, (unsigned long)t);
            if (t < g_patch.tramp || t >= g_patch.region + g_patch.region_size)
                continue;
            const uint32_t *tp = (const uint32_t *)t;
            ST_CHECK(!dec_mov64(tp, &rd, &v), "%#lx: trampoline is not movz/movk", (unsigned long)s->vaddr);
            ST_CHECK(rd == s->xd, "%#lx: trampoline writes x%d, want x%d", (unsigned long)s->vaddr, rd, s->xd);
            ST_CHECK(v == want, "%#lx: trampoline loads %#lx, want %#lx", (unsigned long)s->vaddr,
                     (unsigned long)v, (unsigned long)want);
            ST_CHECK(!dec_b(tp[4], (uintptr_t)&tp[4], &back) && back == a + 4,
                     "%#lx: trampoline does not return to site+4", (unsigned long)s->vaddr);
            int64_t dist = (int64_t)(t - a);
            if (dist < *dmin)
                *dmin = dist;
            if (dist > *dmax)
                *dmax = dist;
            fprintf(stderr, "selftest:   %-12s %#lx: b %+lld -> movz/movk x%d, #%#lx; b %#lx\n",
                    kind_name(s->kind), (unsigned long)s->vaddr, (long long)dist, rd,
                    (unsigned long)v, (unsigned long)(back - g_exe.base));
        } else {
            ST_CHECK((w & ~(0xfffu << 10)) == (s->orig_insn & ~(0xfffu << 10)),
                     "%#lx: cmp register/encoding changed (%08x)", (unsigned long)s->vaddr, w);
            ST_CHECK(IMM12(w) == bound_value(s, nrows), "%#lx: cmp #%u, want #%u",
                     (unsigned long)s->vaddr, IMM12(w), bound_value(s, nrows));
            fprintf(stderr, "selftest:   %-12s %#lx: %08x -> %08x (cmp #%u -> #%u)\n",
                    kind_name(s->kind), (unsigned long)s->vaddr, s->orig_insn, w,
                    IMM12(s->orig_insn), IMM12(w));
        }
        ST_CHECK(!maps_perms(a, perms) && !strcmp(perms, "r-xp"),
                 "%#lx: text page is not r-xp", (unsigned long)s->vaddr);
    }
    ST_CHECK(!maps_perms(g_patch.tramp, perms) && !strcmp(perms, "r-xp"), "trampolines not r-xp");
    ST_CHECK(!maps_perms(g_patch.table, perms) && !strcmp(perms, "rw-p"), "table not rw-p");
}

int gv_selftest(const struct gv_descriptor *d, const struct gv_rows *rows, const char *exe,
                double total_ms, double sha_ms)
{
    uintptr_t diffs[GV_MAX_SITES + 1];
    int nd;

    st_fail = 0;
    if (exe_init() < 0)
        return 1;
    fprintf(stderr, "selftest: gui base %#lx, image %#lx-%#lx\n", (unsigned long)g_exe.base,
            (unsigned long)g_exe.lo, (unsigned long)g_exe.hi);
    fprintf(stderr, "selftest: constructor %.1f ms (sha256 %.1f ms)\n", total_ms, sha_ms);

    nd = text_diff(exe, diffs, GV_MAX_SITES + 1);
    ST_CHECK(nd >= 0, "cannot compare text with %s", exe);

    if (!g_patch.applied || !d || !rows) {
        fprintf(stderr, "selftest: NOT APPLIED; text words differing from %s: %d\n", exe, nd);
        ST_CHECK(nd == 0, "text modified although the patch was not applied");
        fprintf(stderr, "selftest: %s\n", st_fail ? "FAILED" : "OK (GUI untouched)");
        return st_fail ? 1 : 2;
    }

    fprintf(stderr, "selftest: APPLIED: %u rows (%u stock, %u new vendors, %u entries appended)\n",
            rows->n, rows->n_stock, rows->n_new_vendors, rows->n_added_entries);
    fprintf(stderr, "selftest: region %#lx-%#lx: table %#lx, buttons %#lx (%u slots), trampolines %#lx\n",
            (unsigned long)g_patch.region, (unsigned long)(g_patch.region + g_patch.region_size),
            (unsigned long)g_patch.table, (unsigned long)g_patch.buttons, g_patch.button_slots,
            (unsigned long)g_patch.tramp);
    st_print_rows(d, g_patch.table, rows->n);
    st_check_table(d, rows);

    int64_t dmin, dmax;
    st_check_sites(d, rows->n, &dmin, &dmax);
    fprintf(stderr, "selftest: trampoline distance %+lld..%+lld bytes (limit +-%u)\n",
            (long long)dmin, (long long)dmax, B_RANGE);

    /* only the sites may differ from the file */
    fprintf(stderr, "selftest: text words differing from %s: %d\n", exe, nd);
    for (int i = 0; i < nd && i <= GV_MAX_SITES; i++) {
        int is_site = 0;
        for (unsigned k = 0; k < d->n_sites; k++)
            if (g_exe.base + d->sites[k].vaddr == diffs[i])
                is_site = 1;
        ST_CHECK(is_site, "unexpected text change at %#lx", (unsigned long)(diffs[i] - g_exe.base));
    }
    ST_CHECK(nd <= (int)d->n_sites, "too many text changes");
    fprintf(stderr, "selftest: %s\n", st_fail ? "FAILED" : "OK");
    return st_fail ? 1 : 0;
}
