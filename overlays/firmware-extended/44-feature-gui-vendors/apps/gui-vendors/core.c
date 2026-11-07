// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

// Descriptor parsing, SHA-256, filament library parsing and row building.
// Pure logic, no process/memory manipulation: host-testable.

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>

#include "gui_vendors.h"
#include "jsmn.h"

#define LOG_PREFIX "[gui-vendors] "

int gv_debug;
int gv_quiet;

void gv_log(int is_error, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    if (gv_quiet)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, LOG_PREFIX "%s: %s\n", is_error ? "ERROR" : "INFO", buf);
    syslog((is_error ? LOG_ERR : LOG_INFO) | LOG_USER, LOG_PREFIX "%s", buf);
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (!err || !errlen)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* ---- SHA-256 --------------------------------------------------------- */

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
};

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(struct sha256 *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->fill = 0;
}

static void sha256_update(struct sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        size_t k = 64 - s->fill;
        if (k > n)
            k = n;
        memcpy(s->buf + s->fill, p, k);
        s->fill += k; p += k; n -= k;
        if (s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha256_final(struct sha256 *s, char hex[65])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    sha256_update(s, &pad, 1);
    while (s->fill != 56)
        sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++)
        snprintf(hex + 8 * i, 9, "%08x", s->h[i]);
}

void gv_sha256_buf(const void *data, size_t len, char hex[65])
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, hex);
}

int gv_sha256_file(const char *path, char hex[65])
{
    struct sha256 s;
    uint8_t buf[65536];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha256_init(&s);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_update(&s, buf, n);
    int bad = ferror(f);
    fclose(f);
    if (bad)
        return -1;
    sha256_final(&s, hex);
    return 0;
}

/* ---- descriptor ------------------------------------------------------ */

static int parse_hex(const char *s, uint64_t *out)
{
    char *end;
    if (!s || !*s)
        return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (!*s)
        return -1;
    for (const char *p = s; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            return -1;
    *out = strtoull(s, &end, 16);
    return *end ? -1 : 0;
}

static int parse_uint(const char *s, unsigned *out)
{
    char *end;
    unsigned long v;
    if (!s || !*s || *s < '0' || *s > '9')
        return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint64_t h;
        if (parse_hex(s, &h) < 0 || h > 0xffffff)
            return -1;
        *out = (unsigned)h;
        return 0;
    }
    v = strtoul(s, &end, 10);
    if (*end || v > 0xffffff)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static int parse_reg(const char *s, int *out)
{
    unsigned v;
    if (*s == 'x' || *s == 'X')
        s++;
    if (parse_uint(s, &v) < 0 || v > 30)
        return -1;
    *out = (int)v;
    return 0;
}

#define MAX_TOKS 16

static int parse_line(char *line, struct gv_descriptor *d, int *seen, char *err, size_t errlen)
{
    char *tok[MAX_TOKS];
    int n = 0;
    char *c = strchr(line, '#');
    if (c)
        *c = '\0';
    for (char *p = strtok(line, " \t\r"); p; p = strtok(NULL, " \t\r")) {
        if (n == MAX_TOKS) {
            set_err(err, errlen, "too many fields");
            return -1;
        }
        tok[n++] = p;
    }
    if (n == 0)
        return 0;

    if (!strcmp(tok[0], "gui_sha256")) {
        if (n != 2 || strlen(tok[1]) != 64) {
            set_err(err, errlen, "bad gui_sha256 record");
            return -1;
        }
        for (int i = 0; i < 64; i++) {
            char ch = tok[1][i];
            if (ch >= 'A' && ch <= 'F')
                ch = (char)(ch - 'A' + 'a');
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
                set_err(err, errlen, "gui_sha256 is not hex");
                return -1;
            }
            d->gui_sha256[i] = ch;
        }
        d->gui_sha256[64] = '\0';
        *seen |= 1;
    } else if (!strcmp(tok[0], "table")) {
        if (n != 8 || strcmp(tok[2], "rows") || strcmp(tok[4], "stride") ||
            strcmp(tok[6], "max_entries") ||
            parse_hex(tok[1], &d->table_vaddr) < 0 ||
            parse_uint(tok[3], &d->table_rows) < 0 ||
            parse_uint(tok[5], &d->table_stride) < 0 ||
            parse_uint(tok[7], &d->max_entries) < 0) {
            set_err(err, errlen, "bad table record");
            return -1;
        }
        *seen |= 2;
    } else if (!strcmp(tok[0], "buttons")) {
        if (n != 4 || strcmp(tok[2], "slots") ||
            parse_hex(tok[1], &d->buttons_vaddr) < 0 ||
            parse_uint(tok[3], &d->button_slots) < 0) {
            set_err(err, errlen, "bad buttons record");
            return -1;
        }
        *seen |= 4;
    } else if (!strcmp(tok[0], "max_vendors")) {
        if (n != 2 || parse_uint(tok[1], &d->max_vendors) < 0) {
            set_err(err, errlen, "bad max_vendors record");
            return -1;
        }
        *seen |= 8;
    } else if (!strcmp(tok[0], "site")) {
        struct gv_site *s;
        uint64_t insn;
        int wants_reg;
        if (n < 4) {
            set_err(err, errlen, "bad site record");
            return -1;
        }
        if (d->n_sites == GV_MAX_SITES) {
            set_err(err, errlen, "too many sites");
            return -1;
        }
        s = &d->sites[d->n_sites];
        s->xd = -1;
        if (!strcmp(tok[1], "table")) {
            s->kind = GV_SITE_TABLE; wants_reg = 1;
        } else if (!strcmp(tok[1], "buttons")) {
            s->kind = GV_SITE_BUTTONS; wants_reg = 1;
        } else if (!strcmp(tok[1], "bound_count")) {
            s->kind = GV_SITE_BOUND_COUNT; wants_reg = 0;
        } else if (!strcmp(tok[1], "bound_maxidx")) {
            s->kind = GV_SITE_BOUND_MAXIDX; wants_reg = 0;
        } else {
            set_err(err, errlen, "unknown site kind '%s'", tok[1]);
            return -1;
        }
        if (n != 4 + wants_reg || parse_hex(tok[2], &s->vaddr) < 0 ||
            parse_hex(tok[3], &insn) < 0 || insn > 0xffffffffu ||
            (wants_reg && parse_reg(tok[4], &s->xd) < 0)) {
            set_err(err, errlen, "bad site %s record", tok[1]);
            return -1;
        }
        s->orig_insn = (uint32_t)insn;
        d->n_sites++;
    } else {
        set_err(err, errlen, "unknown record '%s'", tok[0]);
        return -1;
    }
    return 0;
}

int gv_descriptor_parse(const char *text, struct gv_descriptor *d, char *err, size_t errlen)
{
    char *copy, *save = NULL, *line;
    int seen = 0, lineno = 0, rc = -1;
    unsigned kinds[5] = { 0 };

    memset(d, 0, sizeof(*d));
    copy = strdup(text);
    if (!copy) {
        set_err(err, errlen, "out of memory");
        return -1;
    }
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char msg[128] = "";
        lineno++;
        if (parse_line(line, d, &seen, msg, sizeof(msg)) < 0) {
            set_err(err, errlen, "line %d: %s", lineno, msg);
            goto out;
        }
    }
    if (seen != 15) {
        set_err(err, errlen, "missing gui_sha256/table/buttons/max_vendors record");
        goto out;
    }
    for (unsigned i = 0; i < d->n_sites; i++)
        kinds[d->sites[i].kind]++;
    if (!kinds[GV_SITE_TABLE] || !kinds[GV_SITE_BUTTONS] ||
        !kinds[GV_SITE_BOUND_COUNT] || !kinds[GV_SITE_BOUND_MAXIDX]) {
        set_err(err, errlen, "descriptor lacks sites of every kind");
        goto out;
    }
    if (!d->table_rows || !d->table_stride || (d->table_stride & 7) ||
        !d->max_entries || d->max_entries > GV_MAX_ENTRIES_CAP ||
        d->max_entries * 8u + 8u > d->table_stride ||
        d->table_rows > GV_MAX_ROWS_CAP || !d->button_slots ||
        d->max_vendors < d->table_rows || d->max_vendors > GV_MAX_ROWS_CAP) {
        set_err(err, errlen, "inconsistent table/buttons/max_vendors values");
        goto out;
    }
    rc = 0;
out:
    free(copy);
    return rc;
}

int gv_descriptor_load(const char *path, struct gv_descriptor *d, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t n;
    int rc;

    if (!f) {
        set_err(err, errlen, "cannot open %s", path);
        return -1;
    }
    buf = malloc(65536);
    if (!buf) {
        fclose(f);
        set_err(err, errlen, "out of memory");
        return -1;
    }
    n = fread(buf, 1, 65536, f);  /* one byte more than allowed: detects too large */
    if (ferror(f) || n > 65535) {
        fclose(f);
        free(buf);
        set_err(err, errlen, "cannot read %s (or too large)", path);
        return -1;
    }
    fclose(f);
    buf[n] = '\0';
    if (strlen(buf) != n) {
        free(buf);
        set_err(err, errlen, "%s contains NUL bytes", path);
        return -1;
    }
    rc = gv_descriptor_parse(buf, d, err, errlen);
    free(buf);
    return rc;
}

int gv_descriptor_verify(const struct gv_descriptor *d, const char *exe_path,
                         char *err, size_t errlen)
{
    char hex[65];
    if (gv_sha256_file(exe_path, hex) < 0) {
        set_err(err, errlen, "cannot hash %s", exe_path);
        return -1;
    }
    if (strcmp(hex, d->gui_sha256)) {
        set_err(err, errlen, "sha256 mismatch (binary %s, descriptor %s)", hex, d->gui_sha256);
        return -1;
    }
    return 0;
}

/* ---- JSON library ---------------------------------------------------- */

/* Number of tokens in the subtree rooted at t[i]. */
static int tok_span(const jsmntok_t *t, int i)
{
    int span = 1;
    for (int k = 0; k < t[i].size; k++)
        span += tok_span(t, i + span);
    return span;
}

static int append_utf8(char *out, size_t cap, size_t *pos, uint32_t cp)
{
    uint8_t b[4];
    int n;
    if (cp < 0x80) { b[0] = (uint8_t)cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xc0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3f); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xe0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3f); b[2] = 0x80 | (cp & 0x3f); n = 3; }
    else { b[0] = 0xf0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3f); b[2] = 0x80 | ((cp >> 6) & 0x3f); b[3] = 0x80 | (cp & 0x3f); n = 4; }
    if (*pos + n >= cap)
        return -1;
    memcpy(out + *pos, b, n);
    *pos += n;
    return 0;
}

static int hex4(const char *s, uint32_t *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        *v <<= 4;
        if (c >= '0' && c <= '9') *v |= c - '0';
        else if (c >= 'a' && c <= 'f') *v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') *v |= c - 'A' + 10;
        else return -1;
    }
    return 0;
}

/* Unescape a JSON string token into out. -1 if invalid or too long. */
static int tok_string(const char *js, const jsmntok_t *t, char *out, size_t cap)
{
    size_t pos = 0;
    if (t->type != JSMN_STRING)
        return -1;
    for (int i = t->start; i < t->end; i++) {
        char c = js[i];
        if (c != '\\') {
            if (pos + 1 >= cap)
                return -1;
            out[pos++] = c;
            continue;
        }
        c = js[++i];
        switch (c) {
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'u': {
            uint32_t cp, lo;
            if (i + 4 >= t->end)
                return -1;
            if (hex4(js + i + 1, &cp) < 0 || cp == 0)
                return -1;
            i += 4;
            if (cp >= 0xd800 && cp < 0xdc00) {
                /* high surrogate: needs \uDC00..\uDFFF */
                if (i + 6 >= t->end || js[i + 1] != '\\' || js[i + 2] != 'u' ||
                    hex4(js + i + 3, &lo) < 0 || lo < 0xdc00 || lo > 0xdfff)
                    return -1;
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                i += 6;
            } else if (cp >= 0xdc00 && cp < 0xe000) {
                return -1;
            }
            if (append_utf8(out, cap, &pos, cp) < 0)
                return -1;
            continue;
        }
        default: break; /* \" \\ \/ */
        }
        if (pos + 1 >= cap)
            return -1;
        out[pos++] = c;
    }
    out[pos] = '\0';
    return 0;
}

static int key_eq(const char *js, const jsmntok_t *t, const char *key)
{
    size_t n = strlen(key);
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == n &&
           !memcmp(js + t->start, key, n);
}

/*
 * The GUI sends `SET_PRINT_FILAMENT_CONFIG ... VENDOR=%s FILAMENT_TYPE=%s
 * FILAMENT_SUBTYPE='%s'`: Klipper ends the command at '#', '*' or ';', and
 * VENDOR/FILAMENT_TYPE are unquoted. '#'/'*' also separate type and subtype
 * in the GUI's table entries. So vendor and type may not contain whitespace
 * or any of #*;'" and sub_type may not contain #*;'" (spaces are fine).
 */
static int field_ok(const char *s, int allow_space)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || strchr("#*;'\"", *p))
            return 0;
        if (!allow_space && *p == ' ')
            return 0;
    }
    return 1;
}

static void trim(char *s)
{
    size_t n = strlen(s), a = 0;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    while (s[a] == ' ' || s[a] == '\t')
        a++;
    if (a)
        memmove(s, s + a, strlen(s + a) + 1);
}

/* Reads a string-valued member; absent or non-string leaves out "". */
static int get_member(const char *js, const jsmntok_t *t, int obj, const char *key,
                      char *out, size_t cap)
{
    int i = obj + 1;
    out[0] = '\0';
    for (int k = 0; k < t[obj].size; k++) {
        int v = i + 1;
        if (key_eq(js, &t[i], key))
            return tok_string(js, &t[v], out, cap) < 0 ? (out[0] = '\0', -1) : 0;
        i = v + tok_span(t, v);
    }
    return 0;
}

/*
 * Nesting depth of JSON text, outside strings. jsmn's closing-bracket walk
 * is O(depth) and tok_span() recurses per level, so deep nesting would hang
 * or overflow the stack of the GUI's constructor: reject it up front.
 */
static int json_depth_ok(const char *js, size_t len)
{
    int depth = 0, in_str = 0;
    for (size_t i = 0; i < len && js[i]; i++) {
        char c = js[i];
        if (in_str) {
            if (c == '\\')
                i++;
            else if (c == '"')
                in_str = 0;
        } else if (c == '"') {
            in_str = 1;
        } else if (c == '{' || c == '[') {
            if (++depth > GV_JSON_MAX_DEPTH)
                return 0;
        } else if (c == '}' || c == ']') {
            depth--;
        }
    }
    return 1;
}

int gv_library_parse(const char *json, size_t len, struct gv_library *lib,
                     char *err, size_t errlen)
{
    jsmn_parser p;
    jsmntok_t *t = NULL;
    int ntok, root = 0, arr = -1, order = -1, rc = -1;
    size_t cap = 0;

    memset(lib, 0, sizeof(*lib));

    if (!json_depth_ok(json, len)) {
        set_err(err, errlen, "JSON nested deeper than %d levels", GV_JSON_MAX_DEPTH);
        return -1;
    }
    jsmn_init(&p);
    ntok = jsmn_parse(&p, json, len, NULL, 0);
    if (ntok < 0) {
        set_err(err, errlen, "malformed JSON");
        return -1;
    }
    if (ntok == 0) {
        set_err(err, errlen, "empty JSON");
        return -1;
    }
    t = calloc((size_t)ntok, sizeof(*t));
    if (!t) {
        set_err(err, errlen, "out of memory");
        return -1;
    }
    jsmn_init(&p);
    if (jsmn_parse(&p, json, len, t, (unsigned)ntok) < 0) {
        set_err(err, errlen, "malformed JSON");
        goto out;
    }
    if (t[root].type != JSMN_OBJECT) {
        set_err(err, errlen, "top level is not an object");
        goto out;
    }
    for (int i = root + 1, k = 0; k < t[root].size; k++) {
        int v = i + 1;
        if (key_eq(json, &t[i], "filaments"))
            arr = v;
        else if (key_eq(json, &t[i], "brand_order"))
            order = v;
        i = v + tok_span(t, v);
    }
    if (order >= 0 && t[order].type == JSMN_ARRAY && t[order].size > 0) {
        lib->brand_order = calloc(GV_MAX_ROWS_CAP, sizeof(*lib->brand_order));
        if (!lib->brand_order) {
            set_err(err, errlen, "out of memory");
            goto out;
        }
        for (int i = order + 1, k = 0; k < t[order].size; k++) {
            char *b = lib->brand_order[lib->n_order];
            int el = i;
            i += tok_span(t, el);
            if (lib->n_order >= GV_MAX_ROWS_CAP || tok_string(json, &t[el], b, GV_VENDOR_LEN) < 0)
                continue;
            trim(b);
            if (b[0])
                lib->n_order++;
        }
    } else if (order >= 0 && t[order].type != JSMN_ARRAY) {
        GV_INFO("ignoring \"brand_order\": not an array");
    }
    if (arr < 0) {
        rc = 0; /* no filaments key: empty library */
        goto out;
    }
    if (t[arr].type != JSMN_ARRAY) {
        set_err(err, errlen, "\"filaments\" is not an array");
        goto out;
    }

    for (int i = arr + 1, k = 0; k < t[arr].size; k++) {
        struct gv_lib_entry e;
        int el = i;
        i += tok_span(t, el);
        if (t[el].type != JSMN_OBJECT)
            continue;
        if (get_member(json, t, el, "vendor", e.vendor, sizeof(e.vendor)) < 0 ||
            get_member(json, t, el, "type", e.type, sizeof(e.type)) < 0 ||
            get_member(json, t, el, "sub_type", e.sub_type, sizeof(e.sub_type)) < 0) {
            GV_INFO("skipping library entry %d: vendor/type/sub_type not a string or too long "
                    "(max %d/%d/%d bytes)", k, GV_VENDOR_LEN - 1, GV_TYPE_LEN - 1, GV_SUBTYPE_LEN - 1);
            continue;
        }
        trim(e.vendor); trim(e.type); trim(e.sub_type);
        if (!e.vendor[0])
            strcpy(e.vendor, "Generic");    /* as the web UI and Klipper */
        if (!e.type[0]) {
            GV_DEBUG("skipping library entry %d: no type", k);
            continue;
        }
        if (!field_ok(e.vendor, 0) || !field_ok(e.type, 0) || !field_ok(e.sub_type, 1)) {
            GV_INFO("skipping library entry %d (%s/%s/%s): vendor/type may not contain "
                    "whitespace or #*;'\", sub_type not #*;'\"", k, e.vendor, e.type, e.sub_type);
            continue;
        }
        if (lib->n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            struct gv_lib_entry *ne = realloc(lib->entries, ncap * sizeof(*ne));
            if (!ne) {
                set_err(err, errlen, "out of memory");
                goto out;
            }
            lib->entries = ne;
            cap = ncap;
        }
        lib->entries[lib->n++] = e;
    }
    rc = 0;
out:
    free(t);
    if (rc < 0)
        gv_library_free(lib);
    return rc;
}

int gv_library_load(const char *path, struct gv_library *lib, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t n;
    int rc;

    memset(lib, 0, sizeof(*lib));
    if (!f) {
        GV_DEBUG("no library at %s", path);
        return 0;
    }
    buf = malloc(GV_LIBRARY_MAX_SIZE + 1);
    if (!buf) {
        fclose(f);
        set_err(err, errlen, "out of memory");
        return -1;
    }
    n = fread(buf, 1, GV_LIBRARY_MAX_SIZE + 1, f);
    if (ferror(f) || n > GV_LIBRARY_MAX_SIZE) {
        fclose(f);
        free(buf);
        set_err(err, errlen, "cannot read %s (or larger than %d bytes)", path, GV_LIBRARY_MAX_SIZE);
        return -1;
    }
    fclose(f);
    buf[n] = '\0';
    {
        /* whitespace-only file counts as empty */
        size_t i = 0;
        while (i < n && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t'))
            i++;
        if (i == n) {
            free(buf);
            return 0;
        }
    }
    rc = gv_library_parse(buf, n, lib, err, errlen);
    free(buf);
    return rc;
}

void gv_library_free(struct gv_library *lib)
{
    free(lib->entries);
    free(lib->brand_order);
    memset(lib, 0, sizeof(*lib));
}

/* ---- rows ------------------------------------------------------------ */

static const char *own(struct gv_rows *r, char *s)
{
    char **n = realloc(r->owned, (r->n_owned + 1) * sizeof(*n));
    if (!n) {
        free(s);
        return NULL;
    }
    r->owned = n;
    n[r->n_owned++] = s;
    return s;
}

void gv_rows_free(struct gv_rows *r)
{
    for (size_t i = 0; i < r->n_owned; i++)
        free(r->owned[i]);
    free(r->owned);
    memset(r, 0, sizeof(*r));
}

static char *make_entry(const struct gv_lib_entry *e)
{
    char *s = malloc(strlen(e->type) + 1 + strlen(e->sub_type) + 1);
    size_t n = 0;
    if (!s)
        return NULL;
    for (const char *p = e->type; *p; p++)
        s[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    if (e->sub_type[0]) {
        s[n++] = '#';
        strcpy(s + n, e->sub_type);
    } else {
        s[n] = '\0';
    }
    return s;
}

static int row_has(const struct gv_row *row, const char *entry)
{
    for (unsigned i = 0; i < row->n_entries; i++)
        if (!strcasecmp(row->entries[i], entry))
            return 1;
    return 0;
}

static size_t brand_rank(const struct gv_library *lib, const char *vendor)
{
    for (size_t i = 0; i < lib->n_order; i++)
        if (!strcasecmp(lib->brand_order[i], vendor))
            return i;
    return lib->n_order;
}

/* Added rows (after the stock ones, which keep their place): brand_order
 * first, then alphabetically. Stable insertion sort, at most 64 rows. */
static void sort_new_rows(const struct gv_library *lib, struct gv_rows *r)
{
    for (unsigned i = r->n_stock + 1; i < r->n; i++) {
        struct gv_row cur = r->v[i];
        size_t rank = brand_rank(lib, cur.vendor);
        unsigned j = i;
        while (j > r->n_stock) {
            const struct gv_row *prev = &r->v[j - 1];
            size_t prank = brand_rank(lib, prev->vendor);
            if (prank < rank || (prank == rank && (rank < lib->n_order ||
                                                   strcasecmp(prev->vendor, cur.vendor) <= 0)))
                break;
            r->v[j] = *prev;
            j--;
        }
        r->v[j] = cur;
    }
}

int gv_build_rows(const struct gv_descriptor *d, const struct gv_library *lib,
                  const struct gv_rows *stock, struct gv_rows *out)
{
    memset(out, 0, sizeof(*out));
    if (stock->n == 0 || stock->n > GV_MAX_ROWS_CAP) {
        GV_ERROR("bad stock row count %u", stock->n);
        return -1;
    }
    for (unsigned i = 0; i < stock->n; i++) {
        if (!stock->v[i].vendor || stock->v[i].n_entries > d->max_entries) {
            GV_ERROR("bad stock row %u", i);
            return -1;
        }
    }
    memcpy(out->v, stock->v, sizeof(stock->v[0]) * stock->n);
    out->n = out->n_stock = stock->n;

    for (size_t i = 0; i < lib->n; i++) {
        const struct gv_lib_entry *e = &lib->entries[i];
        struct gv_row *row = NULL;
        char *entry;
        const char *oe;

        for (unsigned r = 0; r < out->n; r++) {
            if (!strcasecmp(out->v[r].vendor, e->vendor)) {
                row = &out->v[r];
                break;
            }
        }
        entry = make_entry(e);
        if (!entry)
            goto nomem;
        if (row && row_has(row, entry)) {
            GV_DEBUG("duplicate entry %s/%s", row->vendor, entry);
            free(entry);
            continue;
        }
        if (!row) {
            if (out->n >= GV_MAX_ROWS_CAP) {
                GV_INFO("more than %d vendors, dropping %s/%s", GV_MAX_ROWS_CAP, e->vendor, entry);
                free(entry);
                continue;
            }
            char *vn = strdup(e->vendor);
            if (!vn) {
                free(entry);
                goto nomem;
            }
            row = &out->v[out->n];
            row->vendor = own(out, vn);
            if (!row->vendor) {
                free(entry);
                goto nomem;
            }
            row->n_entries = 0;
            out->n++;
            out->n_new_vendors++;
        } else if (row->n_entries >= d->max_entries) {
            GV_INFO("vendor %s is full (%u entries), dropping %s", row->vendor, d->max_entries, entry);
            free(entry);
            continue;
        }
        oe = own(out, entry);
        if (!oe)
            goto nomem;
        row->entries[row->n_entries++] = oe;
        if ((unsigned)(row - out->v) < out->n_stock)
            out->n_added_entries++;
    }
    sort_new_rows(lib, out);
    while (out->n > d->max_vendors) {
        out->n--;
        out->n_new_vendors--;
        GV_INFO("vendor cap %u reached, dropping vendor %s (%u entries)", d->max_vendors,
                out->v[out->n].vendor, out->v[out->n].n_entries);
    }
    return 0;

nomem:
    GV_ERROR("out of memory");
    gv_rows_free(out);
    return -1;
}
