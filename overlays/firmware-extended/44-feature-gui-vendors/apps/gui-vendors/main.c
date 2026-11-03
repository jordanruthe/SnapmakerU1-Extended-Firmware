// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

// LD_PRELOAD library that adds user-defined filament vendors/types to the
// stock Snapmaker touchscreen GUI (/usr/bin/gui). Does nothing in any other
// process, and leaves the GUI untouched on any failure.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gui_vendors.h"

#define GUI_PATH "/usr/bin/gui"
#define DEFAULT_DESCRIPTOR "/usr/local/share/gui-vendors/gui.patch"
#define DEFAULT_LIBRARY "/home/lava/printer_data/config/extended/filament_library.json"

static const char *env_or(const char *name, const char *def)
{
    const char *v = getenv(name);
    return (v && *v) ? v : def;
}

/* GUI_VENDORS_EXE (test-only) replaces /usr/bin/gui, e.g. under qemu-user. */
static int is_gui_process(void)
{
    char self_path[256];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len < 0)
        return 0;
    self_path[len] = '\0';
    return strcmp(self_path, env_or("GUI_VENDORS_EXE", GUI_PATH)) == 0;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static struct gv_descriptor desc;
static struct gv_rows stock, rows;
static double sha_ms;

/* Returns 1 if the GUI was patched, 0 if it was left untouched. */
static int patch_gui(int *have_desc)
{
    char err[256] = "";
    struct gv_library lib = { 0 };
    double t;

    const char *dpath = env_or("GUI_VENDORS_DESCRIPTOR", DEFAULT_DESCRIPTOR);
    GV_DEBUG("loading descriptor %s", dpath);
    if (gv_descriptor_load(dpath, &desc, err, sizeof(err)) < 0) {
        GV_ERROR("descriptor: %s (GUI left unpatched)", err);
        return 0;
    }
    t = now_ms();
    if (gv_descriptor_verify(&desc, "/proc/self/exe", err, sizeof(err)) < 0) {
        GV_ERROR("descriptor does not match this GUI: %s (GUI left unpatched)", err);
        return 0;
    }
    sha_ms = now_ms() - t;
    *have_desc = 1;

    const char *lpath = env_or("GUI_VENDORS_LIBRARY", DEFAULT_LIBRARY);
    GV_DEBUG("loading library %s", lpath);
    if (gv_library_load(lpath, &lib, err, sizeof(err)) < 0) {
        GV_ERROR("library: %s (GUI left unpatched)", err);
        return 0;
    }
    GV_DEBUG("library has %zu usable entries", lib.n);

    if (gv_read_stock_rows(&desc, &stock) < 0) {
        GV_ERROR("cannot read stock vendor table (GUI left unpatched)");
        goto out;
    }
    if (gv_build_rows(&desc, &lib, &stock, &rows) < 0) {
        GV_ERROR("cannot build vendor rows (GUI left unpatched)");
        goto out;
    }

    /* Nothing to add: leave the GUI byte-for-byte stock, unless forced. */
    if (!rows.n_added_entries && !rows.n_new_vendors &&
        !atoi(env_or("GUI_VENDORS_FORCE_APPLY", "0"))) {
        GV_DEBUG("library adds nothing, not patching");
        goto out;
    }

    GV_DEBUG("%u vendors (%u new), %u entries appended to stock vendors",
             rows.n, rows.n_new_vendors, rows.n_added_entries);
    if (apply_patches(&desc, &rows) < 0) {
        GV_ERROR("apply_patches failed (GUI left unpatched)");
        goto out;
    }
    GV_INFO("added %u vendors and %u entries to the GUI", rows.n_new_vendors,
            rows.n_added_entries);
    /* apply_patches copied what it needs; rows stay for the self-test */
    gv_library_free(&lib);
    return 1;

out:
    gv_rows_free(&rows);
    gv_library_free(&lib);
    return 0;
}

__attribute__((constructor))
static void gui_vendors_init(void)
{
    if (!is_gui_process())
        return;

    const char *env = getenv("GUI_VENDORS_DEBUG");
    if (env && atoi(env))
        gv_debug = 1;

    int have_desc = 0;
    double t0 = now_ms();
    int applied = patch_gui(&have_desc);
    double total = now_ms() - t0;
    GV_DEBUG("constructor took %.1f ms (sha256 %.1f ms)", total, sha_ms);

    /* Test-only: verify the patched process and exit before the GUI's main(). */
    if (atoi(env_or("GUI_VENDORS_SELFTEST", "0"))) {
        int rc = gv_selftest(have_desc ? &desc : NULL, applied ? &rows : NULL,
                             "/proc/self/exe", total, sha_ms);
        fflush(stderr);
        _exit(rc);
    }
}
