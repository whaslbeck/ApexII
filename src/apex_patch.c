/* realpath() is POSIX (XSI): -std=c99 alone does not declare it on glibc */
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "apex_patch.h"

#include "apex_asm.h"
#include "apex_png.h"
#include "apex_rominfo.h"
#include "apex_wpc_hooks.h"
#include "apexdmd.h"
#include "apexsprite.h"
#include "cpu6809.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATCH_NOP 0x12u /* 6809 NOP */

/* ── small helpers ─────────────────────────────────────────────────────── */

static char *pstrdup(const char *s) {
    size_t n = strlen(s);
    char *d = xmalloc(n + 1u);
    memcpy(d, s, n + 1u);
    return d;
}

static void set_str(char **dst, const char *s) {
    free(*dst);
    *dst = s ? pstrdup(s) : NULL;
}

/* Append to a heap string (with a separator when both are non-empty). */
static void append_str(char **dst, const char *s, const char *sep) {
    size_t a = *dst ? strlen(*dst) : 0, b = strlen(s), c = (*dst && sep) ? strlen(sep) : 0;
    char *n = xmalloc(a + b + c + 1u);
    if (a)
        memcpy(n, *dst, a);
    if (c)
        memcpy(n + a, sep, c);
    memcpy(n + a + c, s, b + 1u);
    free(*dst);
    *dst = n;
}

static char *trim_ws(char *s) {
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static void fmt_err(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    if (!err || !errsz)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* Hex byte list: "86 0a", "860a", "0x86, 0x0a".  Returns the count, or -1. */
static long parse_hex_bytes(const char *text, uint8_t **out) {
    size_t cap = strlen(text) / 2u + 1u, n = 0;
    uint8_t *b = xmalloc(cap);
    const char *p = text;
    while (*p) {
        int hi, lo;
        while (*p && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (!*p)
            break;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            free(b);
            return -1;
        }
        hi = isdigit((unsigned char)p[0]) ? p[0] - '0' : (tolower((unsigned char)p[0]) - 'a' + 10);
        lo = isdigit((unsigned char)p[1]) ? p[1] - '0' : (tolower((unsigned char)p[1]) - 'a' + 10);
        if (n == cap) {
            cap *= 2u;
            uint8_t *nb = realloc(b, cap);
            if (!nb) {
                free(b);
                return -1;
            }
            b = nb;
        }
        b[n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    *out = b;
    return (long)n;
}

static char *hex_text(const uint8_t *b, size_t n) {
    char *s = xmalloc(n * 3u + 1u);
    size_t i, at = 0;
    s[0] = '\0';
    for (i = 0; i < n; i++)
        at += (size_t)sprintf(s + at, i ? " %02x" : "%02x", b[i]);
    return s;
}

static int ci_ncmp(const char *a, const char *b, size_t n) {
    for (; n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++, n--)
        ;
    return n ? tolower((unsigned char)*a) - tolower((unsigned char)*b) : 0;
}

static const char *const k_type_names[APEX_PATCH_TYPE_COUNT] = {
    "bytes", "nop",    "asm",  "hook", "ptr", "ram",    "free",   "reserve",
    "value", "string", "move", "blob", "dmd", "sprite", "branch", "redirect"};

const char *apex_patch_type_name(int type) {
    return (type >= 0 && type < APEX_PATCH_TYPE_COUNT) ? k_type_names[type] : "?";
}

static const char *const k_checksum_names[] = {"fix", "disable", "keep"};

/* ── parse / save ──────────────────────────────────────────────────────── */

static ApexPatchOp *add_op(ApexPatch *p, const char *name) {
    if (p->count == p->cap) {
        size_t ncap = p->cap ? p->cap * 2u : 16u;
        ApexPatchOp *n = realloc(p->ops, ncap * sizeof(*n));
        if (!n)
            return NULL;
        p->ops = n;
        p->cap = ncap;
    }
    ApexPatchOp *op = &p->ops[p->count++];
    memset(op, 0, sizeof(*op));
    op->name = pstrdup(name);
    op->enabled = 1;
    op->type = -1;
    op->bright = -1;
    return op;
}

/* Value text: quoted ("..." with \n \" \\ escapes) or plain up to a ';' / '#'
   comment.  Writes into a new heap string. */
static char *parse_value(const char *v, int *ok) {
    *ok = 1;
    if (*v == '"') {
        size_t n = 0;
        char *out = xmalloc(strlen(v) + 1u);
        const char *p = v + 1;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                out[n++] = *p == 'n' ? '\n' : *p;
            } else {
                out[n++] = *p;
            }
            p++;
        }
        out[n] = '\0';
        if (*p != '"')
            *ok = 0;
        return out;
    }
    char *tmp = pstrdup(v);
    char *c = tmp;
    while (*c && *c != ';' && *c != '#')
        c++;
    *c = '\0';
    char *t = pstrdup(trim_ws(tmp));
    free(tmp);
    return t;
}

static int parse_bool(const char *v, int *out) {
    if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "on")) {
        *out = 1;
        return 1;
    }
    if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "off")) {
        *out = 0;
        return 1;
    }
    return 0;
}

/* What an op lacks to be built (a missing key, an impossible width), or 0.
   Checked when a file is loaded and again by every build: ops made in the GUI
   ("Add op", a changed type) reach the build without a load. */
static int op_problem(const ApexPatchOp *o, char *why, size_t n) {
    int t = o->type;
    const char *miss = NULL;
    if (t < 0)
        miss = "type";
    else if (!o->at && !((t == APEX_PATCH_ASM || t == APEX_PATCH_BLOB) && o->alloc))
        miss = "at";
    else if (t == APEX_PATCH_BYTES && !o->bytes)
        miss = "bytes";
    else if ((t == APEX_PATCH_ASM || t == APEX_PATCH_HOOK) && !o->code && !o->file)
        miss = "code or file";
    else if (t == APEX_PATCH_PTR && !o->target)
        miss = "target";
    else if (t == APEX_PATCH_RAM && !o->label)
        miss = "label";
    else if ((t == APEX_PATCH_FREE || t == APEX_PATCH_RESERVE) && !o->length)
        miss = "length";
    else if (t == APEX_PATCH_VALUE && !o->value)
        miss = "value";
    else if (t == APEX_PATCH_STRING && !o->text)
        miss = "text";
    else if (t == APEX_PATCH_REDIRECT && !o->target)
        miss = "target";
    else if ((t == APEX_PATCH_BLOB || t == APEX_PATCH_DMD || t == APEX_PATCH_SPRITE) && !o->file)
        miss = "file";
    if (miss) {
        snprintf(why, n, "missing %s", miss);
        return 1;
    }
    if (t == APEX_PATCH_PTR && o->width == 1) {
        snprintf(why, n, "a ptr is 2 (word) or 3 (far) bytes wide");
        return 1;
    }
    return 0;
}

int apex_patch_load(const char *path, ApexPatch *out, char *err, size_t errsz) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        memset(out, 0, sizeof(*out));
        out->path = pstrdup(path);
        fmt_err(err, errsz, "cannot open %s", path);
        return 1;
    }
    fclose(fp);
    Buffer f = read_file(path);
    char *t = xmalloc(f.size + 1u);
    memcpy(t, f.data, f.size);
    t[f.size] = '\0';
    free(f.data);
    int rc = apex_patch_load_text(path, t, out, err, errsz);
    free(t);
    return rc;
}

int apex_patch_load_text(const char *path, const char *src, ApexPatch *out, char *err,
                         size_t errsz) {
    char *text, *line, *next;
    unsigned long lineno = 0;
    int in_patch = 0;
    ApexPatchOp *op = NULL;
    char *pending = NULL; /* comment lines waiting for the next section */
    char **last = NULL;   /* value a '|' line continues */
    char *val = NULL;     /* value of the current line (freed by FAIL) */

    memset(out, 0, sizeof(*out));
    out->path = pstrdup(path);
    text = pstrdup(src);

#define FAIL(...)                                                                                  \
    do {                                                                                           \
        char m_[200];                                                                              \
        snprintf(m_, sizeof(m_), __VA_ARGS__);                                                     \
        fmt_err(err, errsz, "line %lu: %s", lineno, m_);                                           \
        free(val);                                                                                 \
        free(text);                                                                                \
        free(pending);                                                                             \
        apex_patch_free(out);                                                                      \
        return 1;                                                                                  \
    } while (0)

    for (line = text; line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        lineno++;
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r')
            line[ll - 1] = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '|') {
            if (!last)
                FAIL("'|' continuation without a value to continue");
            s++;
            if (*s == ' ')
                s++;
            if (*last && **last)
                append_str(last, s, "\n");
            else
                set_str(last, s);
            continue;
        }
        last = NULL;
        if (*s == '\0' || *s == ';' || *s == '#') {
            if (*s)
                append_str(&pending, line, "\n");
            continue;
        }
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close)
                FAIL("missing ']'");
            *close = '\0';
            const char *sec = s + 1;
            if (!strcmp(sec, "patch")) {
                in_patch = 1;
                op = NULL;
                if (pending) {
                    out->comment = pending;
                    pending = NULL;
                }
            } else if (!strncmp(sec, "op:", 3) && sec[3]) {
                size_t i;
                for (i = 0; i < out->count; i++) {
                    if (!strcmp(out->ops[i].name, sec + 3))
                        FAIL("op '%s' is defined twice", sec + 3);
                }
                in_patch = 0;
                op = add_op(out, sec + 3);
                if (!op)
                    FAIL("out of memory");
                op->comment = pending;
                pending = NULL;
            } else {
                FAIL("unknown section [%s] (expected [patch] or [op:NAME])", sec);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            FAIL("expected key = value");
        *eq = '\0';
        char *key = trim_ws(s);
        int ok;
        val = parse_value(trim_ws(eq + 1), &ok);
        if (!ok) {
            FAIL("unterminated quoted value");
        }
        if (pending && (in_patch || op)) {
            /* a comment inside a section belongs to that section */
            append_str(in_patch ? &out->comment : &op->comment, pending, "\n");
            free(pending);
            pending = NULL;
        }
        if (in_patch) {
            if (!strcmp(key, "base_rom")) {
                set_str(&out->base_rom, val);
            } else if (!strcmp(key, "analysis")) {
                set_str(&out->analysis, val);
            } else if (!strcmp(key, "output")) {
                set_str(&out->output, val);
            } else if (!strcmp(key, "base_crc32")) {
                uint32_t c;
                if (!parse_u32(val, &c)) {
                    FAIL("invalid base_crc32");
                }
                out->base_crc32 = c;
                out->has_crc = 1;
            } else if (!strcmp(key, "checksum")) {
                int i, found = 0;
                for (i = 0; i < 3; i++) {
                    if (!strcmp(val, k_checksum_names[i])) {
                        out->checksum = i;
                        found = 1;
                    }
                }
                if (!found) {
                    FAIL("checksum must be fix, disable or keep");
                }
            } else {
                FAIL("unknown [patch] key '%s'", key);
            }
        } else if (op) {
            if (!strcmp(key, "type")) {
                int t;
                op->type = -1;
                for (t = 0; t < APEX_PATCH_TYPE_COUNT; t++) {
                    if (!strcmp(val, apex_patch_type_name(t)))
                        op->type = t;
                }
                if (op->type < 0) {
                    FAIL("unknown op type '%s' (bytes, nop, asm, hook, ptr, ram, free, reserve, "
                         "value, string, move, blob, dmd, sprite, branch, redirect)",
                         val);
                }
            } else if (!strcmp(key, "enabled")) {
                if (!parse_bool(val, &op->enabled)) {
                    FAIL("enabled must be yes or no");
                }
            } else if (!strcmp(key, "at")) {
                set_str(&op->at, val);
            } else if (!strcmp(key, "expect")) {
                set_str(&op->expect, val);
            } else if (!strcmp(key, "bytes")) {
                set_str(&op->bytes, val);
            } else if (!strcmp(key, "code")) {
                set_str(&op->code, val);
                last = &op->code;
            } else if (!strcmp(key, "file")) {
                set_str(&op->file, val);
            } else if (!strcmp(key, "note")) {
                set_str(&op->note, val);
            } else if (!strcmp(key, "alloc")) {
                set_str(&op->alloc, val);
            } else if (!strcmp(key, "placed")) {
                set_str(&op->placed, val);
            } else if (!strcmp(key, "target")) {
                set_str(&op->target, val);
            } else if (!strcmp(key, "label")) {
                set_str(&op->label, val);
            } else if (!strcmp(key, "width")) {
                uint32_t n;
                if (!parse_u32(val, &n) || n < 1 || n > 3) {
                    FAIL("width must be 1, 2 or 3");
                }
                op->width = n;
            } else if (!strcmp(key, "value")) {
                set_str(&op->value, val);
            } else if (!strcmp(key, "vtype")) {
                set_str(&op->vtype, val);
            } else if (!strcmp(key, "text")) {
                set_str(&op->text, val);
            } else if (!strcmp(key, "mode")) {
                set_str(&op->mode, val);
            } else if (!strcmp(key, "planes")) {
                uint32_t n;
                if (!parse_u32(val, &n) || n < 1 || n > 4) {
                    FAIL("planes must be 1-4");
                }
                op->planes = n;
            } else if (!strcmp(key, "encoding")) {
                if (strcmp(val, "auto") && strcmp(val, "raw")) {
                    FAIL("encoding must be auto or raw");
                }
                set_str(&op->encoding, val);
            } else if (!strcmp(key, "bright")) {
                uint32_t n;
                if (!parse_u32(val, &n) || n > 1) {
                    FAIL("bright must be 0 or 1");
                }
                op->bright = (int)n;
            } else if (!strcmp(key, "accept")) {
                if (!parse_bool(val, &op->accept)) {
                    FAIL("accept must be yes or no");
                }
            } else if (!strcmp(key, "count") || !strcmp(key, "length")) {
                uint32_t n;
                if (!parse_u32(val, &n) || n == 0 || n > 0x8000u) {
                    FAIL("invalid %s", key);
                }
                if (!strcmp(key, "count"))
                    op->count = n;
                else
                    op->length = n;
            } else {
                FAIL("unknown op key '%s'", key);
            }
        } else {
            FAIL("key '%s' outside a section", key);
        }
        free(val);
        val = NULL;
    }
    free(pending);
    free(text);
    {
        size_t i;
        for (i = 0; i < out->count; i++) {
            const ApexPatchOp *o = &out->ops[i];
            char why[96];
            if (op_problem(o, why, sizeof(why))) {
                fmt_err(err, errsz, "op '%s': %s", o->name, why);
                apex_patch_free(out);
                return 1;
            }
        }
    }
#undef FAIL
    return 0;
}

/* Write a value, quoted when it would not survive the plain form. */
/* Where apex_patch_write puts the text: a file, or a growing buffer (undo
   snapshots without a temporary file). */
typedef struct {
    FILE *f;
    char *buf;
    size_t n, cap;
} PatchOut;

static void po_put(PatchOut *o, const char *s, size_t n) {
    if (o->f) {
        fwrite(s, 1, n, o->f);
        return;
    }
    if (o->n + n + 1u > o->cap) {
        size_t nc = o->cap ? o->cap : 4096u;
        while (nc < o->n + n + 1u)
            nc *= 2u;
        o->buf = realloc(o->buf, nc);
        o->cap = nc;
    }
    memcpy(o->buf + o->n, s, n);
    o->n += n;
    o->buf[o->n] = '\0';
}

static void po_puts(PatchOut *o, const char *s) {
    po_put(o, s, strlen(s));
}

static void po_putc(PatchOut *o, int c) {
    char ch = (char)c;
    po_put(o, &ch, 1);
}

static void po_printf(PatchOut *o, const char *fmt, ...) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(small)) {
        po_put(o, small, (size_t)n);
        return;
    }
    char *big = xmalloc((size_t)n + 1u);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1u, fmt, ap);
    va_end(ap);
    po_put(o, big, (size_t)n);
    free(big);
}

static void write_value(PatchOut *f, const char *key, const char *v) {
    int quote = strchr(v, ';') || strchr(v, '#') || strchr(v, '\n') || strchr(v, '"') ||
                (*v && (isspace((unsigned char)v[0]) || isspace((unsigned char)v[strlen(v) - 1])));
    po_printf(f, "%s = ", key);
    if (!quote) {
        po_printf(f, "%s\n", v);
        return;
    }
    po_putc(f, '"');
    for (; *v; v++) {
        if (*v == '"' || *v == '\\')
            po_putc(f, '\\');
        if (*v == '\n')
            po_puts(f, "\\n");
        else
            po_putc(f, *v);
    }
    po_puts(f, "\"\n");
}

static void write_patch(const ApexPatch *p, PatchOut *f);

int apex_patch_save(const ApexPatch *p, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return 1;
    apex_patch_write(p, f);
    return fclose(f) == 0 ? 0 : 1;
}

char *apex_patch_to_text(const ApexPatch *p) {
    PatchOut o = {NULL, NULL, 0, 0};
    write_patch(p, &o);
    return o.buf ? o.buf : pstrdup("");
}

static void write_patch(const ApexPatch *p, PatchOut *f) {
    size_t i;
    if (p->comment)
        po_printf(f, "%s\n", p->comment);
    po_puts(f, "[patch]\n");
    if (p->base_rom)
        write_value(f, "base_rom", p->base_rom);
    if (p->has_crc)
        po_printf(f, "base_crc32 = 0x%08x\n", (unsigned)p->base_crc32);
    if (p->analysis)
        write_value(f, "analysis", p->analysis);
    po_printf(f, "checksum = %s\n", k_checksum_names[p->checksum]);
    if (p->output)
        write_value(f, "output", p->output);
    for (i = 0; i < p->count; i++) {
        const ApexPatchOp *o = &p->ops[i];
        po_putc(f, '\n');
        if (o->comment)
            po_printf(f, "%s\n", o->comment);
        po_printf(f, "[op:%s]\n", o->name);
        po_printf(f, "type = %s\n", apex_patch_type_name(o->type));
        if (!o->enabled)
            po_puts(f, "enabled = no\n");
        if (o->at)
            write_value(f, "at", o->at);
        if (o->alloc)
            write_value(f, "alloc", o->alloc);
        if (o->placed)
            write_value(f, "placed", o->placed);
        if (o->label)
            write_value(f, "label", o->label);
        if (o->target)
            write_value(f, "target", o->target);
        if (o->width)
            po_printf(f, "width = %u\n", o->width);
        if (o->vtype)
            write_value(f, "vtype", o->vtype);
        if (o->value)
            write_value(f, "value", o->value);
        if (o->mode)
            write_value(f, "mode", o->mode);
        if (o->text)
            write_value(f, "text", o->text);
        if (o->planes)
            po_printf(f, "planes = %u\n", o->planes);
        if (o->encoding)
            write_value(f, "encoding", o->encoding);
        if (o->bright >= 0)
            po_printf(f, "bright = %d\n", o->bright);
        if (o->accept)
            po_puts(f, "accept = yes\n");
        if (o->count)
            po_printf(f, "count = %u\n", o->count);
        if (o->length)
            po_printf(f, "length = %u\n", o->length);
        if (o->expect)
            write_value(f, "expect", o->expect);
        if (o->bytes)
            write_value(f, "bytes", o->bytes);
        if (o->file)
            write_value(f, "file", o->file);
        if (o->code) {
            const char *c = o->code;
            po_puts(f, "code =\n");
            while (*c) {
                const char *nl = strchr(c, '\n');
                size_t n = nl ? (size_t)(nl - c) : strlen(c);
                po_printf(f, "    | %.*s\n", (int)n, c);
                c += n + (nl ? 1u : 0u);
            }
        }
        if (o->note)
            write_value(f, "note", o->note);
    }
}
void apex_patch_write(const ApexPatch *p, FILE *f) {
    PatchOut o = {f, NULL, 0, 0};
    write_patch(p, &o);
}

void apex_patch_free(ApexPatch *p) {
    size_t i;
    if (!p)
        return;
    for (i = 0; i < p->count; i++) {
        ApexPatchOp *o = &p->ops[i];
        free(o->name);
        free(o->at);
        free(o->expect);
        free(o->bytes);
        free(o->code);
        free(o->file);
        free(o->note);
        free(o->comment);
        free(o->alloc);
        free(o->placed);
        free(o->target);
        free(o->label);
        free(o->value);
        free(o->vtype);
        free(o->text);
        free(o->mode);
        free(o->encoding);
    }
    free(p->ops);
    free(p->path);
    free(p->base_rom);
    free(p->analysis);
    free(p->output);
    free(p->comment);
    memset(p, 0, sizeof(*p));
}

static void abs_path(const char *path, char *out, size_t outsz) {
#ifdef _WIN32
    if (!_fullpath(out, path, outsz))
        snprintf(out, outsz, "%s", path);
#else
    char buf[4096];
    if (realpath(path, buf))
        snprintf(out, outsz, "%s", buf);
    else
        snprintf(out, outsz, "%s", path);
#endif
}

void apex_patch_relpath(const char *patch_path, const char *path, char *out, size_t outsz) {
    char a[4096], d[4096];
    abs_path(path, a, sizeof(a));
    /* the patch file may not exist yet: resolve its directory */
    const char *b = apex_path_basename(patch_path);
    if (b == patch_path) {
        abs_path(".", d, sizeof(d));
    } else {
        char dir[4096];
        snprintf(dir, sizeof(dir), "%.*s", (int)(b - patch_path), patch_path);
        abs_path(dir, d, sizeof(d));
    }
    size_t dl = strlen(d);
    if (dl && !strncmp(a, d, dl) && (a[dl] == '/' || a[dl] == '\\'))
        snprintf(out, outsz, "%s", a + dl + 1);
    else
        snprintf(out, outsz, "%s", a);
}

void apex_patch_path(const ApexPatch *p, const char *rel, char *out, size_t outsz) {
    const char *base = p->path ? apex_path_basename(p->path) : NULL;
    if (!rel) {
        snprintf(out, outsz, "%s", "");
        return;
    }
    if (rel[0] == '/' || !p->path || base == p->path) {
        snprintf(out, outsz, "%s", rel);
        return;
    }
    snprintf(out, outsz, "%.*s%s", (int)(base - p->path), p->path, rel);
}

/* ── address and symbol resolution ─────────────────────────────────────── */

typedef struct {
    const char *name;
    uint8_t bank;
    uint32_t addr;
    int ambiguous;
} SymEntry;

typedef struct {
    SymEntry *items;
    size_t count;
} SymIndex;

static int sym_cmp(const void *a, const void *b) {
    return strcmp(((const SymEntry *)a)->name, ((const SymEntry *)b)->name);
}

static void sym_index_build(const ApexProject *p, SymIndex *ix) {
    size_t total = p->system_labels.count, n = 0, i, b;
    for (b = 0; b < p->banks; b++)
        total += p->bank_labels[b].count;
    ix->items = xmalloc((total ? total : 1u) * sizeof(*ix->items));
    /* Transient labels (the patch's own, shown in a patched view) are not
       part of the original's analysis: leave them out. */
    for (i = 0; i < p->system_labels.count; i++) {
        if (!apex_project_is_transient_label(p, 0xffu, p->system_labels.items[i].addr))
            ix->items[n++] = (SymEntry){p->system_labels.items[i].name, 0xffu,
                                        p->system_labels.items[i].addr, 0};
    }
    for (b = 0; b < p->banks; b++) {
        uint8_t id = bank_id_for_index(p->banks, (int)b);
        for (i = 0; i < p->bank_labels[b].count; i++) {
            if (!apex_project_is_transient_label(p, id, p->bank_labels[b].items[i].addr))
                ix->items[n++] = (SymEntry){p->bank_labels[b].items[i].name, id,
                                            p->bank_labels[b].items[i].addr, 0};
        }
    }
    qsort(ix->items, n, sizeof(*ix->items), sym_cmp);
    /* Collapse duplicates; one name at two places is ambiguous. */
    size_t w = 0;
    for (i = 0; i < n; i++) {
        if (w && !strcmp(ix->items[w - 1].name, ix->items[i].name)) {
            if (ix->items[w - 1].bank != ix->items[i].bank ||
                ix->items[w - 1].addr != ix->items[i].addr)
                ix->items[w - 1].ambiguous = 1;
            continue;
        }
        ix->items[w++] = ix->items[i];
    }
    ix->count = w;
}

static const SymEntry *sym_find(const SymIndex *ix, const char *name) {
    SymEntry key = {name, 0, 0, 0};
    return bsearch(&key, ix->items, ix->count, sizeof(*ix->items), sym_cmp);
}

static int parse_generated(const char *s, uint8_t *bank, uint32_t *addr) {
    char b[5] = {'0', 'x', 0, 0, 0}, a[7] = {'0', 'x', 0, 0, 0, 0, 0};
    uint32_t bv, av;
    if (strlen(s) != 9 || s[0] != 'B' || s[3] != '_' || s[4] != 'A')
        return 0;
    b[2] = s[1];
    b[3] = s[2];
    memcpy(a + 2, s + 5, 4);
    if (!parse_u32(b, &bv) || !parse_u32(a, &av))
        return 0;
    *bank = (uint8_t)bv;
    *addr = av;
    return 1;
}

static int resolve_addr_ix(const ApexProject *p, const SymIndex *ix, const char *expr,
                           uint8_t *bank, uint32_t *addr, char *err, size_t errsz) {
    char buf[160];
    long off = 0;
    snprintf(buf, sizeof(buf), "%s", expr);
    char *s = trim_ws(buf);
    /* label+N / label-N (the last sign that is not the first character) */
    char *sign = NULL;
    for (char *c = s + 1; *c; c++) {
        if (*c == '+' || *c == '-')
            sign = c;
    }
    if (sign) {
        uint32_t n;
        char sg = *sign;
        *sign = '\0';
        if (!parse_u32(trim_ws(sign + 1), &n)) {
            fmt_err(err, errsz, "invalid offset in '%s'", expr);
            return 0;
        }
        off = sg == '-' ? -(long)n : (long)n;
        s = trim_ws(s);
    }
    uint32_t v;
    const SymEntry *e = sym_find(ix, s);
    if (e) {
        if (e->ambiguous) {
            fmt_err(err, errsz, "label '%s' exists in several banks; use Bxx_Ayyyy", s);
            return 0;
        }
        *bank = e->bank;
        v = e->addr;
    } else if (parse_generated(s, bank, &v)) {
        /* generated form */
    } else if (parse_u32(s, &v)) {
        if (v < APEX_SYSTEM_ORG || v > 0xffffu) {
            fmt_err(err, errsz,
                    "'%s': a bare address must be in the system bank (0x8000-0xffff); "
                    "use Bxx_Ayyyy for paged banks",
                    expr);
            return 0;
        }
        *bank = 0xffu;
    } else {
        fmt_err(err, errsz, "unknown label '%s'", s);
        return 0;
    }
    long a = (long)v + off;
    if (*bank == 0xffu) {
        if (a < (long)APEX_SYSTEM_ORG || a > 0xffff) {
            fmt_err(err, errsz, "'%s' is outside the system bank", expr);
            return 0;
        }
    } else {
        if (bank_index_for_id(p->rom.data, p->banks, *bank) < 0) {
            fmt_err(err, errsz, "'%s': bank 0x%02x is not in this ROM", expr, *bank);
            return 0;
        }
        if (a < (long)APEX_PAGED_ORG || a >= (long)APEX_SYSTEM_ORG) {
            fmt_err(err, errsz, "'%s' is outside the paged window", expr);
            return 0;
        }
    }
    *addr = (uint32_t)a;
    return 1;
}

int apex_patch_resolve_addr(const ApexProject *p, const char *expr, uint8_t *bank, uint32_t *addr,
                            char *err, size_t errsz) {
    SymIndex ix;
    sym_index_build(p, &ix);
    int ok = resolve_addr_ix(p, &ix, expr, bank, addr, err, errsz);
    free(ix.items);
    return ok;
}

static size_t rom_offset(const ApexProject *p, uint8_t bank, uint32_t addr) {
    if (bank == 0xffu)
        return p->paged_size + (addr - APEX_SYSTEM_ORG);
    return (size_t)bank_index_for_id(p->rom.data, p->banks, bank) * APEX_BANK_SIZE +
           (addr - APEX_PAGED_ORG);
}

/* Labels defined by the patch's own asm ops. */
typedef struct {
    char *name;
    uint8_t bank;
    uint32_t addr;
} PatchLabel;

typedef struct {
    const ApexProject *p;
    const SymIndex *ix;
    PatchLabel *labels;
    size_t label_count;
    uint8_t code_bank; /* bank the snippet runs in */
    char foreign[96];  /* first label of another paged bank used (warning) */
    PatchLabel *ram;   /* RAM variables of the patch */
    size_t ram_count;
} ResolveCtx;

static int patch_resolve(void *vctx, const char *name, uint32_t *value, int *has_bank,
                         uint8_t *bank) {
    ResolveCtx *c = vctx;
    size_t i;
    for (i = 0; i < c->label_count; i++) {
        if (!strcmp(c->labels[i].name, name)) {
            *value = c->labels[i].addr;
            *has_bank = 1;
            *bank = c->labels[i].bank;
            return 1;
        }
    }
    for (i = 0; i < c->ram_count; i++) {
        if (!strcmp(c->ram[i].name, name)) {
            *value = c->ram[i].addr;
            return 1;
        }
    }
    const SymEntry *e = sym_find(c->ix, name);
    if (e && !e->ambiguous) {
        *value = e->addr;
        *has_bank = 1;
        *bank = e->bank;
        if (e->bank != 0xffu && e->bank != c->code_bank && !c->foreign[0])
            snprintf(c->foreign, sizeof(c->foreign), "%s (bank 0x%02x)", name, e->bank);
        return 1;
    }
    /* [symbols]: RAM / hardware equates */
    for (i = 0; i < c->p->symbols.count; i++) {
        if (!strcmp(c->p->symbols.items[i].name, name)) {
            *value = c->p->symbols.items[i].value;
            return 1;
        }
    }
    /* TYPE_VALUE enum equates, as the listing emits them */
    for (i = 0; i < c->p->config_types.count; i++) {
        const ConfigType *t = &c->p->config_types.items[i];
        size_t tl = strlen(t->name), j;
        if (ci_ncmp(name, t->name, tl) || name[tl] != '_')
            continue;
        for (j = 0; j < t->value_count; j++) {
            if (!ci_ncmp(name + tl + 1, t->values[j].name, (size_t)-1)) {
                *value = t->values[j].value;
                return 1;
            }
        }
    }
    return 0;
}

/* ── analysed-code boundaries ──────────────────────────────────────────── */

/* Length of the instruction at window offset pos, including the inline
   payload of a call to an [inline] routine.  0 if it does not decode. */
static size_t insn_len(const ApexProject *p, uint8_t bank, const uint8_t *data, size_t size,
                       uint32_t org, size_t pos) {
    char inst[64];
    Cpu6809InstrInfo info =
        cpu6809_disassemble_info(data + pos, size - pos, org + (uint32_t)pos, inst, sizeof(inst));
    size_t len = info.size;
    if (len && info.has_target && (info.flags & CPU6809_CALL)) {
        const InlineSignature *sig = inline_signature_for(&p->inline_sigs, bank, info.target);
        if (sig) {
            int ok;
            len += inline_actual_length(sig, data, pos + len, size, &ok);
            if (!ok)
                return 0;
        }
    }
    return len;
}

/* The original bytes being patched: the project's ROM unless a build gives
   another image (thread-local: set for the duration of a build). */
static APEX_THREAD_LOCAL const uint8_t *g_base;

static const uint8_t *base_bytes(const ApexProject *p) {
    return g_base ? g_base : p->rom.data;
}

static void bank_window(const ApexProject *p, uint8_t bank, const uint8_t **data, size_t *size,
                        uint32_t *org) {
    if (bank == 0xffu) {
        *data = base_bytes(p) + p->paged_size;
        *size = APEX_SYSTEM_SIZE;
        *org = APEX_SYSTEM_ORG;
    } else {
        *data =
            base_bytes(p) + (size_t)bank_index_for_id(p->rom.data, p->banks, bank) * APEX_BANK_SIZE;
        *size = APEX_BANK_SIZE;
        *org = APEX_PAGED_ORG;
    }
}

/* Is addr an instruction start of the analysed code?  1 yes, 0 no (inside an
   instruction), -1 unknown (no code label before it). */
static int insn_start(const ApexProject *p, uint8_t bank, uint32_t addr) {
    uint32_t start;
    const uint8_t *data;
    size_t size;
    uint32_t org;
    if (!apex_wpc_routine_at(p, bank, addr, &start))
        return -1;
    bank_window(p, bank, &data, &size, &org);
    size_t pos = start - org;
    while (org + pos < addr) {
        char inst[64];
        Cpu6809InstrInfo info = cpu6809_disassemble_info(data + pos, size - pos,
                                                         org + (uint32_t)pos, inst, sizeof(inst));
        size_t n = insn_len(p, bank, data, size, org, pos);
        if (!n || data_range_at(bank, org + (uint32_t)pos, &p->data_ranges) ||
            table_def_at(bank, org + (uint32_t)pos, &p->tables))
            return -1;
        pos += n;
        /* Flow ends before addr: addr is not reached from this label (code
           there would carry a label of its own). */
        if ((info.flags & CPU6809_FLOW_STOP) && org + pos < addr)
            return -1;
    }
    return org + pos == addr;
}

/* ── free space ────────────────────────────────────────────────────────── */

static void fs_add(ApexFreeSpace *fs, uint8_t bank, uint32_t addr, uint32_t len, int kind) {
    if (fs->count == fs->cap) {
        size_t ncap = fs->cap ? fs->cap * 2u : 32u;
        ApexFreeBlock *n = realloc(fs->items, ncap * sizeof(*n));
        if (!n)
            return;
        fs->items = n;
        fs->cap = ncap;
    }
    fs->items[fs->count++] = (ApexFreeBlock){bank, addr, len, kind};
}

void apex_patch_free_space_free(ApexFreeSpace *fs) {
    if (!fs)
        return;
    free(fs->items);
    memset(fs, 0, sizeof(*fs));
}

/* Is (bank, addr) a label, data range or table start the analysis knows (other
   than a transient patch label)? */
static int region_classified(const ApexProject *p, uint8_t bank, uint32_t start, uint32_t end) {
    const LabelSet *set = NULL;
    size_t i;
    if (bank == 0xffu) {
        set = &p->system_labels;
    } else {
        int bi = bank_index_for_id(p->rom.data, p->banks, bank);
        if (bi < 0)
            return 1;
        set = &p->bank_labels[bi];
    }
    for (i = 0; i < set->count; i++) {
        uint32_t a = set->items[i].addr;
        if (a >= start && a < end && !apex_project_is_transient_label(p, bank, a))
            return 1;
    }
    for (i = 0; i < p->data_ranges.count; i++) {
        const DataRange *r = &p->data_ranges.items[i];
        uint8_t rb = r->bank;
        uint32_t re = r->addr + (r->length ? (uint32_t)r->length : 1u);
        if (rb == bank && r->addr < end && re > start)
            return 1;
    }
    return 0;
}

void apex_patch_detect_free(const ApexProject *p, const uint8_t *base, ApexFreeSpace *out) {
    size_t w;
    memset(out, 0, sizeof(*out));
    for (w = 0; w <= p->banks; w++) {
        const uint8_t *data;
        uint32_t org, size, limit;
        uint8_t bank;
        if (w < p->banks) {
            data = base + w * APEX_BANK_SIZE;
            org = APEX_PAGED_ORG;
            size = APEX_BANK_SIZE;
            bank = bank_id_for_index(p->banks, (int)w);
        } else {
            data = base + p->paged_size;
            org = APEX_SYSTEM_ORG;
            size = APEX_SYSTEM_SIZE;
            bank = 0xffu;
        }
        /* never the bank byte, nor the system bank's vector/checksum area */
        limit = bank == 0xffu ? 0xffe0u - org : size;
        uint32_t i = 1;
        while (i < limit) {
            if (data[i] != 0xffu) {
                i++;
                continue;
            }
            uint32_t j = i;
            while (j < limit && data[j] == 0xffu)
                j++;
            int trailing = j == limit;
            /* keep the first 0xFF: it may end the data before it */
            uint32_t start = i + 1u;
            if (j > start && (trailing ? j - start >= 2u : j - i >= 64u) &&
                !region_classified(p, bank, org + start, org + j))
                fs_add(out, bank, org + start, j - start,
                       trailing ? APEX_FREE_TRAILING : APEX_FREE_INTERIOR);
            i = j;
        }
    }
}

/* Ledger of free intervals, carved up by the build. */
static void fs_sub(ApexFreeSpace *fs, uint8_t bank, uint32_t start, uint32_t end) {
    size_t i, n = fs->count;
    for (i = 0; i < n; i++) {
        ApexFreeBlock b = fs->items[i];
        uint32_t be = b.addr + b.len;
        if (b.bank != bank || b.len == 0 || end <= b.addr || start >= be)
            continue;
        /* keep the parts left and right of [start, end) */
        fs->items[i].len = start > b.addr ? start - b.addr : 0;
        if (end < be)
            fs_add(fs, bank, end, be - end, b.kind);
    }
}

static int fs_contains(const ApexFreeSpace *fs, uint8_t bank, uint32_t start, uint32_t end) {
    size_t i;
    for (i = 0; i < fs->count; i++) {
        const ApexFreeBlock *b = &fs->items[i];
        if (b->bank == bank && b->addr <= start && end <= b->addr + b->len)
            return 1;
    }
    return 0;
}

/* First block in the allowed banks (in their order) that fits size. */
static int fs_alloc(const ApexFreeSpace *fs, const uint8_t *banks, int nbanks, uint32_t size,
                    uint8_t *bank, uint32_t *addr) {
    int k;
    for (k = 0; k < nbanks; k++) {
        const ApexFreeBlock *best = NULL;
        size_t i;
        for (i = 0; i < fs->count; i++) {
            const ApexFreeBlock *b = &fs->items[i];
            if (b->bank == banks[k] && b->len >= size && (!best || b->addr < best->addr))
                best = b;
        }
        if (best) {
            *bank = best->bank;
            *addr = best->addr;
            return 1;
        }
    }
    return 0;
}

static void fs_copy(ApexFreeSpace *dst, const ApexFreeSpace *src) {
    size_t i;
    memset(dst, 0, sizeof(*dst));
    for (i = 0; i < src->count; i++) {
        if (src->items[i].len)
            fs_add(dst, src->items[i].bank, src->items[i].addr, src->items[i].len,
                   src->items[i].kind);
    }
}

/* ── build ─────────────────────────────────────────────────────────────── */

/* Ops that replace an image (and may relocate it). */
#define IMAGE_OP(t) ((t) == APEX_PATCH_DMD || (t) == APEX_PATCH_SPRITE)

typedef struct {
    int active; /* enabled, no error yet */
    uint8_t bank;
    uint32_t addr;
    int has_at;
    size_t len; /* bytes written at (bank, addr) */
    int alloc;  /* needs an allocated region */
    uint8_t abanks[64];
    int nab;
    uint32_t asize;
    uint8_t abank;
    uint32_t aaddr;
    size_t displaced; /* hook: original bytes moved into the detour */
    int jmp_back;     /* hook: the detour ends with JMP back */
    /* string / move */
    uint8_t *sbytes; /* string: the new bytes (with the terminator) */
    size_t slen;
    uint32_t old_len;  /* the old string / the moved block */
    uint32_t pool_end; /* string shift: end of the contiguous strings after it */
    int smode;         /* string: 0 in place, 1 shift, 2 relocate */
    int want;          /* string: requested mode, -1 = auto */
    /* sprite 0xFE: plane 1 lives behind a pointer and is rewritten there */
    size_t xoff;
    uint8_t *xbytes;
    size_t xlen;
} OpPlan;

typedef struct {
    const ApexPatch *patch;
    const ApexProject *p;
    SymIndex ix;
    ResolveCtx rc;
    OpPlan *plan;
} BuildCtx;

static void op_fail(ApexPatchOpResult *r, const char *fmt, ...) {
    va_list ap;
    r->status = APEX_PATCH_ERROR;
    va_start(ap, fmt);
    vsnprintf(r->msg, sizeof(r->msg), fmt, ap);
    va_end(ap);
}

static void op_warn(ApexPatchOpResult *r, const char *fmt, ...) {
    va_list ap;
    char m[200];
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    if (r->status == APEX_PATCH_OK) {
        r->status = APEX_PATCH_WARN;
        snprintf(r->msg, sizeof(r->msg), "%s", m);
    } else if (r->status == APEX_PATCH_WARN) {
        size_t l = strlen(r->msg);
        snprintf(r->msg + l, sizeof(r->msg) - l, "; %s", m);
    }
}

/* Source of an asm/hook op (inline code or file), heap string or NULL. */
static char *asm_source(const ApexPatch *patch, const ApexPatchOp *o, char *err, size_t errsz) {
    if (o->code)
        return pstrdup(o->code);
    if (!o->file)
        return pstrdup("");
    char path[1024];
    apex_patch_path(patch, o->file, path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fmt_err(err, errsz, "cannot open %s", path);
        return NULL;
    }
    fclose(fp);
    Buffer b = read_file(path);
    char *s = xmalloc(b.size + 1u);
    memcpy(s, b.data, b.size);
    s[b.size] = '\0';
    free(b.data);
    return s;
}

/* Assemble an op's source at (bank, addr).  sizing tolerates unresolved
   symbols.  Returns 0 and fills sn, else sets r's error. */
static int assemble_op(BuildCtx *c, const ApexPatchOp *o, ApexPatchOpResult *r, uint8_t bank,
                       uint32_t addr, int sizing, ApexAsmSnippet *sn) {
    char e[320];
    char *src = asm_source(c->patch, o, e, sizeof(e));
    if (!src) {
        op_fail(r, "%s", e);
        return -1;
    }
    c->rc.code_bank = bank;
    c->rc.foreign[0] = '\0';
    int rc = apex_asm_snippet(src, bank, addr, c->p->rom.size, patch_resolve, &c->rc, sizing, sn, e,
                              sizeof(e));
    free(src);
    if (rc != 0) {
        op_fail(r, "asm: %s", e);
        return -1;
    }
    if (!sizing && c->rc.foreign[0])
        op_warn(r, "uses %s, not mapped while this code runs (use a far call)", c->rc.foreign);
    return 0;
}

/* branch / redirect: the instruction at the site, rewritten.  Conditional
   branches: 0x22-0x2f (short), 0x10 0x22-0x2f (long); invert flips bit 0. */
static uint8_t *jump_bytes(BuildCtx *c, const ApexPatchOp *o, ApexPatchOpResult *r,
                           const uint8_t *ins, size_t avail, uint8_t bank, uint32_t addr,
                           int sizing, size_t *len) {
    char txt[64];
    Cpu6809InstrInfo in = cpu6809_disassemble_info(ins, avail, addr, txt, sizeof(txt));
    size_t n = in.size;
    int short_cond = n == 2 && ins[0] >= 0x22u && ins[0] <= 0x2fu;
    int long_cond = n == 4 && ins[0] == 0x10u && ins[1] >= 0x22u && ins[1] <= 0x2fu;
    if (!n) {
        op_fail(r, "no instruction at %s", o->at);
        return NULL;
    }
    uint8_t *b = xmalloc(4);
    memcpy(b, ins, n);
    *len = n;
    if (o->type == APEX_PATCH_BRANCH) {
        const char *m = o->mode ? o->mode : "always";
        if (!short_cond && !long_cond) {
            op_fail(r, "%s is no conditional branch (%s)", o->at, txt);
        } else if (!strcmp(m, "invert")) {
            b[short_cond ? 0 : 1] ^= 1u;
        } else if (!strcmp(m, "never")) {
            b[short_cond ? 0 : 1] = 0x21u; /* BRN / LBRN */
        } else if (!strcmp(m, "always")) {
            if (short_cond) {
                b[0] = 0x20u; /* BRA */
            } else {          /* LBcc (4 bytes) -> LBRA (3 bytes, offset + 1) + NOP */
                uint16_t rel = (uint16_t)(((ins[2] << 8) | ins[3]) + 1u);
                b[0] = 0x16u;
                b[1] = (uint8_t)(rel >> 8);
                b[2] = (uint8_t)rel;
                b[3] = PATCH_NOP;
            }
        } else {
            op_fail(r, "mode must be always, never or invert");
        }
        if (r->status == APEX_PATCH_ERROR) {
            free(b);
            return NULL;
        }
        return b;
    }
    /* redirect */
    uint8_t o0 = ins[0];
    int ext = n == 3 && (o0 == 0xbdu || o0 == 0x7eu);                  /* JSR / JMP extended */
    int rel8 = n == 2 && (o0 == 0x8du || o0 == 0x20u || short_cond);   /* BSR / BRA / Bcc */
    int rel16 = (n == 3 && (o0 == 0x17u || o0 == 0x16u)) || long_cond; /* LBSR / LBRA / LBcc */
    if (!ext && !rel8 && !rel16) {
        op_fail(r,
                "%s: %s has no direct target to redirect (JSR/JMP extended, BSR, BRA, "
                "LBSR, LBRA, Bcc, LBcc)",
                o->at, txt);
        free(b);
        return NULL;
    }
    if (sizing)
        return b; /* the patch's own labels are not placed yet */
    uint8_t tb = 0;
    uint32_t ta = 0;
    int hb = 0;
    char e[256];
    if (resolve_addr_ix(c->p, &c->ix, o->target, &tb, &ta, e, sizeof(e))) {
        hb = 1;
    } else if (!patch_resolve(&c->rc, o->target, &ta, &hb, &tb)) {
        op_fail(r, "target: %s", e);
        free(b);
        return NULL;
    }
    if (!hb)
        tb = ta >= APEX_SYSTEM_ORG ? 0xffu : bank;
    if (tb != 0xffu && tb != bank) {
        op_fail(r, "target %s is in bank %02x, not mapped while this code (bank %02x) runs",
                o->target, tb, bank);
        free(b);
        return NULL;
    }
    /* a call whose old target takes inline arguments (the bytes after the
       call): the new target must skip them too */
    if (in.has_target && (o0 == 0xbdu || o0 == 0x8du || o0 == 0x17u)) {
        const InlineSignature *was = inline_signature_for(&c->p->inline_sigs, bank, in.target);
        const InlineSignature *now = inline_signature_for(&c->p->inline_sigs, tb, ta);
        if (was && !now)
            op_warn(r,
                    "the old target takes inline argument byte(s) after the call; %s must "
                    "skip them as well",
                    o->target);
    }
    if (ext) {
        b[1] = (uint8_t)(ta >> 8);
        b[2] = (uint8_t)ta;
    } else if (rel8) {
        long d = (long)ta - (long)(addr + 2u);
        if (d < -128 || d > 127) {
            op_fail(r,
                    "target %s is %ld bytes away; a short branch reaches -128..127 (use a "
                    "long branch)",
                    o->target, d);
            free(b);
            return NULL;
        }
        b[1] = (uint8_t)(int8_t)d;
    } else {
        uint16_t d = (uint16_t)(ta - (addr + (uint32_t)n));
        b[n - 2] = (uint8_t)(d >> 8);
        b[n - 1] = (uint8_t)d;
    }
    return b;
}

/* Bytes a fixed-address bytes/nop/asm op writes (malloc'd) and how many
   original bytes it replaces. */
static uint8_t *op_bytes(BuildCtx *c, const ApexPatchOp *o, ApexPatchOpResult *r, uint8_t bank,
                         uint32_t addr, int sizing, size_t *len) {
    const ApexProject *p = c->p;
    const uint8_t *data;
    size_t size;
    uint32_t org;
    bank_window(p, bank, &data, &size, &org);
    size_t pos = addr - org;

    if (o->type == APEX_PATCH_BYTES) {
        uint8_t *b;
        long n = parse_hex_bytes(o->bytes, &b);
        if (n <= 0) {
            op_fail(r, "invalid bytes '%s'", o->bytes);
            return NULL;
        }
        *len = (size_t)n;
        return b;
    }

    /* Code ops start on an instruction boundary of the analysed code. */
    int st = insn_start(p, bank, addr);
    if (st == 0) {
        op_fail(r, "%s is inside an instruction", o->at);
        return NULL;
    }
    if (st < 0 && !sizing)
        op_warn(r, "%s is not inside analysed code", o->at);
    if (o->type == APEX_PATCH_BRANCH || o->type == APEX_PATCH_REDIRECT)
        return jump_bytes(c, o, r, data + pos, size - pos, bank, addr, sizing, len);

    uint8_t *code = NULL;
    size_t code_len = 0;
    if (o->type == APEX_PATCH_ASM) {
        ApexAsmSnippet sn;
        if (assemble_op(c, o, r, bank, addr, sizing, &sn) != 0)
            return NULL;
        code = sn.bytes;
        code_len = sn.size;
        sn.bytes = NULL;
        apex_asm_snippet_free(&sn);
    }

    /* The replaced region: given length, nop count, or the code size rounded
       up to the next original instruction boundary. */
    size_t region = 0;
    if (o->length) {
        region = o->length;
    } else if (o->type == APEX_PATCH_NOP) {
        unsigned k, n = o->count ? o->count : 1u;
        for (k = 0; k < n; k++) {
            size_t l = pos + region < size ? insn_len(p, bank, data, size, org, pos + region) : 0;
            if (!l) {
                op_fail(r, "cannot decode instruction %u at %s", k + 1u, o->at);
                free(code);
                return NULL;
            }
            region += l;
        }
    } else if (st < 0) {
        region = code_len; /* not analysed code (e.g. free space): exactly the code */
    } else {
        while (region < code_len) {
            size_t l = pos + region < size ? insn_len(p, bank, data, size, org, pos + region) : 0;
            if (!l) {
                region = code_len; /* undecodable: replace exactly the code */
                break;
            }
            region += l;
        }
    }
    if (pos + region > size) {
        op_fail(r, "%s + %lu bytes runs past the end of the bank", o->at, (unsigned long)region);
        free(code);
        return NULL;
    }
    if (code_len > region) {
        op_fail(r, "code is %lu bytes, but only %lu are replaced (length = %u)",
                (unsigned long)code_len, (unsigned long)region, o->length);
        free(code);
        return NULL;
    }
    if (!sizing && o->length && st > 0 && insn_start(p, bank, addr + (uint32_t)region) == 0)
        op_warn(r, "length %u ends inside an original instruction", o->length);
    uint8_t *b = xmalloc(region ? region : 1u);
    memset(b, PATCH_NOP, region);
    if (code_len)
        memcpy(b, code, code_len);
    if (!sizing && o->type == APEX_PATCH_ASM && region > code_len)
        op_warn(r, "%lu NOP byte(s) pad the replaced instructions",
                (unsigned long)(region - code_len));
    free(code);
    *len = region;
    return b;
}

/* PC-relative instructions cannot be moved into a detour unchanged. */
static int pc_relative(const uint8_t *op, const char *inst) {
    if (op[0] == 0x16u || op[0] == 0x17u || op[0] == 0x8du || (op[0] >= 0x20u && op[0] <= 0x2fu))
        return 1;
    if (op[0] == 0x10u && op[1] >= 0x20u && op[1] <= 0x2fu)
        return 1;
    return strstr(inst, ",PC") != NULL && strncmp(inst, "PSH", 3) && strncmp(inst, "PUL", 3);
}

/* A hook site: whole instructions from addr covering at least the 3 bytes of
   a JMP, none PC-relative, no other code path entering between them. */
static int hook_site(BuildCtx *c, const ApexPatchOp *o, ApexPatchOpResult *r, uint8_t bank,
                     uint32_t addr, size_t *displaced, int *jmp_back) {
    const ApexProject *p = c->p;
    const uint8_t *data;
    size_t size;
    uint32_t org;
    if (insn_start(p, bank, addr) != 1) {
        op_fail(r, "%s is not the start of an analysed instruction", o->at);
        return -1;
    }
    bank_window(p, bank, &data, &size, &org);
    size_t pos = addr - org, n = 0;
    *jmp_back = 1;
    while (n < 3u) {
        char inst[64];
        Cpu6809InstrInfo info = cpu6809_disassemble_info(
            data + pos + n, size - pos - n, org + (uint32_t)(pos + n), inst, sizeof(inst));
        size_t l = insn_len(p, bank, data, size, org, pos + n);
        if (!l) {
            op_fail(r, "cannot decode the instruction at %s+%lu", o->at, (unsigned long)n);
            return -1;
        }
        if (n > 0 && apex_wpc_routine_at(p, bank, org + (uint32_t)(pos + n), NULL)) {
            uint32_t ls;
            apex_wpc_routine_at(p, bank, org + (uint32_t)(pos + n), &ls);
            if (ls == org + pos + n) {
                op_fail(r, "B%02x_A%04x inside the replaced instructions is a jump target", bank,
                        (unsigned)(org + pos + n));
                return -1;
            }
        }
        if (pc_relative(data + pos + n, inst)) {
            op_fail(r, "'%s' is PC-relative and cannot move into the detour", inst);
            return -1;
        }
        n += l;
        if (info.flags & CPU6809_FLOW_STOP) {
            *jmp_back = 0; /* RTS/JMP: the detour ends there */
            if (n < 3u) {
                op_fail(r, "only %lu byte(s) before the flow ends; a JMP needs 3",
                        (unsigned long)n);
                return -1;
            }
        }
    }
    *displaced = n;
    return 0;
}

/* Banks an allocation may use, in preference order. */
static int alloc_banks(const ApexProject *p, const ApexPatchOp *o, int has_site, uint8_t site,
                       uint8_t *banks, int *nb, char *err, size_t errsz) {
    const char *a = o->alloc;
    *nb = 0;
    if (!a || !strcmp(a, "same")) {
        if (!has_site || site == 0xffu) {
            banks[(*nb)++] = 0xffu;
        } else {
            banks[(*nb)++] = site;
            banks[(*nb)++] = 0xffu;
        }
    } else if (!strcmp(a, "system")) {
        banks[(*nb)++] = 0xffu;
    } else if (!strcmp(a, "any")) {
        for (size_t b = 0; b < p->banks && *nb < 63; b++)
            banks[(*nb)++] = bank_id_for_index(p->banks, (int)b);
        banks[(*nb)++] = 0xffu;
    } else if (!strncmp(a, "bank:", 5)) {
        char hx[16];
        uint32_t v;
        snprintf(hx, sizeof(hx), "0x%s", a + 5 + (a[5] == '0' && a[6] == 'x' ? 2 : 0));
        if (!parse_u32(hx, &v) || v > 0xffu ||
            (v != 0xffu && bank_index_for_id(p->rom.data, p->banks, (uint8_t)v) < 0)) {
            fmt_err(err, errsz, "alloc '%s': no such bank", a);
            return -1;
        }
        banks[(*nb)++] = (uint8_t)v;
    } else {
        fmt_err(err, errsz, "alloc must be system, any, same or bank:NN (not '%s')", a);
        return -1;
    }
    /* a detour must run in the site's bank or the always-mapped system bank */
    if (o->type == APEX_PATCH_HOOK && has_site) {
        for (int k = 0; k < *nb; k++) {
            if (banks[k] != 0xffu && banks[k] != site) {
                fmt_err(err, errsz,
                        "a hook in bank 0x%02x needs its code in that bank or the system bank",
                        site);
                return -1;
            }
        }
    }
    return 0;
}

static int sym_push(ApexPatchBuild *out, size_t *cap, const char *name, uint8_t bank, uint32_t addr,
                    int is_code, int is_ram, int starts_code) {
    if (out->sym_count == *cap) {
        size_t ncap = *cap ? *cap * 2u : 32u;
        ApexPatchSym *n = realloc(out->syms, ncap * sizeof(*n));
        if (!n)
            return -1;
        out->syms = n;
        *cap = ncap;
    }
    ApexPatchSym *s = &out->syms[out->sym_count++];
    s->name = pstrdup(name);
    s->bank = bank;
    s->addr = addr;
    s->is_code = is_code;
    s->is_ram = is_ram;
    s->starts_code = starts_code;
    return 0;
}

/* RAM address: a number or a [symbols] name, below 0x4000. */
static int ram_addr(const ApexProject *p, const char *expr, uint32_t *addr) {
    size_t i;
    if (parse_u32(expr, addr))
        return *addr < APEX_PAGED_ORG;
    for (i = 0; i < p->symbols.count; i++) {
        if (!strcmp(p->symbols.items[i].name, expr)) {
            *addr = p->symbols.items[i].value;
            return *addr < APEX_PAGED_ORG;
        }
    }
    return 0;
}

/* Write len bytes at rom_off for op i: overlap check, then copy. */
static int claim_bytes(const ApexPatch *patch, uint16_t *owner, ApexPatchOpResult *r, size_t i,
                       size_t rom_off, size_t len) {
    size_t k;
    for (k = 0; k < len; k++) {
        uint16_t ow = owner[rom_off + k];
        if (ow && ow != (uint16_t)(i + 1u)) {
            op_fail(r, "overlaps op '%s' at file offset 0x%06lx", patch->ops[ow - 1u].name,
                    (unsigned long)(rom_off + k));
            return -1;
        }
    }
    for (k = 0; k < len; k++)
        owner[rom_off + k] = (uint16_t)(i + 1u);
    return 0;
}

static int check_expect(const ApexProject *p, const ApexPatchOp *o, ApexPatchOpResult *r,
                        size_t len) {
    if (!o->expect) {
        op_warn(r, "no expect guard (apexpatch pin adds it)");
        return 0;
    }
    uint8_t *ex;
    long n = parse_hex_bytes(o->expect, &ex);
    if (n < 0) {
        op_fail(r, "invalid expect '%s'", o->expect);
        return -1;
    }
    int bad = 0;
    if ((size_t)n != len) {
        op_fail(r, "expect has %ld byte(s), the op replaces %lu", n, (unsigned long)len);
        bad = 1;
    } else if (memcmp(ex, base_bytes(p) + r->rom_off, len) != 0) {
        char *have = hex_text(base_bytes(p) + r->rom_off, len);
        op_fail(r, "original bytes are %s, expect says %s", have, o->expect);
        free(have);
        bad = 1;
    }
    free(ex);
    return bad ? -1 : 0;
}

/* ── data ops: strings, values, moves, reference rewriting ─────────────── */

/* New string bytes: text with \n \t \a \\ escapes, plus the terminating 0.
   Returns the length, or 0 (with err) for a character a string cannot hold. */
static size_t string_bytes(const char *text, uint8_t **out, char *err, size_t errsz) {
    size_t n = 0;
    uint8_t *b = xmalloc(strlen(text) + 1u);
    for (const char *t = text; *t; t++) {
        uint8_t ch = (uint8_t)*t;
        if (*t == '\\' && t[1]) {
            t++;
            ch = *t == 'n' ? 0x0au : *t == 't' ? 0x09u : *t == 'a' ? 0x07u : (uint8_t)*t;
        }
        if (!(ch == 0x0au || ch == 0x09u || ch == 0x07u || (ch >= 0x20u && ch <= 0x7fu))) {
            fmt_err(err, errsz, "character 0x%02x cannot be part of a string", ch);
            free(b);
            return 0;
        }
        b[n++] = ch;
    }
    b[n++] = 0x00u;
    *out = b;
    return n;
}

/* Bytes of `planes` consecutive encoded DMD fullframes, 0 if they do not
   decode. */
static uint32_t dmd_size(const uint8_t *src, size_t avail, unsigned planes) {
    uint8_t page[APEX_DMD_PAGE_BYTES];
    size_t pos = 0;
    for (unsigned k = 0; k < planes; k++) {
        size_t used = 0;
        if (pos >= avail || !apexdmd_decode_fullframe(src + pos, avail - pos, page, &used, NULL))
            return 0;
        pos += used;
    }
    return (uint32_t)pos;
}

/* Length of the table / data range / string that starts at (bank, addr). */
static uint32_t block_length(const ApexProject *p, uint8_t bank, uint32_t addr) {
    const uint8_t *data;
    size_t size;
    uint32_t org;
    bank_window(p, bank, &data, &size, &org);
    size_t pos = addr - org;
    const TableDef *t = table_def_at(bank, addr, &p->tables);
    if (t && t->addr == addr) {
        if (t->has_header)
            return pos + 3u <= size ? 3u + (uint32_t)read_be16(data + pos) * data[pos + 2u] : 0;
        return (uint32_t)(t->rows * table_schema_width(&t->schema));
    }
    const DataRange *r = data_range_at(bank, addr, &p->data_ranges);
    if (r && r->addr == addr) {
        switch (r->kind) {
        case DATA_BYTES:
        case DATA_STRING_FIXED:
        case DATA_BCD:
            return (uint32_t)r->length;
        case DATA_DMD_FULLFRAME:
            return dmd_size(data + pos, size - pos, r->length ? (unsigned)r->length : 1u);
        case DATA_STRING:
            break;
        default:
            return 0; /* pointers, sprites: the size is not known here */
        }
    }
    return (uint32_t)valid_string_len(data + pos, size - pos);
}

/* Width of the row at row_addr of a table in bank (the nearest table start at
   or before it). */
static size_t row_width_at(const ApexProject *p, uint8_t bank, uint32_t row_addr) {
    const TableDef *best = NULL;
    size_t i;
    for (i = 0; i < p->tables.count; i++) {
        const TableDef *t = &p->tables.items[i];
        if (t->bank == bank && t->addr <= row_addr && (!best || t->addr > best->addr))
            best = t;
    }
    if (!best)
        return 0;
    if (best->has_header) {
        const uint8_t *data;
        size_t size;
        uint32_t org;
        bank_window(p, bank, &data, &size, &org);
        size_t pos = best->addr - org;
        return pos + 3u <= size ? data[pos + 2u] : 0;
    }
    return table_schema_width(&best->schema);
}

/* Bytes of the item that holds a reference: the table row, or the
   instruction (with its inline payload). */
static int ref_source_range(const ApexProject *p, const Reference *r, uint32_t *start,
                            uint32_t *len) {
    const uint8_t *data;
    size_t size;
    uint32_t org;
    if (r->source_bank != 0xffu && bank_index_for_id(p->rom.data, p->banks, r->source_bank) < 0)
        return 0;
    bank_window(p, r->source_bank, &data, &size, &org);
    if (r->row_index >= 0) {
        size_t w = row_width_at(p, r->source_bank, r->row_cpu_addr);
        if (!w)
            return 0;
        *start = r->row_cpu_addr;
        *len = (uint32_t)w;
    } else {
        if (r->source_addr < org || r->source_addr >= org + size)
            return 0;
        size_t l = insn_len(p, r->source_bank, data, size, org, r->source_addr - org);
        if (!l)
            return 0;
        *start = r->source_addr;
        *len = (uint32_t)l;
    }
    return *start >= org && *start + *len <= org + size;
}

/* 1 if (bank, addr) has known references and every one is a far pointer
   (address + bank byte): such data may move to any bank. */
static int refs_far_only(const ApexProject *p, uint8_t bank, uint32_t addr) {
    size_t i, n = 0;
    for (i = 0; i < p->refs.count; i++) {
        const Reference *r = &p->refs.items[i];
        uint32_t st, ln, k;
        int far = 0;
        if (r->bank != bank || r->addr != addr)
            continue;
        n++;
        if (!ref_source_range(p, r, &st, &ln))
            return 0;
        const uint8_t *b = base_bytes(p) + rom_offset(p, r->source_bank, st);
        for (k = 0; k + 2u < ln; k++)
            far = far ||
                  (b[k] == (uint8_t)(addr >> 8) && b[k + 1u] == (uint8_t)addr && b[k + 2u] == bank);
        if (!far)
            return 0;
    }
    return n > 0;
}

/* Banks for relocated data: the allocation spec, else the data's own bank --
   plus every other bank when all references are far pointers. */
static void data_banks(const ApexProject *p, const ApexPatchOp *o, OpPlan *pl) {
    if (o->alloc)
        return; /* alloc_banks() already filled the list */
    pl->abanks[0] = pl->bank;
    pl->nab = 1;
    if (!refs_far_only(p, pl->bank, pl->addr))
        return;
    for (size_t b = 0; b < p->banks && pl->nab < 63; b++) {
        uint8_t id = bank_id_for_index(p->banks, (int)b);
        if (id != pl->bank)
            pl->abanks[pl->nab++] = id;
    }
    if (pl->bank != 0xffu)
        pl->abanks[pl->nab++] = 0xffu;
}

/* An image op's new bytes (pl->sbytes, slen) replace old_len bytes: in place
   when they fit (or mode = inplace), else relocated to free space with the
   references rewritten (mode = relocate forces it). */
static void plan_replace(const ApexProject *p, const ApexPatchOp *o, OpPlan *pl,
                         ApexPatchOpResult *r) {
    char e[256];
    const char *m = o->mode ? o->mode : "auto";
    int want = !strcmp(m, "inplace") ? 0 : !strcmp(m, "relocate") ? 2 : -1;
    if (strcmp(m, "auto") && want < 0) {
        op_fail(r, "mode must be auto, inplace or relocate");
        return;
    }
    if (want == 0 && pl->slen > pl->old_len) {
        op_fail(r, "the new image needs %lu byte(s), the old one has %u", (unsigned long)pl->slen,
                (unsigned)pl->old_len);
        return;
    }
    if (want != 2 && pl->slen <= pl->old_len) {
        pl->smode = 0;
        pl->len = pl->slen;
        return;
    }
    pl->smode = 2;
    if (o->alloc && alloc_banks(p, o, 1, pl->bank, pl->abanks, &pl->nab, e, sizeof(e)) != 0) {
        op_fail(r, "%s", e);
        return;
    }
    data_banks(p, o, pl); /* far-referenced images may go to any bank */
    pl->asize = (uint32_t)pl->slen;
    pl->alloc = 1;
}

/* Sprite planes from a grey image, MSB first (bit 7 = leftmost pixel); for
   two planes `bright` lights level 2 (see apexdmd_planes_from_gray). */
static void sprite_planes(const uint8_t *gray, unsigned w, unsigned h, int nplanes, int bright,
                          uint8_t *p0, uint8_t *p1) {
    size_t rb = (w + 7u) / 8u;
    memset(p0, 0, rb * h);
    if (p1)
        memset(p1, 0, rb * h);
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            unsigned v = gray[(size_t)y * w + x];
            size_t byte = y * rb + x / 8u;
            uint8_t bit = (uint8_t)(0x80u >> (x % 8u));
            if (nplanes < 2) {
                if (v >= 128u)
                    p0[byte] |= bit;
                continue;
            }
            unsigned level = (v * 3u + 127u) / 255u;
            uint8_t *dim = bright ? p0 : p1, *brt = bright ? p1 : p0;
            if (level & 1u)
                dim[byte] |= bit;
            if (level & 2u)
                brt[byte] |= bit;
        }
    }
}

static void plan_sprite(const ApexPatch *patch, const ApexProject *p, const ApexPatchOp *o,
                        OpPlan *pl, ApexPatchOpResult *r) {
    char path[1024], e[256];
    const uint8_t *data;
    size_t size;
    uint32_t org;
    bank_window(p, pl->bank, &data, &size, &org);
    const uint8_t *src = data + (pl->addr - org);
    size_t avail = size - (pl->addr - org);
    const DataRange *dr = data_range_at(pl->bank, pl->addr, &p->data_ranges);
    int noheader = dr && dr->addr == pl->addr && dr->kind == DATA_SPRITE_NOHEADER;
    uint8_t tmp[APEX_SPRITE_MAX_BYTES], hdr = 0, vo = 0, ho = 0, ow = 0, oh = 0, enc = 0;
    size_t used = 0;
    if (noheader) {
        oh = (uint8_t)dr->length;
        if (!apexsprite_decode_noheader(src, avail, tmp, oh, &ow, &used)) {
            op_fail(r, "no sprite (without header) at %s", o->at);
            return;
        }
    } else if (!apexsprite_decode(src, avail, tmp, &hdr, &vo, &ho, &ow, &oh, &enc, &used)) {
        op_fail(r, "no sprite at %s", o->at);
        return;
    }
    pl->old_len = (uint32_t)used;
    apex_patch_path(patch, o->file, path, sizeof(path));
    uint8_t *gray;
    unsigned w, h;
    if (!apex_image_read_gray(path, &gray, &w, &h, e, sizeof(e))) {
        op_fail(r, "%s", e);
        return;
    }
    if (w < 1 || h < 1 || w > APEX_SPRITE_MAX_WIDTH || h > APEX_SPRITE_MAX_HEIGHT) {
        free(gray);
        op_fail(r, "%s is %ux%u; a sprite is at most %ux%u", path, w, h, APEX_SPRITE_MAX_WIDTH,
                APEX_SPRITE_MAX_HEIGHT);
        return;
    }
    if (noheader && h != oh) {
        free(gray);
        op_fail(r, "%s is %u pixel(s) high; this sprite table's images are %u", path, h, oh);
        return;
    }
    int two = !noheader && (hdr == 0xFEu || hdr == 0xFFu);
    if (two && hdr == 0xFEu && (w != ow || h != oh)) {
        free(gray);
        op_fail(r,
                "%s is %ux%u: a 0xFE sprite keeps its size (%ux%u), its second plane is "
                "shared through a pointer",
                path, w, h, ow, oh);
        return;
    }
    size_t page = (w + 7u) / 8u * h;
    uint8_t p0[APEX_SPRITE_MAX_BYTES], p1[APEX_SPRITE_MAX_BYTES];
    sprite_planes(gray, w, h, two ? 2 : 1, o->bright >= 0 ? o->bright : 1, p0, two ? p1 : NULL);
    free(gray);
    pl->sbytes = xmalloc(5u + 2u * page);
    if (noheader) {
        pl->sbytes[0] = (uint8_t)w;
        memcpy(pl->sbytes + 1, p0, page);
        pl->slen = 1u + page;
    } else {
        uint8_t hd[5] = {hdr, vo, ho, (uint8_t)h, (uint8_t)w};
        memcpy(pl->sbytes, hd, 5);
        pl->slen = 5;
        if (hdr == 0xFFu) {
            memcpy(pl->sbytes + pl->slen, p1, page);
            pl->slen += page;
        } else if (hdr == 0xFEu) {
            memcpy(pl->sbytes + pl->slen, src + 5, 2); /* the plane-1 pointer stays */
            pl->slen += 2;
            uint32_t p1a = ((uint32_t)src[5] << 8) | src[6];
            uint8_t p1b = p1a >= APEX_SYSTEM_ORG ? 0xffu : pl->bank;
            pl->xoff = rom_offset(p, p1b, p1a);
            if (pl->xoff + page > p->rom.size) {
                op_fail(r, "the second plane's pointer 0x%04x leaves the ROM", (unsigned)p1a);
                return;
            }
            pl->xbytes = xmalloc(page);
            memcpy(pl->xbytes, p1, page);
            pl->xlen = page;
        }
        memcpy(pl->sbytes + pl->slen, p0, page);
        pl->slen += page;
    }
    plan_replace(p, o, pl, r);
}

static ApexPatchFixup *fixup_push(ApexPatchBuild *out, size_t *cap) {
    if (out->fixup_count == *cap) {
        size_t ncap = *cap ? *cap * 2u : 32u;
        ApexPatchFixup *n = realloc(out->fixups, ncap * sizeof(*n));
        if (!n)
            return NULL;
        out->fixups = n;
        *cap = ncap;
    }
    ApexPatchFixup *f = &out->fixups[out->fixup_count++];
    memset(f, 0, sizeof(*f));
    return f;
}

/* Rewrite every known reference to [old, old+len) in bank old_bank so it
   points at the same offset from new_addr in new_bank.  References from
   inside [skip_start, skip_end) of the same bank (the moved block itself) are
   rewritten in `copy` (the block's new bytes) instead of the image.  Returns
   the number of references that need a manual fix. */
static int rewrite_refs(BuildCtx *c, ApexPatchBuild *out, size_t *fcap, uint16_t *owner, size_t opi,
                        uint8_t old_bank, uint32_t old, uint32_t len, uint8_t new_bank,
                        uint32_t new_addr, uint32_t skip_start, uint32_t skip_end, uint8_t *copy) {
    const ApexProject *p = c->p;
    const ReferenceSet *refs = &p->refs;
    int manual = 0;
    size_t i;
    for (i = 0; i < refs->count; i++) {
        const Reference *r = &refs->items[i];
        if (r->bank != old_bank || r->addr < old || r->addr >= old + len)
            continue;
        uint32_t target = r->addr, moved = new_addr + (r->addr - old);
        ApexPatchFixup *f = fixup_push(out, fcap);
        if (!f)
            break;
        f->src_bank = r->source_bank;
        f->src_addr = r->row_index >= 0 ? r->row_cpu_addr : r->source_addr;
        f->old_bank = old_bank;
        f->old_addr = target;
        f->new_bank = new_bank;
        f->new_addr = moved;
        if (r->row_index >= 0)
            snprintf(f->what, sizeof(f->what), "table row %d", r->row_index);
        else
            snprintf(f->what, sizeof(f->what), "code");
        uint32_t st, ln;
        if (!ref_source_range(p, r, &st, &ln)) {
            f->status = APEX_FIXUP_MANUAL;
            manual++;
            continue;
        }
        /* the old address in the item: far (addr + bank byte) first, else word */
        const uint8_t *base = base_bytes(p) + rom_offset(p, r->source_bank, st);
        uint8_t hi = (uint8_t)(target >> 8), lo = (uint8_t)target;
        int far_at = -1, word_at = -1, far_n = 0, word_n = 0;
        uint32_t k;
        for (k = 0; k + 1u < ln; k++) {
            if (base[k] != hi || base[k + 1u] != lo)
                continue;
            word_n++;
            word_at = (int)k;
            if (k + 2u < ln && base[k + 2u] == old_bank) {
                far_n++;
                far_at = (int)k;
            }
        }
        int at = -1;
        if (far_n == 1) {
            f->far = 1;
            at = far_at;
        } else if (word_n == 1) {
            at = word_at;
        }
        if (at < 0) {
            f->status = APEX_FIXUP_MANUAL;
            manual++;
            continue;
        }
        if (!f->far && new_bank != old_bank) {
            /* a 16-bit pointer only reaches the bank that is mapped where it is
               used: the target must stay in its bank */
            f->status = APEX_FIXUP_UNREACHABLE;
            manual++;
            continue;
        }
        uint32_t loc = st + (uint32_t)at;
        uint8_t nb[3] = {(uint8_t)(moved >> 8), (uint8_t)moved, new_bank};
        size_t nlen = f->far ? 3u : 2u;
        if (copy && r->source_bank == old_bank && loc >= skip_start && loc < skip_end) {
            memcpy(copy + (loc - skip_start), nb, nlen);
            f->rom_off = rom_offset(p, new_bank, new_addr + (loc - skip_start));
        } else {
            f->rom_off = rom_offset(p, r->source_bank, loc);
            ApexPatchOpResult *res = &out->ops[opi];
            if (claim_bytes(c->patch, owner, res, opi, f->rom_off, nlen) != 0)
                return manual;
            memcpy(out->rom + f->rom_off, nb, nlen);
        }
    }
    return manual;
}

/* value: number, a type's enum name, an equate / label (resolver). */
static int value_eval(BuildCtx *c, const ApexPatchOp *o, uint32_t *v, uint8_t *bank, int *has_bank,
                      char *err, size_t errsz) {
    *has_bank = 0;
    if (parse_u32(o->value, v))
        return 1;
    if (o->vtype) {
        const ConfigType *t = find_config_type(&c->p->config_types, o->vtype);
        if (!t) {
            fmt_err(err, errsz, "unknown type '%s'", o->vtype);
            return 0;
        }
        for (size_t j = 0; j < t->value_count; j++) {
            if (!strcmp(t->values[j].name, o->value)) {
                *v = t->values[j].value;
                return 1;
            }
        }
    }
    if (patch_resolve(&c->rc, o->value, v, has_bank, bank))
        return 1;
    fmt_err(err, errsz, "'%s' is no number, %s value, equate or label", o->value,
            o->vtype ? o->vtype : "enum");
    return 0;
}

int apex_patch_build(const ApexPatch *patch, const ApexProject *p, ApexPatchBuild *out) {
    return apex_patch_build_base(patch, p, p->rom.data, p->rom.size, out);
}

static int build_impl(const ApexPatch *patch, const ApexProject *p, ApexPatchBuild *out);
static APEX_THREAD_LOCAL const ApexFreeSpace *g_free_hint;

int apex_patch_build_base(const ApexPatch *patch, const ApexProject *p, const uint8_t *base,
                          size_t base_size, ApexPatchBuild *out) {
    return apex_patch_build_free_hint(patch, p, base, base_size, NULL, out);
}

int apex_patch_build_free_hint(const ApexPatch *patch, const ApexProject *p, const uint8_t *base,
                               size_t base_size, const ApexFreeSpace *detected,
                               ApexPatchBuild *out) {
    if (base_size != p->rom.size) {
        memset(out, 0, sizeof(*out));
        snprintf(out->msg, sizeof(out->msg), "base image size differs from the project ROM");
        out->errors = 1;
        return 1;
    }
    const uint8_t *prev = g_base;
    const ApexFreeSpace *prev_hint = g_free_hint;
    g_base = base;
    g_free_hint = detected;
    int rc = build_impl(patch, p, out);
    g_base = prev;
    g_free_hint = prev_hint;
    return rc;
}

static int build_impl(const ApexPatch *patch, const ApexProject *p, ApexPatchBuild *out) {
    size_t i, j, sym_cap = 0, fixup_cap = 0;
    BuildCtx c;

    memset(out, 0, sizeof(*out));
    out->count = patch->count;
    out->ops = calloc(patch->count ? patch->count : 1u, sizeof(*out->ops));
    if (!out->ops)
        return -1;
    if (patch->has_crc) {
        uint32_t crc = ari_crc32(base_bytes(p), p->rom.size);
        if (crc != patch->base_crc32) {
            snprintf(out->msg, sizeof(out->msg),
                     "base ROM CRC-32 is %08X, the patch expects %08X (different ROM version?)",
                     (unsigned)crc, (unsigned)patch->base_crc32);
            out->errors = 1;
            return 1;
        }
    }
    out->rom_size = p->rom.size;
    out->rom = xmalloc(p->rom.size);
    memcpy(out->rom, base_bytes(p), p->rom.size);
    uint16_t *owner = calloc(p->rom.size, sizeof(uint16_t)); /* op index + 1 per byte */
    OpPlan *plan = calloc(patch->count ? patch->count : 1u, sizeof(OpPlan));
    if (!owner || !plan) {
        free(owner);
        free(plan);
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.patch = patch;
    c.p = p;
    c.plan = plan;
    sym_index_build(p, &c.ix);
    c.rc.p = p;
    c.rc.ix = &c.ix;

    /* ---- 1. addresses and sizes; RAM variables; declared free/reserved ---- */
    ApexFreeSpace ledger;
    if (g_free_hint)
        fs_copy(&ledger, g_free_hint);
    else
        apex_patch_detect_free(p, base_bytes(p), &ledger);
    for (i = 0; i < patch->count; i++) {
        const ApexPatchOp *o = &patch->ops[i];
        ApexPatchOpResult *r = &out->ops[i];
        OpPlan *pl = &plan[i];
        char e[256];
        if (!o->enabled) {
            r->status = APEX_PATCH_DISABLED;
            continue;
        }
        if (op_problem(o, e, sizeof(e))) {
            op_fail(r, "%s", e); /* not active: no later phase touches it */
            out->errors++;
            continue;
        }
        pl->active = 1;
        if (o->type == APEX_PATCH_RAM) {
            uint32_t a;
            if (!ram_addr(p, o->at, &a)) {
                op_fail(r, "'%s' is not a RAM address (0x0000-0x3fff or a [symbols] name)", o->at);
            } else {
                pl->addr = a;
                pl->len = o->length ? o->length : 1u;
                for (j = 0; j < i; j++) {
                    if (plan[j].active && patch->ops[j].type == APEX_PATCH_RAM &&
                        a < plan[j].addr + plan[j].len && plan[j].addr < a + pl->len)
                        op_fail(r, "RAM 0x%04x overlaps RAM variable '%s'", (unsigned)a,
                                patch->ops[j].label);
                }
                for (j = 0; j < p->symbols.count; j++) {
                    const ConfigSymbol *sy = &p->symbols.items[j];
                    uint32_t sl = sy->length ? sy->length : 1u;
                    if (sy->value < a + pl->len && a < sy->value + sl) {
                        op_warn(r, "RAM 0x%04x is the game's '%s'", (unsigned)sy->value, sy->name);
                        break;
                    }
                }
            }
            if (r->status != APEX_PATCH_ERROR) {
                PatchLabel *nl = realloc(c.rc.ram, (c.rc.ram_count + 1u) * sizeof(*nl));
                if (nl) {
                    c.rc.ram = nl;
                    nl[c.rc.ram_count].name = pstrdup(o->label);
                    nl[c.rc.ram_count].bank = 0;
                    nl[c.rc.ram_count].addr = pl->addr;
                    c.rc.ram_count++;
                }
            }
            continue;
        }
        if (o->at && !(o->type == APEX_PATCH_ASM && o->alloc)) {
            if (!resolve_addr_ix(p, &c.ix, o->at, &pl->bank, &pl->addr, e, sizeof(e))) {
                op_fail(r, "%s", e);
                continue;
            }
            pl->has_at = 1;
        }
        if (o->type == APEX_PATCH_FREE || o->type == APEX_PATCH_RESERVE) {
            if (o->type == APEX_PATCH_FREE)
                fs_add(&ledger, pl->bank, pl->addr, o->length, APEX_FREE_DECLARED);
            continue; /* reserves are taken out below, after all frees */
        }
        if (o->type == APEX_PATCH_VALUE) {
            pl->len = o->width ? o->width : 1u;
            continue;
        }
        if (o->type == APEX_PATCH_STRING) {
            const uint8_t *data;
            size_t size;
            uint32_t org;
            bank_window(p, pl->bank, &data, &size, &org);
            size_t pos = pl->addr - org;
            pl->old_len = (uint32_t)valid_string_len(data + pos, size - pos);
            if (!pl->old_len) {
                op_fail(r, "no string at %s", o->at);
                continue;
            }
            pl->slen = string_bytes(o->text, &pl->sbytes, e, sizeof(e));
            if (!pl->slen) {
                op_fail(r, "%s", e);
                continue;
            }
            const char *m = o->mode ? o->mode : "auto";
            pl->want = !strcmp(m, "inplace")    ? 0
                       : !strcmp(m, "shift")    ? 1
                       : !strcmp(m, "relocate") ? 2
                       : !strcmp(m, "auto")     ? -1
                                                : -2;
            if (pl->want == -2) {
                op_fail(r, "mode must be auto, inplace, shift or relocate");
                continue;
            }
            int interior = 0;
            for (j = 0; j < p->refs.count; j++) {
                const Reference *rf = &p->refs.items[j];
                interior = interior || (rf->bank == pl->bank && rf->addr > pl->addr &&
                                        rf->addr < pl->addr + pl->old_len);
            }
            if (pl->want == 0 || (pl->want < 0 && pl->slen <= pl->old_len && !interior)) {
                if (pl->slen > pl->old_len) {
                    op_fail(r, "the new text needs %lu byte(s), the old one has %u",
                            (unsigned long)pl->slen, (unsigned)pl->old_len);
                    continue;
                }
                if (interior)
                    op_warn(r, "code points into the middle of the old string");
                pl->smode = 0;
                pl->len = pl->slen;
                continue;
            }
            /* the strings right behind it (each starting at a label) */
            const LabelSet *set =
                pl->bank == 0xffu
                    ? &p->system_labels
                    : &p->bank_labels[bank_index_for_id(p->rom.data, p->banks, pl->bank)];
            size_t q = pos + pl->old_len;
            while (q < size && label_set_name_at((LabelSet *)set, org + (uint32_t)q)) {
                size_t l = valid_string_len(data + q, size - q);
                if (!l)
                    break;
                q += l;
            }
            pl->pool_end = org + (uint32_t)q;
            pl->smode = (pl->want == 2 || pl->slen <= pl->old_len) ? 2 : 1;
            if (pl->smode == 2 || pl->want < 0) {
                /* relocation (decided now, or the fallback of a shift) */
                if (alloc_banks(p, o, 1, pl->bank, pl->abanks, &pl->nab, e, sizeof(e)) != 0) {
                    op_fail(r, "%s", e);
                    continue;
                }
                data_banks(p, o, pl); /* 16-bit pointers: the string keeps its bank */
                pl->asize = (uint32_t)pl->slen;
            }
            if (pl->smode == 2)
                pl->alloc = 1;
            continue;
        }
        if (o->type == APEX_PATCH_BLOB) {
            char path[1024];
            apex_patch_path(patch, o->file, path, sizeof(path));
            FILE *fp = fopen(path, "rb");
            if (!fp) {
                op_fail(r, "cannot open %s", path);
                continue;
            }
            fclose(fp);
            Buffer fb = read_file(path);
            if (!fb.size) {
                free(fb.data);
                op_fail(r, "%s is empty", path);
                continue;
            }
            pl->sbytes = fb.data;
            pl->slen = fb.size;
            if (o->alloc) {
                if (alloc_banks(p, o, 0, 0, pl->abanks, &pl->nab, e, sizeof(e)) != 0) {
                    op_fail(r, "%s", e);
                    continue;
                }
                pl->asize = (uint32_t)pl->slen;
                pl->alloc = 1;
            } else {
                pl->len = pl->slen;
            }
            continue;
        }
        if (o->type == APEX_PATCH_DMD) {
            char path[1024];
            const uint8_t *data;
            size_t size;
            uint32_t org;
            bank_window(p, pl->bank, &data, &size, &org);
            const DataRange *dr = data_range_at(pl->bank, pl->addr, &p->data_ranges);
            unsigned planes =
                o->planes ? o->planes
                : (dr && dr->addr == pl->addr && dr->kind == DATA_DMD_FULLFRAME && dr->length)
                    ? (unsigned)dr->length
                    : 1u;
            pl->old_len = dmd_size(data + (pl->addr - org), size - (pl->addr - org), planes);
            if (!pl->old_len) {
                op_fail(r, "no %u-plane DMD fullframe at %s", planes, o->at);
                continue;
            }
            apex_patch_path(patch, o->file, path, sizeof(path));
            uint8_t *gray;
            unsigned w, h;
            if (!apex_image_read_gray(path, &gray, &w, &h, e, sizeof(e))) {
                op_fail(r, "%s", e);
                continue;
            }
            if (w != APEX_DMD_WIDTH || h != APEX_DMD_HEIGHT) {
                free(gray);
                op_fail(r, "%s is %ux%u; a DMD frame is %ux%u", path, w, h, APEX_DMD_WIDTH,
                        APEX_DMD_HEIGHT);
                continue;
            }
            uint8_t pl_bits[4 * APEX_DMD_PAGE_BYTES];
            apexdmd_planes_from_gray(gray, (int)planes, o->bright >= 0 ? o->bright : 1, pl_bits);
            free(gray);
            pl->sbytes = xmalloc((size_t)planes * APEX_DMD_ENCODE_MAX);
            pl->slen = 0;
            int raw = o->encoding && !strcmp(o->encoding, "raw");
            for (unsigned k = 0; k < planes; k++) {
                const uint8_t *src = pl_bits + (size_t)k * APEX_DMD_PAGE_BYTES;
                pl->slen += raw ? apexdmd_encode_plane(src, 0x00u, pl->sbytes + pl->slen)
                                : apexdmd_encode_best(src, pl->sbytes + pl->slen, NULL);
            }
            plan_replace(p, o, pl, r);
            continue;
        }
        if (o->type == APEX_PATCH_SPRITE) {
            plan_sprite(patch, p, o, pl, r);
            continue;
        }
        if (o->type == APEX_PATCH_MOVE) {
            pl->old_len = o->length ? o->length : block_length(p, pl->bank, pl->addr);
            if (!pl->old_len) {
                op_fail(r, "cannot tell the size of the data at %s; set length", o->at);
                continue;
            }
            if (alloc_banks(p, o, 1, pl->bank, pl->abanks, &pl->nab, e, sizeof(e)) != 0) {
                op_fail(r, "%s", e);
                continue;
            }
            data_banks(p, o, pl); /* 16-bit pointers: the data keeps its bank */
            pl->asize = pl->old_len;
            pl->alloc = 1;
            continue;
        }
        if (o->type == APEX_PATCH_PTR) {
            pl->len = o->width ? o->width : 3u;
        } else if (o->type == APEX_PATCH_HOOK) {
            if (hook_site(&c, o, r, pl->bank, pl->addr, &pl->displaced, &pl->jmp_back) != 0)
                continue;
            pl->len = pl->displaced;
        } else if (!(o->type == APEX_PATCH_ASM && o->alloc)) {
            ApexPatchOpResult tmp;
            memset(&tmp, 0, sizeof(tmp));
            uint8_t *b = op_bytes(&c, o, &tmp, pl->bank, pl->addr, 1, &pl->len);
            if (!b) {
                op_fail(r, "%s", tmp.msg);
                continue;
            }
            free(b);
        }
        if ((o->type == APEX_PATCH_ASM && o->alloc) || o->type == APEX_PATCH_HOOK) {
            if (alloc_banks(p, o, pl->has_at, pl->bank, pl->abanks, &pl->nab, e, sizeof(e)) != 0) {
                op_fail(r, "%s", e);
                continue;
            }
            uint32_t prov = pl->abanks[0] == 0xffu ? APEX_SYSTEM_ORG : APEX_PAGED_ORG;
            ApexAsmSnippet sn;
            if (assemble_op(&c, o, r, pl->abanks[0], prov, 1, &sn) != 0)
                continue;
            pl->asize = (uint32_t)sn.size;
            apex_asm_snippet_free(&sn);
            if (o->type == APEX_PATCH_HOOK)
                pl->asize += (uint32_t)pl->displaced + (pl->jmp_back ? 3u : 0u);
            if (pl->asize == 0) {
                op_fail(r, "no code to place");
                continue;
            }
            pl->alloc = 1;
        }
    }
    for (i = 0; i < patch->count; i++) {
        if (plan[i].active && patch->ops[i].type == APEX_PATCH_RESERVE &&
            out->ops[i].status != APEX_PATCH_ERROR)
            fs_sub(&ledger, plan[i].bank, plan[i].addr, plan[i].addr + patch->ops[i].length);
    }
    fs_copy(&out->free_total, &ledger);

    /* ---- 2. take the fixed writes out, then place the allocations ---- */
    for (i = 0; i < patch->count; i++) {
        if (plan[i].active && plan[i].has_at && plan[i].len &&
            out->ops[i].status != APEX_PATCH_ERROR && patch->ops[i].type != APEX_PATCH_FREE &&
            patch->ops[i].type != APEX_PATCH_RESERVE)
            fs_sub(&ledger, plan[i].bank, plan[i].addr, plan[i].addr + (uint32_t)plan[i].len);
    }
    /* string shifts: the strings behind move up, if the bytes after them are free */
    for (i = 0; i < patch->count; i++) {
        OpPlan *pl = &plan[i];
        if (!pl->active || patch->ops[i].type != APEX_PATCH_STRING || pl->smode != 1 ||
            out->ops[i].status == APEX_PATCH_ERROR)
            continue;
        uint32_t delta = (uint32_t)pl->slen - pl->old_len;
        /* The free-space scan keeps the first 0xFF after data as a possible
           terminator; behind a string pool (its last string ends with 0) that
           byte is free too. */
        const uint8_t *pe = base_bytes(p) + rom_offset(p, pl->bank, pl->pool_end);
        int room =
            fs_contains(&ledger, pl->bank, pl->pool_end, pl->pool_end + delta) ||
            (pe[0] == 0xffu && (delta == 1u || fs_contains(&ledger, pl->bank, pl->pool_end + 1u,
                                                           pl->pool_end + delta)));
        if (room) {
            fs_sub(&ledger, pl->bank, pl->pool_end, pl->pool_end + delta);
            pl->len = pl->pool_end + delta - pl->addr;
        } else if (pl->want == 1) {
            op_fail(&out->ops[i], "the %u byte(s) after the strings at B%02x_A%04x are not free",
                    (unsigned)delta, pl->bank, (unsigned)pl->pool_end);
        } else {
            pl->smode = 2; /* auto: relocate */
            pl->alloc = 1;
        }
    }
    for (int pass = 0; pass < 2; pass++) { /* pinned ones first, then the rest */
        for (i = 0; i < patch->count; i++) {
            const ApexPatchOp *o = &patch->ops[i];
            ApexPatchOpResult *r = &out->ops[i];
            OpPlan *pl = &plan[i];
            char e[256];
            if (!pl->active || !pl->alloc || r->status == APEX_PATCH_ERROR ||
                (pass == 0) != (o->placed != NULL))
                continue;
            if (o->placed) {
                int ok = 0;
                if (!resolve_addr_ix(p, &c.ix, o->placed, &pl->abank, &pl->aaddr, e, sizeof(e))) {
                    op_fail(r, "placed: %s", e);
                    continue;
                }
                for (int k = 0; k < pl->nab; k++)
                    ok = ok || pl->abanks[k] == pl->abank;
                if (!ok) {
                    op_fail(r, "placed %s is not in a bank alloc allows", o->placed);
                    continue;
                }
                if (!fs_contains(&ledger, pl->abank, pl->aaddr, pl->aaddr + pl->asize)) {
                    op_fail(r, "placed %s: %u byte(s) there are no longer free", o->placed,
                            (unsigned)pl->asize);
                    continue;
                }
            } else if (!fs_alloc(&ledger, pl->abanks, pl->nab, pl->asize, &pl->abank, &pl->aaddr)) {
                op_fail(r, "no free block of %u byte(s) in the allowed bank(s)",
                        (unsigned)pl->asize);
                continue;
            }
            fs_sub(&ledger, pl->abank, pl->aaddr, pl->aaddr + pl->asize);
        }
    }
    /* the old places of moved data are free now */
    for (i = 0; i < patch->count; i++) {
        if (plan[i].active &&
            (patch->ops[i].type == APEX_PATCH_MOVE ||
             (IMAGE_OP(patch->ops[i].type) && plan[i].smode == 2)) &&
            out->ops[i].status != APEX_PATCH_ERROR)
            fs_add(&ledger, plan[i].bank, plan[i].addr, plan[i].old_len, APEX_FREE_DECLARED);
    }
    fs_copy(&out->free_left, &ledger);
    apex_patch_free_space_free(&ledger);

    /* ---- 3. labels of the patch code, at their final addresses ---- */
    for (i = 0; i < patch->count; i++) {
        const ApexPatchOp *o = &patch->ops[i];
        ApexPatchOpResult *r = &out->ops[i];
        OpPlan *pl = &plan[i];
        if (pl->active && r->status != APEX_PATCH_ERROR && o->type == APEX_PATCH_BLOB && o->label) {
            /* a named blob: patch code and ptr ops can use its label */
            uint8_t lb = pl->alloc ? pl->abank : pl->bank;
            uint32_t la = pl->alloc ? pl->aaddr : pl->addr;
            if (sym_find(&c.ix, o->label)) {
                op_fail(r, "label '%s' already exists in the analysis", o->label);
                continue;
            }
            PatchLabel *nl = realloc(c.rc.labels, (c.rc.label_count + 1u) * sizeof(*nl));
            if (nl) {
                c.rc.labels = nl;
                nl[c.rc.label_count].name = pstrdup(o->label);
                nl[c.rc.label_count].bank = lb;
                nl[c.rc.label_count].addr = la;
                c.rc.label_count++;
                sym_push(out, &sym_cap, o->label, lb, la, 0, 0, 0);
            }
            continue;
        }
        if (!pl->active || r->status == APEX_PATCH_ERROR ||
            (o->type != APEX_PATCH_ASM && o->type != APEX_PATCH_HOOK))
            continue;
        uint8_t cb = pl->alloc ? pl->abank : pl->bank;
        uint32_t ca = pl->alloc ? pl->aaddr : pl->addr;
        ApexAsmSnippet sn;
        if (assemble_op(&c, o, r, cb, ca, 1, &sn) != 0)
            continue;
        for (j = 0; j < sn.label_count; j++) {
            if (sym_find(&c.ix, sn.labels[j].name)) {
                op_fail(r, "label '%s' already exists in the analysis", sn.labels[j].name);
                continue;
            }
            PatchLabel *nl = realloc(c.rc.labels, (c.rc.label_count + 1u) * sizeof(*nl));
            if (!nl)
                break;
            c.rc.labels = nl;
            nl[c.rc.label_count].name = pstrdup(sn.labels[j].name);
            nl[c.rc.label_count].bank = cb;
            nl[c.rc.label_count].addr = sn.labels[j].addr;
            c.rc.label_count++;
            sym_push(out, &sym_cap, sn.labels[j].name, cb, sn.labels[j].addr, 1, 0,
                     pl->alloc && sn.labels[j].addr == ca);
        }
        /* a name for allocated code that does not start with a label */
        if (pl->alloc) {
            int has = 0;
            for (j = 0; j < sn.label_count; j++)
                has = has || sn.labels[j].addr == ca;
            if (!has) {
                char nm[128];
                size_t k;
                snprintf(nm, sizeof(nm), "patch_%s", o->name);
                for (k = 0; nm[k]; k++) {
                    if (!(isalnum((unsigned char)nm[k]) || nm[k] == '_'))
                        nm[k] = '_';
                }
                sym_push(out, &sym_cap, nm, cb, ca, 1, 0, 1);
            }
        }
        apex_asm_snippet_free(&sn);
    }
    for (i = 0; i < c.rc.ram_count; i++)
        sym_push(out, &sym_cap, c.rc.ram[i].name, 0, c.rc.ram[i].addr, 0, 1, 0);

    /* ---- 4. emit ---- */
    for (i = 0; i < patch->count; i++) {
        const ApexPatchOp *o = &patch->ops[i];
        ApexPatchOpResult *r = &out->ops[i];
        OpPlan *pl = &plan[i];
        if (!pl->active)
            continue;
        if (r->status == APEX_PATCH_ERROR) {
            out->errors++;
            continue;
        }
        if (o->type == APEX_PATCH_RAM || o->type == APEX_PATCH_FREE ||
            o->type == APEX_PATCH_RESERVE) {
            r->bank = pl->bank;
            r->addr = pl->addr;
            if (r->status == APEX_PATCH_WARN)
                out->warnings++;
            continue;
        }
        uint8_t *site = NULL, *code = NULL;
        size_t site_len = 0, code_len = 0;
        if (o->type == APEX_PATCH_ASM && pl->alloc) {
            ApexAsmSnippet sn;
            if (assemble_op(&c, o, r, pl->abank, pl->aaddr, 0, &sn) == 0) {
                code = sn.bytes;
                code_len = sn.size;
                sn.bytes = NULL;
                apex_asm_snippet_free(&sn);
            }
        } else if (o->type == APEX_PATCH_HOOK) {
            ApexAsmSnippet sn;
            if (assemble_op(&c, o, r, pl->abank, pl->aaddr, 0, &sn) == 0) {
                const uint8_t *orig = base_bytes(p) + rom_offset(p, pl->bank, pl->addr);
                uint32_t back = pl->addr + (uint32_t)pl->displaced;
                code_len = sn.size + pl->displaced + (pl->jmp_back ? 3u : 0u);
                code = xmalloc(code_len);
                memcpy(code, sn.bytes, sn.size);
                memcpy(code + sn.size, orig, pl->displaced);
                if (pl->jmp_back) {
                    code[sn.size + pl->displaced] = 0x7eu; /* JMP back */
                    code[sn.size + pl->displaced + 1u] = (uint8_t)(back >> 8);
                    code[sn.size + pl->displaced + 2u] = (uint8_t)back;
                }
                apex_asm_snippet_free(&sn);
                site_len = pl->displaced;
                site = xmalloc(site_len);
                memset(site, PATCH_NOP, site_len);
                site[0] = 0x7eu; /* JMP detour */
                site[1] = (uint8_t)(pl->aaddr >> 8);
                site[2] = (uint8_t)pl->aaddr;
            }
        } else if (o->type == APEX_PATCH_VALUE) {
            uint32_t v;
            uint8_t vb = 0;
            int hb;
            char e[256];
            if (!value_eval(&c, o, &v, &vb, &hb, e, sizeof(e))) {
                op_fail(r, "%s", e);
            } else if ((pl->len == 1u && v > 0xffu) || (pl->len == 2u && v > 0xffffu)) {
                op_fail(r, "0x%x does not fit in %lu byte(s)", (unsigned)v, (unsigned long)pl->len);
            } else if (pl->len == 3u && !hb) {
                op_fail(r, "a far (3-byte) value needs a label");
            } else {
                site_len = pl->len;
                site = xmalloc(3);
                if (pl->len == 1u) {
                    site[0] = (uint8_t)v;
                } else {
                    site[0] = (uint8_t)(v >> 8);
                    site[1] = (uint8_t)v;
                    site[2] = vb;
                }
            }
        } else if (o->type == APEX_PATCH_BLOB) {
            uint8_t *b = xmalloc(pl->slen);
            memcpy(b, pl->sbytes, pl->slen);
            if (pl->alloc) {
                code = b;
                code_len = pl->slen;
            } else {
                site = b;
                site_len = pl->slen;
            }
        } else if (o->type == APEX_PATCH_STRING || IMAGE_OP(o->type)) {
            static const char *const modes[] = {"inplace", "shift", "relocate"};
            snprintf(r->mode_used, sizeof(r->mode_used), "%s", modes[pl->smode]);
            if (pl->smode == 2) {
                code_len = pl->slen;
                code = xmalloc(code_len);
                memcpy(code, pl->sbytes, code_len);
                if (IMAGE_OP(o->type)) { /* like move: the old image is cleared */
                    site_len = pl->old_len;
                    site = xmalloc(site_len);
                    memset(site, 0xffu, site_len);
                }
            } else {
                site_len = pl->smode == 0 ? pl->slen : pl->len;
                site = xmalloc(site_len);
                memcpy(site, pl->sbytes, pl->slen);
                if (pl->smode == 1)
                    memcpy(site + pl->slen,
                           base_bytes(p) + rom_offset(p, pl->bank, pl->addr + pl->old_len),
                           pl->pool_end - (pl->addr + pl->old_len));
            }
        } else if (o->type == APEX_PATCH_MOVE) {
            code_len = pl->old_len;
            code = xmalloc(code_len);
            memcpy(code, base_bytes(p) + rom_offset(p, pl->bank, pl->addr), code_len);
            site_len = pl->old_len; /* the old place is cleared */
            site = xmalloc(site_len);
            memset(site, 0xffu, site_len);
        } else if (o->type == APEX_PATCH_PTR) {
            char src[200];
            snprintf(src, sizeof(src), pl->len == 2u ? ".DW %s" : "FAR_PTR %s", o->target);
            char e[320];
            ApexAsmSnippet sn;
            c.rc.code_bank = pl->bank;
            if (apex_asm_snippet(src, pl->bank, pl->addr, p->rom.size, patch_resolve, &c.rc, 0, &sn,
                                 e, sizeof(e)) != 0) {
                op_fail(r, "target: %s", e);
            } else {
                site = sn.bytes;
                site_len = sn.size;
                sn.bytes = NULL;
                apex_asm_snippet_free(&sn);
            }
        } else {
            site = op_bytes(&c, o, r, pl->bank, pl->addr, 0, &site_len);
        }
        if (r->status == APEX_PATCH_ERROR) {
            free(site);
            free(code);
            out->errors++;
            continue;
        }
        if (code && code_len != pl->asize) {
            op_fail(r, "code size changed from %u to %lu byte(s) between passes",
                    (unsigned)pl->asize, (unsigned long)code_len);
            free(site);
            free(code);
            out->errors++;
            continue;
        }
        /* string / move: expect guards the old data, whatever is written */
        int data_op =
            o->type == APEX_PATCH_STRING || o->type == APEX_PATCH_MOVE || IMAGE_OP(o->type);
        if (data_op) {
            uint32_t cap = IMAGE_OP(o->type) ? 16u : 8u;
            r->guard_len = o->type == APEX_PATCH_STRING ? pl->old_len
                                                        : (pl->old_len < cap ? pl->old_len : cap);
            r->guard = xmalloc(r->guard_len);
            r->old_length = pl->old_len;
            memcpy(r->guard, base_bytes(p) + rom_offset(p, pl->bank, pl->addr), r->guard_len);
            if (!o->expect) {
                op_warn(r, "no expect guard (apexpatch pin adds it)");
            } else {
                uint8_t *ex;
                long n = parse_hex_bytes(o->expect, &ex);
                if (n < 0 || (size_t)n != r->guard_len || memcmp(ex, r->guard, r->guard_len)) {
                    char *have = hex_text(r->guard, r->guard_len);
                    op_fail(r, "the original data is %s, expect says %s", have, o->expect);
                    free(have);
                }
                if (n >= 0)
                    free(ex);
            }
        }
        /* the region at `at` (absent for allocated asm) */
        if (site && r->status != APEX_PATCH_ERROR) {
            r->bank = pl->bank;
            r->addr = pl->addr;
            r->rom_off = rom_offset(p, pl->bank, pl->addr);
            if (r->rom_off + site_len > p->rom.size) {
                op_fail(r, "runs past the end of the ROM");
            } else if (data_op || check_expect(p, o, r, site_len) == 0) {
                claim_bytes(patch, owner, r, i, r->rom_off, site_len);
            }
        }
        if (pl->xlen && r->status != APEX_PATCH_ERROR)
            claim_bytes(patch, owner, r, i, pl->xoff, pl->xlen);
        if (code && r->status != APEX_PATCH_ERROR) {
            r->has_alloc = 1;
            r->alloc_bank = pl->abank;
            r->alloc_addr = pl->aaddr;
            r->alloc_rom_off = rom_offset(p, pl->abank, pl->aaddr);
            r->alloc_length = code_len;
            claim_bytes(patch, owner, r, i, r->alloc_rom_off, code_len);
        }
        /* moved strings / data: rewrite the references to them */
        if (r->status != APEX_PATCH_ERROR &&
            (((o->type == APEX_PATCH_STRING || IMAGE_OP(o->type)) && pl->smode) ||
             o->type == APEX_PATCH_MOVE)) {
            int manual;
            r->fixup_first = out->fixup_count;
            if (o->type == APEX_PATCH_MOVE)
                manual =
                    rewrite_refs(&c, out, &fixup_cap, owner, i, pl->bank, pl->addr, pl->old_len,
                                 pl->abank, pl->aaddr, pl->addr, pl->addr + pl->old_len, code);
            else if (pl->smode == 1)
                manual =
                    rewrite_refs(&c, out, &fixup_cap, owner, i, pl->bank, pl->addr + pl->old_len,
                                 pl->pool_end - (pl->addr + pl->old_len), pl->bank,
                                 pl->addr + (uint32_t)pl->slen, 0, 0, NULL);
            else
                manual = rewrite_refs(&c, out, &fixup_cap, owner, i, pl->bank, pl->addr, 1u,
                                      pl->abank, pl->aaddr, 0, 0, NULL);
            r->fixup_count = out->fixup_count - r->fixup_first;
            if (manual && !o->accept)
                op_fail(r,
                        "%d reference(s) cannot be rewritten automatically (see the review); "
                        "accept = yes builds anyway",
                        manual);
            else if (manual)
                op_warn(r, "%d reference(s) need a manual fix", manual);
            if (r->status != APEX_PATCH_ERROR && r->fixup_count == 0 &&
                !(o->type == APEX_PATCH_STRING && pl->smode == 1))
                op_warn(r, "no known reference to it (computed addresses are not found)");
        }
        if (r->status == APEX_PATCH_ERROR) {
            free(site);
            free(code);
            out->errors++;
            continue;
        }
        if (site) {
            r->length = site_len;
            r->orig = xmalloc(site_len ? site_len : 1u);
            memcpy(r->orig, base_bytes(p) + r->rom_off, site_len);
            r->patched = site;
            memcpy(out->rom + r->rom_off, site, site_len);
            if (pl->xlen) /* a 0xFE sprite's second plane, behind its pointer */
                memcpy(out->rom + pl->xoff, pl->xbytes, pl->xlen);
        } else {
            /* allocated asm: its code is the op's region */
            r->bank = pl->abank;
            r->addr = pl->aaddr;
            r->rom_off = r->alloc_rom_off;
            r->length = code_len;
            r->orig = xmalloc(code_len);
            memcpy(r->orig, base_bytes(p) + r->rom_off, code_len);
            r->patched = xmalloc(code_len);
            memcpy(r->patched, code, code_len);
        }
        if (code) {
            memcpy(out->rom + r->alloc_rom_off, code, code_len);
            r->alloc_bytes = code;
        }
        if (r->status == APEX_PATCH_WARN)
            out->warnings++;
    }
    free(owner);
    for (i = 0; i < patch->count; i++) {
        free(plan[i].sbytes);
        free(plan[i].xbytes);
    }
    free(plan);
    for (i = 0; i < c.rc.label_count; i++)
        free(c.rc.labels[i].name);
    free(c.rc.labels);
    for (i = 0; i < c.rc.ram_count; i++)
        free(c.rc.ram[i].name);
    free(c.rc.ram);
    free(c.ix.items);

    if (patch->checksum == APEX_PATCH_CHECKSUM_FIX) {
        if (!apex_rominfo_fix_checksum(out->rom, out->rom_size)) {
            snprintf(out->msg, sizeof(out->msg), "no valid checksum solution found");
            out->errors++;
        }
    } else if (patch->checksum == APEX_PATCH_CHECKSUM_DISABLE) {
        ari_w16(out->rom, out->rom_size - APEX_ROMINFO_DELTA_END, APEX_ROMINFO_DISABLE_DELTA);
    }
    return out->errors ? 1 : 0;
}

void apex_patch_build_free(ApexPatchBuild *b) {
    size_t i;
    if (!b)
        return;
    for (i = 0; i < b->count; i++) {
        free(b->ops[i].orig);
        free(b->ops[i].patched);
        free(b->ops[i].alloc_bytes);
        free(b->ops[i].guard);
    }
    free(b->fixups);
    for (i = 0; i < b->sym_count; i++)
        free(b->syms[i].name);
    free(b->syms);
    free(b->ops);
    free(b->rom);
    apex_patch_free_space_free(&b->free_total);
    apex_patch_free_space_free(&b->free_left);
    memset(b, 0, sizeof(*b));
}

int apex_patch_pin(ApexPatch *patch, const ApexPatchBuild *build) {
    size_t i;
    int n = 0;
    for (i = 0; i < patch->count && i < build->count; i++) {
        ApexPatchOp *o = &patch->ops[i];
        const ApexPatchOpResult *r = &build->ops[i];
        if (r->status == APEX_PATCH_ERROR || r->status == APEX_PATCH_DISABLED)
            continue;
        if (!o->expect && r->guard && r->guard_len) {
            o->expect = hex_text(r->guard, r->guard_len);
            n++;
        } else if (!o->expect && r->orig && r->length && !r->guard &&
                   !((o->type == APEX_PATCH_ASM || o->type == APEX_PATCH_BLOB) && o->alloc)) {
            o->expect = hex_text(r->orig, r->length);
            n++;
        }
        if (!o->placed && r->has_alloc) {
            char pl[16];
            snprintf(pl, sizeof(pl), "B%02x_A%04x", r->alloc_bank, (unsigned)r->alloc_addr);
            o->placed = pstrdup(pl);
            n++;
        }
    }
    return n;
}

/* ── editing helpers ───────────────────────────────────────────────────── */

void apex_patch_set_field(char **field, const char *value) {
    set_str(field, value && *value ? value : NULL);
}

long apex_patch_add_op(ApexPatch *patch, int type, const char *base_name, const char *at) {
    char name[96];
    size_t i;
    int n = 1;
    for (;;) {
        int clash = 0;
        if (n == 1)
            snprintf(name, sizeof(name), "%s", base_name);
        else
            snprintf(name, sizeof(name), "%s_%d", base_name, n);
        for (i = 0; i < patch->count; i++)
            clash = clash || !strcmp(patch->ops[i].name, name);
        if (!clash)
            break;
        n++;
    }
    ApexPatchOp *op = add_op(patch, name);
    if (!op)
        return -1;
    op->type = type;
    set_str(&op->at, at);
    return (long)(patch->count - 1u);
}

static void free_op(ApexPatchOp *o) {
    free(o->name);
    free(o->at);
    free(o->expect);
    free(o->bytes);
    free(o->code);
    free(o->file);
    free(o->note);
    free(o->comment);
    free(o->alloc);
    free(o->placed);
    free(o->target);
    free(o->label);
    free(o->value);
    free(o->vtype);
    free(o->text);
    free(o->mode);
    free(o->encoding);
}

void apex_patch_remove_op(ApexPatch *patch, size_t index) {
    if (index >= patch->count)
        return;
    free_op(&patch->ops[index]);
    memmove(&patch->ops[index], &patch->ops[index + 1u],
            (patch->count - index - 1u) * sizeof(patch->ops[0]));
    patch->count--;
}

int apex_patch_move_op(ApexPatch *patch, size_t index, int dir) {
    size_t other = index + (size_t)(long)dir;
    if (index >= patch->count || other >= patch->count)
        return 0;
    ApexPatchOp t = patch->ops[index];
    patch->ops[index] = patch->ops[other];
    patch->ops[other] = t;
    return 1;
}

void apex_patch_format_at(const ApexProject *p, uint8_t bank, uint32_t addr, char *out,
                          size_t outsz) {
    uint32_t start;
    const char *nm = apex_wpc_routine_at(p, bank, addr, &start);
    if (nm && !generated_any_label_name(nm) && addr - start < 0x400u) {
        if (addr == start)
            snprintf(out, outsz, "%s", nm);
        else
            snprintf(out, outsz, "%s+0x%x", nm, (unsigned)(addr - start));
        return;
    }
    snprintf(out, outsz, "B%02x_A%04x", bank, (unsigned)addr);
}

/* ── romset lookup ─────────────────────────────────────────────────────── */

static uint32_t rd_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t rd_le16(const uint8_t *b) {
    return (uint16_t)(b[0] | (b[1] << 8));
}

int apex_patch_romset_member(const char *path, uint32_t crc, char *name, size_t namesz) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    Buffer b = read_file(path);
    int found = 0;
    if (b.size < 22u) {
        free(b.data);
        return 0;
    }
    /* End of central directory record (PK 5 6) within the last 64 KB. */
    size_t lim = b.size > 65557u ? b.size - 65557u : 0;
    size_t i = b.size - 22u;
    while (rd_le32(b.data + i) != 0x06054b50u) {
        if (i == lim) {
            free(b.data);
            return 0;
        }
        i--;
    }
    uint16_t entries = rd_le16(b.data + i + 10u);
    size_t cd = rd_le32(b.data + i + 16u);
    for (uint16_t e = 0; e < entries && cd + 46u <= b.size; e++) {
        const uint8_t *h = b.data + cd;
        if (rd_le32(h) != 0x02014b50u)
            break;
        uint32_t ecrc = rd_le32(h + 16u);
        uint16_t nl = rd_le16(h + 28u), xl = rd_le16(h + 30u), cl = rd_le16(h + 32u);
        if (cd + 46u + nl > b.size)
            break;
        if (ecrc == crc && nl < namesz) {
            memcpy(name, h + 46u, nl);
            name[nl] = '\0';
            found = 1;
            break;
        }
        cd += 46u + nl + xl + cl;
    }
    free(b.data);
    return found;
}
