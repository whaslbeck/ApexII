#include "apex_nvram.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Minimal JSON value tree + recursive-descent parser.
 * ------------------------------------------------------------------------- */

static char *nv_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal {
    JType type;
    int    b;      /* J_BOOL */
    double n;      /* J_NUM  */
    char  *num_raw;/* J_NUM: exact source text, re-emitted verbatim (owned; may be NULL) */
    char  *s;      /* J_STR (owned) */
    struct JVal **arr;              /* J_ARR (owned) */
    size_t arrn;
    char **keys;                    /* J_OBJ (owned) */
    struct JVal **vals;
    size_t objn;
} JVal;

/* Cap nesting so a pathological/deep map cannot overflow the C stack. */
#define JSON_MAX_DEPTH 200

typedef struct {
    const char *p;
    const char *end;
    int   depth;
    int   error;
    char  errbuf[128];
} JParser;

static void jval_free(JVal *v)
{
    size_t i;
    if (!v) return;
    if (v->type == J_NUM) {
        free(v->num_raw);
    } else if (v->type == J_STR) {
        free(v->s);
    } else if (v->type == J_ARR) {
        for (i = 0; i < v->arrn; i++) jval_free(v->arr[i]);
        free(v->arr);
    } else if (v->type == J_OBJ) {
        for (i = 0; i < v->objn; i++) {
            free(v->keys[i]);
            jval_free(v->vals[i]);
        }
        free(v->keys);
        free(v->vals);
    }
    free(v);
}

static void jerr(JParser *jp, const char *msg)
{
    if (!jp->error) {
        jp->error = 1;
        snprintf(jp->errbuf, sizeof(jp->errbuf), "%s", msg);
    }
}

static void jskip_ws(JParser *jp)
{
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            jp->p++;
        } else if (c == '/' && jp->p + 1 < jp->end && jp->p[1] == '/') {
            /* tolerate // line comments (some maps carry them) */
            while (jp->p < jp->end && *jp->p != '\n') jp->p++;
        } else {
            break;
        }
    }
}

static JVal *jparse_value(JParser *jp);

/* Parse a JSON string literal (jp->p points at the opening quote). */
static char *jparse_string_raw(JParser *jp)
{
    char  *buf;
    size_t cap = 16, len = 0;
    if (jp->p >= jp->end || *jp->p != '"') {
        jerr(jp, "expected string");
        return NULL;
    }
    jp->p++;
    buf = malloc(cap);
    if (!buf) { jerr(jp, "out of memory"); return NULL; }
    while (jp->p < jp->end && *jp->p != '"') {
        char c = *jp->p++;
        char out = c;
        if (c == '\\') {
            if (jp->p >= jp->end) break;
            char e = *jp->p++;
            switch (e) {
            case '"':  out = '"';  break;
            case '\\': out = '\\'; break;
            case '/':  out = '/';  break;
            case 'n':  out = '\n'; break;
            case 't':  out = '\t'; break;
            case 'r':  out = '\r'; break;
            case 'b':  out = '\b'; break;
            case 'f':  out = '\f'; break;
            case 'u': {
                /* \uXXXX: keep ASCII, otherwise emit '?' (docs are display text) */
                unsigned v = 0; int k;
                for (k = 0; k < 4 && jp->p < jp->end; k++) {
                    char h = *jp->p++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                }
                out = (v < 0x80u) ? (char)v : '?';
                break;
            }
            default: out = e; break;
            }
        }
        if (len + 1 >= cap) {
            char *nb = realloc(buf, cap *= 2);
            if (!nb) { free(buf); jerr(jp, "out of memory"); return NULL; }
            buf = nb;
        }
        buf[len++] = out;
    }
    if (jp->p >= jp->end || *jp->p != '"') {
        free(buf);
        jerr(jp, "unterminated string");
        return NULL;
    }
    jp->p++;
    buf[len] = '\0';
    return buf;
}

static JVal *jparse_object(JParser *jp)
{
    JVal *v = calloc(1, sizeof(*v));
    if (!v) { jerr(jp, "out of memory"); return NULL; }
    v->type = J_OBJ;
    jp->p++; /* '{' */
    jskip_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') { jp->p++; return v; }
    for (;;) {
        char *key;
        JVal *val;
        jskip_ws(jp);
        key = jparse_string_raw(jp);
        if (jp->error) { jval_free(v); return NULL; }
        jskip_ws(jp);
        if (jp->p >= jp->end || *jp->p != ':') { free(key); jerr(jp, "expected ':'"); jval_free(v); return NULL; }
        jp->p++;
        val = jparse_value(jp);
        if (jp->error) { free(key); jval_free(v); return NULL; }
        {
            char **nk = realloc(v->keys, (v->objn + 1) * sizeof(*nk));
            JVal **nv = realloc(v->vals, (v->objn + 1) * sizeof(*nv));
            if (!nk || !nv) { free(nk ? nk : v->keys); free(key); jval_free(val); jerr(jp, "out of memory"); jval_free(v); return NULL; }
            v->keys = nk; v->vals = nv;
            v->keys[v->objn] = key;
            v->vals[v->objn] = val;
            v->objn++;
        }
        jskip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
        if (jp->p < jp->end && *jp->p == '}') { jp->p++; break; }
        jerr(jp, "expected ',' or '}'");
        jval_free(v);
        return NULL;
    }
    return v;
}

static JVal *jparse_array(JParser *jp)
{
    JVal *v = calloc(1, sizeof(*v));
    if (!v) { jerr(jp, "out of memory"); return NULL; }
    v->type = J_ARR;
    jp->p++; /* '[' */
    jskip_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') { jp->p++; return v; }
    for (;;) {
        JVal *el = jparse_value(jp);
        if (jp->error) { jval_free(v); return NULL; }
        {
            JVal **na = realloc(v->arr, (v->arrn + 1) * sizeof(*na));
            if (!na) { jval_free(el); jerr(jp, "out of memory"); jval_free(v); return NULL; }
            v->arr = na;
            v->arr[v->arrn++] = el;
        }
        jskip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
        if (jp->p < jp->end && *jp->p == ']') { jp->p++; break; }
        jerr(jp, "expected ',' or ']'");
        jval_free(v);
        return NULL;
    }
    return v;
}

/* Length-bounded literal match: the fixed-size keywords `true`/`false`/`null`
   are compared only within [jp->p, jp->end), never past the buffer. */
static int jmatch_kw(JParser *jp, const char *kw)
{
    size_t n = strlen(kw);
    if ((size_t)(jp->end - jp->p) < n) return 0;
    return memcmp(jp->p, kw, n) == 0;
}

static JVal *jmk_bool_null(JParser *jp, JType t, int b)
{
    JVal *v = calloc(1, sizeof(*v));
    if (!v) { jerr(jp, "out of memory"); return NULL; }
    v->type = t; v->b = b;
    return v;
}

static JVal *jparse_value(JParser *jp)
{
    jskip_ws(jp);
    if (jp->p >= jp->end) { jerr(jp, "unexpected end of input"); return NULL; }
    char c = *jp->p;
    if (c == '{' || c == '[') {
        if (++jp->depth > JSON_MAX_DEPTH) {
            jerr(jp, "nesting too deep");
            jp->depth--;
            return NULL;
        }
        JVal *v = (c == '{') ? jparse_object(jp) : jparse_array(jp);
        jp->depth--;
        return v;
    }
    if (c == '"') {
        JVal *v = calloc(1, sizeof(*v));
        if (!v) { jerr(jp, "out of memory"); return NULL; }
        v->type = J_STR;
        v->s = jparse_string_raw(jp);
        if (jp->error) { jval_free(v); return NULL; }
        return v;
    }
    if (jmatch_kw(jp, "true"))  { jp->p += 4; return jmk_bool_null(jp, J_BOOL, 1); }
    if (jmatch_kw(jp, "false")) { jp->p += 5; return jmk_bool_null(jp, J_BOOL, 0); }
    if (jmatch_kw(jp, "null"))  { jp->p += 4; return jmk_bool_null(jp, J_NULL, 0); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        /* Scan the numeric run bounded by jp->end (so strtod never reads past
           the buffer) and keep the exact source text for verbatim re-emit. */
        const char *start = jp->p;
        const char *q = jp->p;
        while (q < jp->end && (*q == '-' || *q == '+' || *q == '.' ||
               *q == 'e' || *q == 'E' || (*q >= '0' && *q <= '9'))) {
            q++;
        }
        size_t nlen = (size_t)(q - start);
        if (nlen == 0) { jerr(jp, "invalid number"); return NULL; }
        char numbuf[64];
        char *tmp = (nlen < sizeof(numbuf)) ? numbuf : malloc(nlen + 1);
        if (!tmp) { jerr(jp, "out of memory"); return NULL; }
        memcpy(tmp, start, nlen); tmp[nlen] = '\0';
        char *end2 = NULL;
        double d = strtod(tmp, &end2);
        int ok = (end2 != tmp);
        JVal *v = ok ? calloc(1, sizeof(*v)) : NULL;
        if (ok && v) {
            v->type = J_NUM; v->n = d;
            v->num_raw = nv_strdup(tmp);
        }
        if (tmp != numbuf) free(tmp);
        if (!ok)  { jerr(jp, "invalid number"); return NULL; }
        if (!v)   { jerr(jp, "out of memory"); return NULL; }
        jp->p = q;
        return v;
    }
    jerr(jp, "unexpected token");
    return NULL;
}

static const JVal *jobj_get(const JVal *o, const char *key)
{
    size_t i;
    if (!o || o->type != J_OBJ) return NULL;
    for (i = 0; i < o->objn; i++) {
        if (strcmp(o->keys[i], key) == 0) return o->vals[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Descriptor walk + mapping to ApexII RAM locations.
 * ------------------------------------------------------------------------- */

static int parse_u32_str(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    v = strtoul(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 0);
    if (end == s) return 0;
    *out = (uint32_t)v;
    return 1;
}

/* Produce a valid symbol name from a label: [A-Za-z_][A-Za-z0-9_]* */
static char *sanitize_name(const char *label)
{
    size_t cap = 64, len = 0;
    char  *buf = malloc(cap);
    const char *p = label ? label : "";
    int last_us = 0;
    if (!buf) return NULL;
    for (; *p && len + 2 < cap; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            buf[len++] = c;
            last_us = 0;
        } else if (!last_us && len > 0) {
            buf[len++] = '_';
            last_us = 1;
        }
    }
    while (len > 0 && buf[len - 1] == '_') len--;   /* trim trailing _ */
    buf[len] = '\0';
    if (len == 0 || (buf[0] >= '0' && buf[0] <= '9')) {
        /* empty or leading digit → prefix underscore */
        memmove(buf + 1, buf, len + 1);
        buf[0] = '_';
    }
    return buf;
}

static int name_in_use(const ApexNvramLocs *locs, const char *name)
{
    size_t i;
    for (i = 0; i < locs->count; i++) {
        if (locs->items[i].name && strcmp(locs->items[i].name, name) == 0) return 1;
    }
    return 0;
}

static void add_loc(ApexNvramLocs *locs, char *name, uint32_t addr, char *doc)
{
    if (locs->count == locs->cap) {
        size_t nc = locs->cap ? locs->cap * 2 : 32;
        ApexNvramLoc *ni = realloc(locs->items, nc * sizeof(*ni));
        if (!ni) { free(name); free(doc); return; }
        locs->items = ni;
        locs->cap = nc;
    }
    locs->items[locs->count].name = name;
    locs->items[locs->count].addr = addr;
    locs->items[locs->count].doc = doc;
    locs->count++;
}

/* An object with a "start" key is a leaf descriptor → one RAM location. */
static void add_from_descriptor(const JVal *d, uint32_t base, ApexNvramLocs *locs)
{
    const JVal *start = jobj_get(d, "start");
    const JVal *label = jobj_get(d, "label");
    const JVal *slabel = jobj_get(d, "short_label");
    uint32_t addr = 0;
    const char *name_src, *doc_src;
    char *name, *doc;

    if (start->type == J_STR) {
        if (!parse_u32_str(start->s, &addr)) return;
    } else if (start->type == J_NUM) {
        addr = (uint32_t)start->n;
    } else {
        return;
    }
    addr += base;

    /* name: short_label preferred (concise), else label */
    name_src = (slabel && slabel->type == J_STR && slabel->s[0]) ? slabel->s
             : (label && label->type == J_STR && label->s[0]) ? label->s : NULL;
    name = sanitize_name(name_src);
    if (!name) return;
    if (name[0] == '\0') {
        free(name);
        name = malloc(16);
        if (!name) return;
        snprintf(name, 16, "RAM_%04x", addr & 0xffffu);
    }
    /* ensure uniqueness */
    if (name_in_use(locs, name)) {
        char *u = malloc(strlen(name) + 12);
        int k = 2;
        if (u) {
            do { sprintf(u, "%s_%d", name, k++); } while (name_in_use(locs, u) && k < 10000);
            free(name);
            name = u;
        }
    }

    /* doc: the human label (fall back to short_label) */
    doc_src = (label && label->type == J_STR && label->s[0]) ? label->s
            : (slabel && slabel->type == J_STR) ? slabel->s : "";
    doc = nv_strdup(doc_src ? doc_src : "");

    add_loc(locs, name, addr, doc);
}

static void walk(const JVal *v, uint32_t base, ApexNvramLocs *locs)
{
    size_t i;
    if (!v) return;
    if (v->type == J_OBJ) {
        if (jobj_get(v, "start")) {
            add_from_descriptor(v, base, locs); /* leaf */
            return;
        }
        for (i = 0; i < v->objn; i++) {
            if (v->keys[i][0] == '_') continue; /* _metadata / _fileformat */
            walk(v->vals[i], base, locs);
        }
    } else if (v->type == J_ARR) {
        for (i = 0; i < v->arrn; i++) walk(v->arr[i], base, locs);
    }
}

int apex_nvram_parse_json(const char *text, size_t len, uint32_t base,
                          ApexNvramLocs *out, char *err, size_t errsz)
{
    JParser jp;
    JVal   *root;

    memset(out, 0, sizeof(*out));
    memset(&jp, 0, sizeof(jp));
    jp.p = text;
    jp.end = text + len;
    root = jparse_value(&jp);
    if (jp.error || !root) {
        if (err) snprintf(err, errsz, "JSON parse error: %s", jp.errbuf);
        jval_free(root);
        return 1;
    }
    if (root->type != J_OBJ) {
        if (err) snprintf(err, errsz, "top-level JSON is not an object");
        jval_free(root);
        return 1;
    }
    walk(root, base, out);
    jval_free(root);
    if (out->count == 0) {
        if (err) snprintf(err, errsz, "no memory-location descriptors found");
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Export.
 * ------------------------------------------------------------------------- */

static void json_write_escaped(FILE *out, const char *s)
{
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out);  break;
        case '\t': fputs("\\t", out);  break;
        case '\r': fputs("\\r", out);  break;
        default:
            if (c < 0x20) fprintf(out, "\\u%04x", c);
            else fputc((char)c, out);
        }
    }
}

int apex_nvram_write_json(FILE *out, const ApexNvramLoc *items, size_t count,
                          const char *rom_name)
{
    size_t i;
    fputs("{\n", out);
    fputs("  \"_fileformat\": 0.8,\n", out);
    fputs("  \"_metadata\": {\n", out);
    fputs("    \"version\": 1.0,\n", out);
    fputs("    \"roms\": [", out);
    if (rom_name && rom_name[0]) {
        fputc('"', out);
        json_write_escaped(out, rom_name);
        fputc('"', out);
    }
    fputs("],\n", out);
    fputs("    \"platform\": \"williams-wpc\",\n", out);
    fputs("    \"_note\": \"exported by ApexII\"\n", out);
    fputs("  },\n", out);
    fputs("  \"game_state\": {\n", out);
    fputs("    \"ApexII RAM map\": {\n", out);
    for (i = 0; i < count; i++) {
        fputs("      \"", out);
        json_write_escaped(out, items[i].name);
        fputs("\": { \"short_label\": \"", out);
        json_write_escaped(out, items[i].name);
        fputs("\", \"label\": \"", out);
        json_write_escaped(out, items[i].doc ? items[i].doc : "");
        fprintf(out, "\", \"start\": \"0x%04x\", \"encoding\": \"raw\", \"length\": 1 }%s\n",
                items[i].addr & 0xffffu, (i + 1 < count) ? "," : "");
    }
    fputs("    }\n", out);
    fputs("  }\n", out);
    fputs("}\n", out);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Zero-loss (template-merge) export: JSON tree serializer + descriptor update.
 * ------------------------------------------------------------------------- */

static void jindent(FILE *f, int n) { while (n-- > 0) fputs("  ", f); }

static void jwrite_string(FILE *f, const char *s)
{
    fputc('"', f);
    json_write_escaped(f, s ? s : "");
    fputc('"', f);
}

static void jval_write(FILE *f, const JVal *v, int ind)
{
    size_t i;
    if (!v) { fputs("null", f); return; } /* tolerate an OOM-dropped node */
    switch (v->type) {
    case J_NULL: fputs("null", f); break;
    case J_BOOL: fputs(v->b ? "true" : "false", f); break;
    case J_NUM:
        /* Re-emit the original numeric text verbatim (zero-loss); only synthesized
           numbers, which have no source text, are formatted from the double. */
        if (v->num_raw) fputs(v->num_raw, f);
        else if (v->n == (double)(long long)v->n) fprintf(f, "%lld", (long long)v->n);
        else fprintf(f, "%.17g", v->n);
        break;
    case J_STR: jwrite_string(f, v->s); break;
    case J_ARR:
        if (v->arrn == 0) { fputs("[]", f); break; }
        fputs("[\n", f);
        for (i = 0; i < v->arrn; i++) {
            jindent(f, ind + 1);
            jval_write(f, v->arr[i], ind + 1);
            fputs(i + 1 < v->arrn ? ",\n" : "\n", f);
        }
        jindent(f, ind); fputc(']', f);
        break;
    case J_OBJ:
        if (v->objn == 0) { fputs("{}", f); break; }
        fputs("{\n", f);
        for (i = 0; i < v->objn; i++) {
            jindent(f, ind + 1);
            jwrite_string(f, v->keys[i]);
            fputs(": ", f);
            jval_write(f, v->vals[i], ind + 1);
            fputs(i + 1 < v->objn ? ",\n" : "\n", f);
        }
        jindent(f, ind); fputc('}', f);
        break;
    }
}

static JVal *jmk_str(const char *s) { JVal *v = calloc(1, sizeof(*v)); if (v) { v->type = J_STR; v->s = nv_strdup(s); } return v; }
static JVal *jmk_num(double n)      { JVal *v = calloc(1, sizeof(*v)); if (v) { v->type = J_NUM; v->n = n; } return v; }
static JVal *jmk_obj(void)          { JVal *v = calloc(1, sizeof(*v)); if (v)   v->type = J_OBJ; return v; }

static void jobj_add(JVal *o, const char *k, JVal *val)
{
    char **nk = realloc(o->keys, (o->objn + 1) * sizeof(char *));
    JVal **nv = realloc(o->vals, (o->objn + 1) * sizeof(JVal *));
    if (!nk || !nv) { free(nk ? nk : o->keys); jval_free(val); return; } /* OOM: drop */
    o->keys = nk;
    o->vals = nv;
    o->keys[o->objn] = nv_strdup(k);
    o->vals[o->objn] = val;
    o->objn++;
}

static void jobj_set_str(JVal *o, const char *k, const char *val)
{
    size_t i;
    for (i = 0; i < o->objn; i++) {
        if (strcmp(o->keys[i], k) == 0) {
            jval_free(o->vals[i]);
            o->vals[i] = jmk_str(val);
            return;
        }
    }
    jobj_add(o, k, jmk_str(val));
}

/* Update each template descriptor's short_label/label from the matching apex
   location (by start address), marking that location used. */
static void merge_walk(JVal *v, const ApexNvramLoc *items, size_t count, char *used)
{
    size_t i;
    if (!v) return;
    if (v->type == J_OBJ) {
        const JVal *start = jobj_get(v, "start");
        if (start) {
            uint32_t addr = 0;
            int ok = 0;
            if (start->type == J_STR) ok = parse_u32_str(start->s, &addr);
            else if (start->type == J_NUM) { addr = (uint32_t)start->n; ok = 1; }
            if (ok) {
                for (i = 0; i < count; i++) {
                    if (items[i].addr == addr && !used[i]) {
                        jobj_set_str(v, "short_label", items[i].name);
                        if (items[i].doc && items[i].doc[0])
                            jobj_set_str(v, "label", items[i].doc);
                        used[i] = 1;
                        break;
                    }
                }
            }
            return; /* leaf */
        }
        for (i = 0; i < v->objn; i++) {
            if (v->keys[i][0] == '_') continue;
            merge_walk(v->vals[i], items, count, used);
        }
    } else if (v->type == J_ARR) {
        for (i = 0; i < v->arrn; i++) merge_walk(v->arr[i], items, count, used);
    }
}

int apex_nvram_export_merged(FILE *out, const char *template_text, size_t template_len,
                             const ApexNvramLoc *items, size_t count,
                             char *err, size_t errsz)
{
    JParser jp;
    JVal   *root;
    char   *used;
    size_t  i, unused = 0;

    memset(&jp, 0, sizeof(jp));
    jp.p = template_text;
    jp.end = template_text + template_len;
    root = jparse_value(&jp);
    if (jp.error || !root || root->type != J_OBJ) {
        if (err) snprintf(err, errsz, "template parse error: %s", jp.errbuf);
        jval_free(root);
        return 1;
    }
    used = calloc(count ? count : 1, 1);
    if (!used) {
        if (err) snprintf(err, errsz, "out of memory");
        jval_free(root);
        return 1;
    }
    merge_walk(root, items, count, used);

    for (i = 0; i < count; i++) if (!used[i]) unused++;
    if (unused) {
        /* Re-fetch after each add so a node dropped on OOM (jobj_add frees it)
           becomes NULL here instead of a dangling pointer. */
        JVal *gs = (JVal *)jobj_get(root, "game_state");
        JVal *grp = NULL;
        if (!gs || gs->type != J_OBJ) {
            jobj_add(root, "game_state", jmk_obj());
            gs = (JVal *)jobj_get(root, "game_state");
        }
        if (gs && gs->type == J_OBJ) {
            grp = (JVal *)jobj_get(gs, "ApexII RAM map");
            if (!grp || grp->type != J_OBJ) {
                jobj_add(gs, "ApexII RAM map", jmk_obj());
                grp = (JVal *)jobj_get(gs, "ApexII RAM map");
            }
        }
        for (i = 0; grp && i < count; i++) {
            char sb[16];
            JVal *d;
            if (used[i]) continue;
            d = jmk_obj();
            jobj_add(d, "short_label", jmk_str(items[i].name));
            jobj_add(d, "label", jmk_str(items[i].doc ? items[i].doc : ""));
            snprintf(sb, sizeof(sb), "0x%04x", items[i].addr & 0xffffu);
            jobj_add(d, "start", jmk_str(sb));
            jobj_add(d, "encoding", jmk_str("raw"));
            jobj_add(d, "length", jmk_num(1));
            jobj_add(grp, items[i].name, d);
        }
    }
    jval_write(out, root, 0);
    fputc('\n', out);
    free(used);
    jval_free(root);
    return 0;
}

void apex_nvram_locs_free(ApexNvramLocs *locs)
{
    size_t i;
    if (!locs) return;
    for (i = 0; i < locs->count; i++) {
        free(locs->items[i].name);
        free(locs->items[i].doc);
    }
    free(locs->items);
    memset(locs, 0, sizeof(*locs));
}
