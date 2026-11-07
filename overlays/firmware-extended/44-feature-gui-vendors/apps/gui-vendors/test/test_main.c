// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gui_vendors.h"

static int g_fail, g_run;

#define CHECK(cond) do { g_run++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { g_run++; const char *_a = (a), *_b = (b); \
    if (!_a || strcmp(_a, _b)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a ? _a : "(null)", _b); } } while (0)

#define SHA "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static const char *VALID =
    "# generated\n"
    "gui_sha256 " SHA "\n"
    "table 0x69b520 rows 3 stride 0xc0 max_entries 23\n"
    "buttons 0x26ecdb0 slots 3\n"
    "max_vendors 8   # cap\n"
    "\n"
    "site table 0x1520fc 0x91148000 x0\n"
    "site table 0x15215c 91148001 x1\n"
    "site buttons 0x152870 0x9101 22\n"
    "site bound_count 0x15289c 0xf1000e7f\n"
    "site bound_maxidx 0x152970 0x7100089f\n";

/* decimal counts, bare hex addresses */
static const char *VALID_DEC =
    "gui_sha256 " SHA "\n"
    "table 69b520 rows 3 stride 192 max_entries 23\n"
    "buttons 26ecdb0 slots 3\n"
    "max_vendors 8\n"
    "site table 1520fc 91148000 x0\n"
    "site buttons 152870 9101 x22\n"
    "site bound_count 15289c f1000e7f\n"
    "site bound_maxidx 152970 7100089f\n";

static void test_sha256(void)
{
    char hex[65];
    gv_sha256_buf("abc", 3, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    gv_sha256_buf("", 0, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    char big[1000];
    memset(big, 'a', sizeof(big));
    gv_sha256_buf(big, sizeof(big), hex);
    CHECK_STR(hex, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

static void test_descriptor(void)
{
    struct gv_descriptor d;
    char err[128];

    /* 0x-prefixed numbers are accepted everywhere */
    CHECK(gv_descriptor_parse(VALID, &d, err, sizeof(err)) == 0);
    CHECK(d.table_stride == 192 && d.sites[3].xd == -1 && d.sites[2].xd == 22);
    CHECK(gv_descriptor_parse(VALID_DEC, &d, err, sizeof(err)) == 0);
    CHECK_STR(d.gui_sha256, SHA);
    CHECK(d.table_vaddr == 0x69b520 && d.table_rows == 3 && d.table_stride == 192);
    CHECK(d.max_entries == 23 && d.buttons_vaddr == 0x26ecdb0 && d.button_slots == 3);
    CHECK(d.max_vendors == 8 && d.n_sites == 4);
    CHECK(d.sites[0].kind == GV_SITE_TABLE && d.sites[0].vaddr == 0x1520fc &&
          d.sites[0].orig_insn == 0x91148000 && d.sites[0].xd == 0);
    CHECK(d.sites[1].kind == GV_SITE_BUTTONS && d.sites[1].xd == 22);
    CHECK(d.sites[2].kind == GV_SITE_BOUND_COUNT && d.sites[2].xd == -1 &&
          d.sites[2].orig_insn == 0xf1000e7f);
    CHECK(d.sites[3].kind == GV_SITE_BOUND_MAXIDX);

    /* comment-only input has no records */
    CHECK(gv_descriptor_parse("# c\r\n\r\n", &d, err, sizeof(err)) < 0);

    /* malformed variants */
    static const char *bad[] = {
        "",
        "gui_sha256 abc\n",
        "bogus 1 2 3\n",
        /* missing sites */
        "gui_sha256 " SHA "\ntable 1 rows 3 stride 192 max_entries 23\nbuttons 2 slots 3\nmax_vendors 8\n",
        /* missing max_vendors */
        "gui_sha256 " SHA "\ntable 1 rows 3 stride 192 max_entries 23\nbuttons 2 slots 3\n"
        "site table 1 91148000 x0\nsite buttons 1 9101 x1\nsite bound_count 1 1\nsite bound_maxidx 1 1\n",
        /* max_vendors below stock rows */
        "gui_sha256 " SHA "\ntable 1 rows 3 stride 192 max_entries 23\nbuttons 2 slots 3\nmax_vendors 2\n"
        "site table 1 91148000 x0\nsite buttons 1 9101 x1\nsite bound_count 1 1\nsite bound_maxidx 1 1\n",
        /* entries do not fit stride */
        "gui_sha256 " SHA "\ntable 1 rows 3 stride 64 max_entries 23\nbuttons 2 slots 3\nmax_vendors 8\n"
        "site table 1 91148000 x0\nsite buttons 1 9101 x1\nsite bound_count 1 1\nsite bound_maxidx 1 1\n",
        /* unknown site kind */
        "gui_sha256 " SHA "\ntable 1 rows 3 stride 192 max_entries 23\nbuttons 2 slots 3\nmax_vendors 8\n"
        "site frob 1 91148000\n",
        /* table site without register */
        "gui_sha256 " SHA "\ntable 1 rows 3 stride 192 max_entries 23\nbuttons 2 slots 3\nmax_vendors 8\n"
        "site table 1 91148000\n",
        /* garbage hex */
        "gui_sha256 " SHA "\ntable zz rows 3 stride 192 max_entries 23\n",
        /* sha with non-hex chars */
        "gui_sha256 zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err[0] = 0;
        CHECK(gv_descriptor_parse(bad[i], &d, err, sizeof(err)) < 0);
        CHECK(err[0] != 0);
    }
}

static const char *tmpfile_with(const char *tmpl_out, const char *content)
{
    FILE *f = fopen(tmpl_out, "wb");
    if (!f) { perror("fopen"); exit(2); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return tmpl_out;
}

static void test_descriptor_verify(void)
{
    struct gv_descriptor d;
    char err[256], hex[65], path[] = "/tmp/gv-test-exe-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello\n", 6) == 6);
    close(fd);

    CHECK(gv_sha256_file(path, hex) == 0);
    CHECK_STR(hex, "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03");

    CHECK(gv_descriptor_parse(VALID_DEC, &d, err, sizeof(err)) == 0);
    CHECK(gv_descriptor_verify(&d, path, err, sizeof(err)) < 0);   /* wrong sha */
    CHECK(strstr(err, "mismatch") != NULL);
    memcpy(d.gui_sha256, hex, 65);
    CHECK(gv_descriptor_verify(&d, path, err, sizeof(err)) == 0);
    CHECK(gv_descriptor_verify(&d, "/nonexistent/gui", err, sizeof(err)) < 0);
    unlink(path);

    /* load from file */
    char dpath[] = "/tmp/gv-test-desc-XXXXXX";
    fd = mkstemp(dpath);
    close(fd);
    tmpfile_with(dpath, VALID_DEC);
    CHECK(gv_descriptor_load(dpath, &d, err, sizeof(err)) == 0);
    /* padded with a comment to exactly 65535 bytes: accepted; one more: rejected */
    {
        size_t base = strlen(VALID_DEC);
        char *big = malloc(65537);
        CHECK(big != NULL && base + 2 < 65535);
        if (big) {
            memcpy(big, VALID_DEC, base);
            big[base] = '#';
            memset(big + base + 1, 'x', 65535 - base - 2);
            strcpy(big + 65535 - 1, "\n");
            tmpfile_with(dpath, big);
            CHECK(gv_descriptor_load(dpath, &d, err, sizeof(err)) == 0);
            strcpy(big + 65535 - 1, "x\n");
            tmpfile_with(dpath, big);
            CHECK(gv_descriptor_load(dpath, &d, err, sizeof(err)) < 0);
            free(big);
        }
    }
    unlink(dpath);
    CHECK(gv_descriptor_load(dpath, &d, err, sizeof(err)) < 0);    /* missing */
}

static int lib_parse(const char *json, struct gv_library *lib)
{
    char err[128];
    return gv_library_parse(json, strlen(json), lib, err, sizeof(err));
}

static void test_library(void)
{
    struct gv_library lib;
    char err[128];

    CHECK(lib_parse("{\"version\":1,\"filaments\":[]}", &lib) == 0 && lib.n == 0);
    gv_library_free(&lib);

    CHECK(lib_parse("{\"version\":1}", &lib) == 0 && lib.n == 0);   /* no filaments key */
    gv_library_free(&lib);

    /* brand_order, before or after filaments; non-strings and blanks skipped */
    CHECK(lib_parse("{\"brand_order\":[\" Overture \",3,\"\",{\"a\":1},\"eSun\"],"
                    "\"filaments\":[{\"vendor\":\"eSun\",\"type\":\"PLA\"}]}", &lib) == 0);
    CHECK(lib.n == 1 && lib.n_order == 2);
    if (lib.n_order == 2) {
        CHECK_STR(lib.brand_order[0], "Overture");
        CHECK_STR(lib.brand_order[1], "eSun");
    }
    gv_library_free(&lib);
    CHECK(lib_parse("{\"filaments\":[],\"brand_order\":[\"A\"]}", &lib) == 0 && lib.n_order == 1);
    gv_library_free(&lib);
    CHECK(lib_parse("{\"brand_order\":\"A\",\"filaments\":[{\"type\":\"PLA\"}]}", &lib) == 0 &&
          lib.n == 1 && lib.n_order == 0);                              /* not an array: ignored */
    gv_library_free(&lib);

    CHECK(lib_parse("{not json", &lib) < 0 && lib.n == 0);
    CHECK(lib_parse("", &lib) < 0);
    CHECK(lib_parse("[1,2]", &lib) < 0);
    CHECK(lib_parse("{\"filaments\": 3}", &lib) < 0);
    CHECK(lib_parse("{\"filaments\": [", &lib) < 0);

    /* missing file / empty file -> empty library, success */
    CHECK(gv_library_load("/nonexistent/filament_library.json", &lib, err, sizeof(err)) == 0 && lib.n == 0);
    char path[] = "/tmp/gv-test-lib-XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    CHECK(gv_library_load(path, &lib, err, sizeof(err)) == 0 && lib.n == 0);  /* 0 bytes */
    tmpfile_with(path, " \n ");
    CHECK(gv_library_load(path, &lib, err, sizeof(err)) == 0 && lib.n == 0);
    tmpfile_with(path, "{oops");
    CHECK(gv_library_load(path, &lib, err, sizeof(err)) < 0);
    tmpfile_with(path, "{\"filaments\":[{\"vendor\":\"A\",\"type\":\"pla\"}]}");
    CHECK(gv_library_load(path, &lib, err, sizeof(err)) == 0 && lib.n == 1);
    gv_library_free(&lib);
    unlink(path);

    /* entries without type / bad shapes are skipped, others kept. Extra keys
     * (nested arrays/objects, numbers, bools) are ignored. */
    CHECK(lib_parse(
        "{\"version\":1,\"x\":{\"a\":[1,2,{\"b\":null}]},\"filaments\":["
        " {\"id\":\"1\",\"vendor\":\"Overture\",\"type\":\"PETG\",\"sub_type\":\"HF\","
        "  \"colors\":[\"808080\"],\"alpha\":255,\"is_soft\":false,\"load_temp\":240},"
        " {\"vendor\":\"NoType\"},"
        " {\"vendor\":\"EmptyType\",\"type\":\"\"},"
        " {\"type\":\"PLA\"},"
        " 42, \"str\", null, [],"
        " {\"vendor\":\"Num\",\"type\":5},"
        " {\"vendor\":\"Bad#Type\",\"type\":\"PL#A\"},"
        " {\"vendor\":\"Star\",\"type\":\"PLA\",\"sub_type\":\"a*b\"},"
        " {\"vendor\":\"  Padded  \",\"type\":\" abs \",\"sub_type\":\" Matte \"}"
        "]}", &lib) == 0);
    CHECK(lib.n == 3);
    if (lib.n == 3) {
        CHECK_STR(lib.entries[0].vendor, "Overture");
        CHECK_STR(lib.entries[0].type, "PETG");
        CHECK_STR(lib.entries[0].sub_type, "HF");
        CHECK_STR(lib.entries[1].vendor, "Generic");   /* {"type":"PLA"} */
        CHECK_STR(lib.entries[1].type, "PLA");
        CHECK_STR(lib.entries[2].vendor, "Padded");
        CHECK_STR(lib.entries[2].type, "abs");
        CHECK_STR(lib.entries[2].sub_type, "Matte");
    }
    gv_library_free(&lib);

    /* deep nesting is rejected up front: jsmn is quadratic in depth and
     * tok_span() recurses, either would hang or crash the GUI's constructor */
    {
        enum { DEPTH = 400000 };
        const char *head = "{\"x\":", *tail = ",\"filaments\":[{\"type\":\"PLA\",\"y\":[]}]}";
        char *js = malloc(strlen(head) + 2 * DEPTH + strlen(tail) + 1), *p = js;
        CHECK(js != NULL);
        if (js) {
            p += sprintf(p, "%s", head);
            memset(p, '[', DEPTH); p += DEPTH;
            memset(p, ']', DEPTH); p += DEPTH;
            strcpy(p, tail);
            CHECK(lib_parse(js, &lib) < 0 && lib.n == 0);
            gv_library_free(&lib);
            free(js);
        }
        /* brackets inside strings do not count */
        CHECK(lib_parse("{\"x\":[[[[\"]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]\"]]]],"
                        "\"filaments\":[{\"type\":\"PLA\"}]}", &lib) == 0 && lib.n == 1);
        gv_library_free(&lib);
    }

    /* unicode and escapes */
    CHECK(lib_parse(
        "{\"filaments\":[{\"vendor\":\"Pol\\u00ffmer\",\"type\":\"PLA\","
        "\"sub_type\":\"\\u00e9\\u20ac\\ud83d\\ude00 / \\\\\"},"
        " {\"vendor\":\"Caf\xc3\xa9\",\"type\":\"PETG\"},"
        " {\"vendor\":\"Bad\\ud800\",\"type\":\"PLA\"},"
        " {\"vendor\":\"Nul\\u0000x\",\"type\":\"PLA\"},"
        " {\"vendor\":\"Ctl\\nx\",\"type\":\"PLA\"}]}", &lib) == 0);
    CHECK(lib.n == 2);
    if (lib.n == 2) {
        CHECK_STR(lib.entries[0].vendor, "Pol\xc3\xbfmer");
        CHECK_STR(lib.entries[0].sub_type, "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80 / \\");
        CHECK_STR(lib.entries[1].vendor, "Caf\xc3\xa9");
    }
    gv_library_free(&lib);

    /* empty vendor -> Generic; characters the GUI cannot pass to Klipper
     * (unquoted VENDOR/FILAMENT_TYPE, quoted FILAMENT_SUBTYPE) */
    CHECK(lib_parse(
        "{\"filaments\":["
        " {\"vendor\":\"\",\"type\":\"PLA+\",\"sub_type\":\"Silk Blue\"},"
        " {\"vendor\":\"  \",\"type\":\"PETG\"},"
        " {\"vendor\":\"Over ture\",\"type\":\"PLA\"},"
        " {\"vendor\":\"Over\\tture\",\"type\":\"PLA\"},"
        " {\"vendor\":\"A;B\",\"type\":\"PLA\"},"
        " {\"vendor\":\"A'B\",\"type\":\"PLA\"},"
        " {\"vendor\":\"A\\\"B\",\"type\":\"PLA\"},"
        " {\"vendor\":\"A#B\",\"type\":\"PLA\"},"
        " {\"vendor\":\"A*B\",\"type\":\"PLA\"},"
        " {\"vendor\":\"V\",\"type\":\"PL A\"},"
        " {\"vendor\":\"V\",\"type\":\"PLA;\"},"
        " {\"vendor\":\"V\",\"type\":\"PLA'\"},"
        " {\"vendor\":\"V\",\"type\":\"PLA\",\"sub_type\":\"a;b\"},"
        " {\"vendor\":\"V\",\"type\":\"PLA\",\"sub_type\":\"it's\"},"
        " {\"vendor\":\"V\",\"type\":\"PLA\",\"sub_type\":\"\\\"q\\\"\"},"
        " {\"vendor\":\"V\",\"type\":\"PLA\",\"sub_type\":\"a#b\"},"
        " {\"vendor\":\"V\",\"type\":\"PLA\",\"sub_type\":\"0123456789012345678901234567890\"},"
        " {\"vendor\":\"V\",\"type\":\"PLA\",\"sub_type\":\"01234567890123456789012345678901\"},"
        " {\"vendor\":\"V\",\"type\":\"0123456789012345678901234567890X\"},"
        " {\"vendor\":\"Ok-Vendor_1.0\",\"type\":\"pla-cf\",\"sub_type\":\"High Speed (HS)\"}"
        "]}", &lib) == 0);
    CHECK(lib.n == 4);
    if (lib.n == 4) {
        CHECK_STR(lib.entries[0].vendor, "Generic");
        CHECK_STR(lib.entries[0].sub_type, "Silk Blue");
        CHECK_STR(lib.entries[1].vendor, "Generic");     /* whitespace-only vendor */
        CHECK_STR(lib.entries[1].type, "PETG");
        CHECK_STR(lib.entries[2].sub_type, "0123456789012345678901234567890");  /* 31 chars */
        CHECK_STR(lib.entries[3].vendor, "Ok-Vendor_1.0");
        CHECK_STR(lib.entries[3].sub_type, "High Speed (HS)");
    }
    gv_library_free(&lib);

    /* over-long fields are dropped, not truncated */
    char big[512];
    memset(big, 'v', 200);
    big[200] = 0;
    char js[1024];
    snprintf(js, sizeof(js), "{\"filaments\":[{\"vendor\":\"%s\",\"type\":\"PLA\"}]}", big);
    CHECK(lib_parse(js, &lib) == 0 && lib.n == 0);
    gv_library_free(&lib);
}

/* ---- rows ---- */

static struct gv_descriptor g_desc;

static void mk_desc(unsigned max_entries, unsigned max_vendors)
{
    memset(&g_desc, 0, sizeof(g_desc));
    g_desc.table_rows = 3;
    g_desc.table_stride = 192;
    g_desc.max_entries = max_entries;
    g_desc.max_vendors = max_vendors;
}

static void mk_stock(struct gv_rows *s)
{
    memset(s, 0, sizeof(*s));
    s->v[0] = (struct gv_row){ .vendor = "Snapmaker", .entries = { "PLA", "PLA#Matte", "PETG" }, .n_entries = 3 };
    s->v[1] = (struct gv_row){ .vendor = "Polymaker", .entries = { "PLA" }, .n_entries = 1 };
    s->v[2] = (struct gv_row){ .vendor = "Generic", .entries = { "PLA", "ABS" }, .n_entries = 2 };
    s->n = s->n_stock = 3;
}

static struct gv_library mk_lib(const struct gv_lib_entry *e, size_t n)
{
    struct gv_library l = { .entries = (struct gv_lib_entry *)e, .n = n };
    return l;
}

#define E(v, t, s) { v, t, s }

static void test_rows(void)
{
    struct gv_rows stock, out;
    struct gv_library lib;

    /* empty library: identical to stock */
    mk_desc(5, 8);
    mk_stock(&stock);
    lib = mk_lib(NULL, 0);
    CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) == 0);
    CHECK(out.n == 3 && out.n_new_vendors == 0 && out.n_added_entries == 0);
    CHECK(!memcmp(out.v, stock.v, 3 * sizeof(out.v[0])));
    gv_rows_free(&out);

    /* append into stock vendors case-insensitively, TYPE#Sub formatting,
     * type upper-cased, sub case preserved */
    {
        const struct gv_lib_entry e[] = {
            E("snapmaker", "petg", "HF"),
            E("SNAPMAKER", "tpu", ""),
            E("POLYMAKER", "Pla+", "Silk"),
        };
        lib = mk_lib(e, 3);
        mk_stock(&stock);
        CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) == 0);
        CHECK(out.n == 3 && out.n_new_vendors == 0 && out.n_added_entries == 3);
        CHECK(out.v[0].n_entries == 5);
        CHECK_STR(out.v[0].entries[3], "PETG#HF");
        CHECK_STR(out.v[0].entries[4], "TPU");
        CHECK_STR(out.v[0].vendor, "Snapmaker");   /* stock spelling kept */
        CHECK(out.v[1].n_entries == 2);
        CHECK_STR(out.v[1].entries[1], "PLA+#Silk");
        CHECK(stock.v[0].n_entries == 3);          /* stock input untouched */
        gv_rows_free(&out);
    }

    /* overflow dropped: Snapmaker has 3 of max 5 -> only 2 fit */
    {
        const struct gv_lib_entry e[] = {
            E("Snapmaker", "A", ""), E("Snapmaker", "B", ""), E("Snapmaker", "C", ""),
            E("Snapmaker", "D", ""),
        };
        lib = mk_lib(e, 4);
        mk_stock(&stock);
        CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) == 0);
        CHECK(out.v[0].n_entries == 5 && out.n_added_entries == 2);
        CHECK_STR(out.v[0].entries[4], "B");
        gv_rows_free(&out);
    }

    /* new vendors sorted alphabetically (no brand_order); later entries join the row;
     * per-row max_entries; dedupe case-insensitive (also vs stock) */
    {
        const struct gv_lib_entry e[] = {
            E("Overture", "PETG", ""),
            E("eSun", "PLA", "Matte"),
            E("overture", "PLA", ""),
            E("OVERTURE", "petg", ""),         /* dup of PETG */
            E("Overture", "PLA", ""),          /* dup */
            E("Generic", "pla", ""),           /* dup of stock PLA */
            E("Generic", "PETG", ""),          /* appended into stock Generic */
            E("esun", "ABS", ""), E("esun", "ASA", ""), E("esun", "TPU", ""),
            E("esun", "PA", ""), E("esun", "PC", ""),   /* 6th entry in eSun: dropped */
            E("Polymaker", "pla", ""),         /* dup of stock PLA */
        };
        lib = mk_lib(e, sizeof(e) / sizeof(e[0]));
        mk_stock(&stock);
        CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) == 0);
        CHECK(out.n == 5 && out.n_stock == 3 && out.n_new_vendors == 2);
        CHECK_STR(out.v[3].vendor, "eSun");         /* case-insensitive */
        CHECK(out.v[3].n_entries == 5);
        CHECK_STR(out.v[3].entries[0], "PLA#Matte");
        CHECK_STR(out.v[3].entries[4], "PA");
        CHECK_STR(out.v[4].vendor, "Overture");
        CHECK(out.v[4].n_entries == 2);
        CHECK_STR(out.v[4].entries[0], "PETG");
        CHECK_STR(out.v[4].entries[1], "PLA");
        CHECK(out.v[2].n_entries == 3);
        CHECK_STR(out.v[2].entries[2], "PETG");
        CHECK(out.v[1].n_entries == 1);
        CHECK(out.n_added_entries == 1);
        gv_rows_free(&out);
    }

    /* brand_order: listed vendors first in that order (case-insensitive,
     * unknown names ignored), the rest alphabetically; stock rows stay put */
    {
        const struct gv_lib_entry e[] = {
            E("Zeta", "PLA", ""), E("alpha", "PLA", ""), E("Mid", "PLA", ""),
            E("Beta", "PLA", ""), E("Omega", "PLA", ""),
        };
        char order[3][GV_VENDOR_LEN] = { "omega", "Nobody", "Mid" };
        lib = mk_lib(e, 5);
        lib.brand_order = order;
        lib.n_order = 3;
        mk_desc(5, 8);
        mk_stock(&stock);
        CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) == 0);
        CHECK(out.n == 8);
        CHECK_STR(out.v[0].vendor, "Snapmaker");
        CHECK_STR(out.v[2].vendor, "Generic");
        CHECK_STR(out.v[3].vendor, "Omega");
        CHECK_STR(out.v[4].vendor, "Mid");
        CHECK_STR(out.v[5].vendor, "alpha");
        CHECK_STR(out.v[6].vendor, "Beta");
        CHECK_STR(out.v[7].vendor, "Zeta");
        gv_rows_free(&out);

        /* the vendor cap keeps the first vendors of the sorted order */
        mk_desc(5, 5);
        CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) == 0);
        CHECK(out.n == 5 && out.n_new_vendors == 2);
        CHECK_STR(out.v[3].vendor, "Omega");
        CHECK_STR(out.v[4].vendor, "Mid");
        gv_rows_free(&out);
    }

    /* max_vendors cap: 3 stock + 2 new allowed, rest dropped; entries for an
     * already-added vendor are still accepted */
    {
        const struct gv_lib_entry e[] = {
            E("V1", "PLA", ""), E("V2", "PLA", ""), E("V3", "PLA", ""),
            E("V4", "PLA", ""), E("v1", "ABS", ""),
        };
        lib = mk_lib(e, 5);
        mk_desc(5, 5);
        mk_stock(&stock);
        CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) == 0);
        CHECK(out.n == 5 && out.n_new_vendors == 2);
        CHECK_STR(out.v[3].vendor, "V1");
        CHECK_STR(out.v[4].vendor, "V2");
        CHECK(out.v[3].n_entries == 2);
        gv_rows_free(&out);

        /* cap equal to stock count: no new vendors at all */
        mk_desc(5, 3);
        CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) == 0);
        CHECK(out.n == 3 && out.n_new_vendors == 0);
        gv_rows_free(&out);
    }

    /* bad stock input */
    {
        mk_desc(5, 8);
        mk_stock(&stock);
        stock.v[1].n_entries = 6;
        lib = mk_lib(NULL, 0);
        CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) < 0);
        memset(&stock, 0, sizeof(stock));
        CHECK(gv_build_rows(&g_desc, &lib, &stock, &out) < 0);
    }
}

int main(void)
{
    gv_quiet = 1;
    test_sha256();
    test_descriptor();
    test_descriptor_verify();
    test_library();
    test_rows();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
