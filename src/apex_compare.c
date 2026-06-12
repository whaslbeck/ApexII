#include "apex_compare.h"

#include "apex.h"
#include "apex_project.h"
#include "apex_analysis.h"
#include "apex_config.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static uint32_t fnv32(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Locate the ROM bytes for a CPU (bank, addr).  Returns the available byte
   count from that point (0 if the address is not mapped). */
static size_t locate(const struct ApexProject *p, uint8_t bank, uint32_t addr,
                     const uint8_t **out)
{
    size_t offset;
    size_t total = p->banks * APEX_BANK_SIZE + APEX_SYSTEM_SIZE;

    if (bank == 0xffu || in_system_addr(addr)) {
        if (addr < APEX_SYSTEM_ORG || addr >= 0x10000u) {
            return 0;
        }
        offset = p->banks * APEX_BANK_SIZE + (size_t)(addr - APEX_SYSTEM_ORG);
    } else {
        int idx;
        if (addr < APEX_PAGED_ORG || addr >= 0x8000u) {
            return 0;
        }
        idx = bank_index_for_far_ref(p->rom.data, p->banks, bank);
        if (idx < 0) {
            return 0;
        }
        offset = (size_t)idx * APEX_BANK_SIZE + (size_t)(addr - APEX_PAGED_ORG);
    }
    if (offset >= total) {
        return 0;
    }
    *out = p->rom.data + offset;
    return total - offset;
}

static ApexCompareEntry *report_push(ApexCompareReport *r)
{
    if (r->count == r->cap) {
        size_t nc = r->cap == 0 ? 64 : r->cap * 2;
        ApexCompareEntry *ni = realloc(r->items, nc * sizeof(*ni));
        if (!ni) {
            return NULL;
        }
        r->items = ni;
        r->cap = nc;
    }
    memset(&r->items[r->count], 0, sizeof(r->items[0]));
    return &r->items[r->count++];
}

static void tally(ApexCompareReport *r, ApexCompareStatus s)
{
    switch (s) {
    case APEX_CMP_IDENTICAL: r->n_identical++; break;
    case APEX_CMP_MOVED:     r->n_moved++;     break;
    case APEX_CMP_CHANGED:   r->n_changed++;   break;
    case APEX_CMP_REMOVED:   r->n_removed++;   break;
    case APEX_CMP_ADDED:     r->n_added++;     break;
    }
}

static void emit(ApexCompareReport *r, ApexCompareKind kind, ApexCompareStatus status,
                 int has_a, uint8_t ab, uint32_t aa,
                 int has_b, uint8_t bb, uint32_t ba,
                 int conf, const char *label, const char *detail)
{
    ApexCompareEntry *e = report_push(r);

    if (!e) {
        return;
    }
    e->kind = kind;
    e->status = status;
    e->has_a = has_a;
    e->has_b = has_b;
    e->a_bank = ab;
    e->a_addr = aa;
    e->b_bank = bb;
    e->b_addr = ba;
    e->confidence = conf;
    if (label) {
        snprintf(e->label, sizeof(e->label), "%s", label);
    } else {
        snprintf(e->label, sizeof(e->label), "B%02x_A%04x",
                 has_a ? ab : bb, (unsigned)(has_a ? aa : ba) & 0xffffu);
    }
    if (detail) {
        snprintf(e->detail, sizeof(e->detail), "%s", detail);
    }
    tally(r, status);
}

/* ------------------------------------------------------------------ */
/* code comparison                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  bank;
    uint32_t addr;
    uint32_t l1;
    uint32_t l2;
    uint16_t instr;
    const char *name;   /* NULL for generated */
    int used;
} CFp;

static uint64_t addr_key(uint8_t bank, uint32_t addr)
{
    return ((uint64_t)bank << 32) | (addr & 0xffffffffu);
}

static CFp *build_cfps(const ApexFingerprintDB *db, int min_instrs, size_t *out_count)
{
    size_t n = apex_fingerprint_count(db);
    size_t i, k = 0;
    CFp *out = n ? malloc(n * sizeof(*out)) : NULL;

    for (i = 0; i < n; i++) {
        const ApexFunctionFingerprint *fp = apex_fingerprint_get(db, i);
        if (fp->instr_count < min_instrs) {
            continue;
        }
        out[k].bank = fp->bank;
        out[k].addr = fp->addr;
        out[k].l1 = fp->l1_hash;
        out[k].l2 = fp->l2_hash;
        out[k].instr = fp->instr_count;
        out[k].name = fp->label_name;
        out[k].used = 0;
        k++;
    }
    *out_count = k;
    return out;
}

static int cmp_idx_by_addr(const void *a, const void *b, void *ctx)
{
    const CFp *fps = ctx;
    uint64_t ka = addr_key(fps[*(const size_t *)a].bank, fps[*(const size_t *)a].addr);
    uint64_t kb = addr_key(fps[*(const size_t *)b].bank, fps[*(const size_t *)b].addr);
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

static int cmp_idx_by_body(const void *a, const void *b, void *ctx)
{
    const CFp *fps = ctx;
    const CFp *x = &fps[*(const size_t *)a];
    const CFp *y = &fps[*(const size_t *)b];
    if (x->l1 != y->l1) return x->l1 < y->l1 ? -1 : 1;
    if (x->l2 != y->l2) return x->l2 < y->l2 ? -1 : 1;
    return 0;
}

/* portable qsort_r wrapper via a small global is avoided; use insertion of a
   thread-local context through a static pointer is unsafe.  Instead sort index
   arrays with an inlined comparator using a file-static context pointer. */
static const CFp *g_sort_ctx;

static int qcmp_addr(const void *a, const void *b)
{
    return cmp_idx_by_addr(a, b, (void *)g_sort_ctx);
}

static int qcmp_body(const void *a, const void *b)
{
    return cmp_idx_by_body(a, b, (void *)g_sort_ctx);
}

/* Find index into idx[] (sorted by addr) whose CFp matches (bank,addr); -1 if none. */
static long find_by_addr(const CFp *fps, const size_t *idx, size_t n,
                         uint8_t bank, uint32_t addr)
{
    long lo = 0, hi = (long)n - 1;
    uint64_t key = addr_key(bank, addr);

    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        uint64_t k = addr_key(fps[idx[mid]].bank, fps[idx[mid]].addr);
        if (k == key) return mid;
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* First index into idx[] (sorted by body) matching (l1,l2); -1 if none. */
static long find_by_body(const CFp *fps, const size_t *idx, size_t n,
                         uint32_t l1, uint32_t l2)
{
    long lo = 0, hi = (long)n - 1, found = -1;

    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        const CFp *m = &fps[idx[mid]];
        if (m->l1 == l1 && m->l2 == l2) {
            found = mid;
            hi = mid - 1; /* keep searching left for the first */
        } else if (m->l1 < l1 || (m->l1 == l1 && m->l2 < l2)) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return found;
}

static void code_label(const CFp *a, char *buf, size_t cap)
{
    if (a->name) {
        snprintf(buf, cap, "%s", a->name);
    } else {
        snprintf(buf, cap, "B%02x_A%04x", a->bank, (unsigned)a->addr & 0xffffu);
    }
}

static void compare_code(const struct ApexProject *a, const struct ApexProject *b,
                         const ApexCompareOptions *opt, ApexCompareReport *r)
{
    ApexFingerprintDB *da = apex_fingerprint_build(a);
    ApexFingerprintDB *db = apex_fingerprint_build(b);
    CFp *af = NULL, *bf = NULL;
    size_t an = 0, bn = 0, i;
    size_t *b_addr = NULL, *b_body = NULL;
    char lbl[APEX_CMP_LABEL_MAX];

    if (!da || !db) {
        goto done;
    }
    af = build_cfps(da, opt->min_instrs, &an);
    bf = build_cfps(db, opt->min_instrs, &bn);

    b_addr = bn ? malloc(bn * sizeof(*b_addr)) : NULL;
    b_body = bn ? malloc(bn * sizeof(*b_body)) : NULL;
    for (i = 0; i < bn; i++) {
        b_addr[i] = i;
        b_body[i] = i;
    }
    g_sort_ctx = bf;
    if (bn) {
        qsort(b_addr, bn, sizeof(*b_addr), qcmp_addr);
        qsort(b_body, bn, sizeof(*b_body), qcmp_body);
    }

    for (i = 0; i < an; i++) {
        CFp *x = &af[i];
        long pos;
        uint32_t bl1 = 0, bl2 = 0;
        uint16_t bn_i;

        code_label(x, lbl, sizeof(lbl));

        /* Re-fingerprint B at A's address, capped to A's instruction count, so
           an unchanged routine hashes identically regardless of B's analysis. */
        bn_i = apex_fingerprint_raw(b, x->bank, x->addr, x->instr, &bl1, &bl2);

        /* 1. identical in place */
        if (bn_i == x->instr && bl1 == x->l1 && bl2 == x->l2) {
            pos = find_by_addr(bf, b_addr, bn, x->bank, x->addr);
            if (pos >= 0) {
                bf[b_addr[pos]].used = 1;
            }
            if (opt->include_identical) {
                emit(r, APEX_CMP_CODE, APEX_CMP_IDENTICAL, 1, x->bank, x->addr,
                     1, x->bank, x->addr, 100, lbl, "identical body");
            } else {
                r->n_identical++;
            }
            continue;
        }

        /* 2. exact body relocated elsewhere -> moved */
        pos = find_by_body(bf, b_body, bn, x->l1, x->l2);
        while (pos >= 0 && (size_t)pos < bn) {
            CFp *y = &bf[b_body[pos]];
            if (y->l1 != x->l1 || y->l2 != x->l2) {
                pos = -1;
                break;
            }
            if (!y->used && !(y->bank == x->bank && y->addr == x->addr)) {
                y->used = 1;
                emit(r, APEX_CMP_CODE, APEX_CMP_MOVED, 1, x->bank, x->addr,
                     1, y->bank, y->addr, 100, lbl, "relocated");
                break;
            }
            pos++;
        }
        if (pos >= 0 && (size_t)pos < bn && bf[b_body[pos]].l1 == x->l1 &&
            bf[b_body[pos]].l2 == x->l2 && bf[b_body[pos]].used) {
            continue; /* emitted as moved */
        }

        /* consume any B routine sitting at the same address */
        pos = find_by_addr(bf, b_addr, bn, x->bank, x->addr);
        if (pos >= 0) {
            bf[b_addr[pos]].used = 1;
        }

        /* 3. same structure in place, operands changed */
        if (bn_i > 0 && bl1 == x->l1) {
            emit(r, APEX_CMP_CODE, APEX_CMP_CHANGED, 1, x->bank, x->addr,
                 1, x->bank, x->addr, 70, lbl, "same flow, operands changed");
            continue;
        }

        /* 4. routine gone — different code in place (or B lacks bytes here) */
        emit(r, APEX_CMP_CODE, APEX_CMP_REMOVED, 1, x->bank, x->addr,
             bn_i > 0, x->bank, x->addr, 0, lbl,
             bn_i > 0 ? "replaced in place" : "only in A");
    }

    /* 4. added: B routines never used.  Skip those whose identical bytes also
       exist in A but are classified there as data (analysis asymmetry), so they
       aren't reported as spurious new code. */
    for (i = 0; i < bn; i++) {
        uint32_t al1 = 0, al2 = 0;
        uint16_t an_i;

        if (bf[i].used) {
            continue;
        }
        an_i = apex_fingerprint_raw(a, bf[i].bank, bf[i].addr, bf[i].instr, &al1, &al2);
        if (an_i == bf[i].instr && al1 == bf[i].l1 && al2 == bf[i].l2) {
            continue;
        }
        code_label(&bf[i], lbl, sizeof(lbl));
        emit(r, APEX_CMP_CODE, APEX_CMP_ADDED, 0, 0, 0,
             1, bf[i].bank, bf[i].addr, 0, lbl, "only in B");
    }

done:
    free(af);
    free(bf);
    free(b_addr);
    free(b_body);
    apex_fingerprint_free(da);
    apex_fingerprint_free(db);
}

/* ------------------------------------------------------------------ */
/* string comparison                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  bank;
    uint32_t addr;
    uint32_t hash;
    size_t   len;
    char     preview[40];
    int      used;
} SStr;

typedef struct {
    SStr  *items;
    size_t count;
    size_t cap;
} SStrSet;

static void sstr_push(SStrSet *set, uint8_t bank, uint32_t addr,
                      const uint8_t *data, size_t len)
{
    SStr *s;
    size_t i, k = 0;

    /* dedup by (bank, addr) */
    for (i = 0; i < set->count; i++) {
        if (set->items[i].bank == bank && set->items[i].addr == addr) {
            return;
        }
    }
    if (set->count == set->cap) {
        size_t nc = set->cap == 0 ? 128 : set->cap * 2;
        SStr *ni = realloc(set->items, nc * sizeof(*ni));
        if (!ni) {
            return;
        }
        set->items = ni;
        set->cap = nc;
    }
    s = &set->items[set->count++];
    s->bank = bank;
    s->addr = addr;
    s->hash = fnv32(data, len);
    s->len = len;
    s->used = 0;
    for (i = 0; i < len && k < sizeof(s->preview) - 1u; i++) {
        unsigned char c = data[i];
        s->preview[k++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    s->preview[k] = '\0';
}

static void collect_strings_from(const struct ApexProject *p, const LabelSet *ls,
                                 uint8_t bank, SStrSet *set)
{
    size_t i;

    for (i = 0; i < ls->count; i++) {
        const Label *lb = &ls->items[i];
        const uint8_t *data;
        size_t avail, len;

        if (!lb->is_string) {
            continue;
        }
        avail = locate(p, bank, lb->addr, &data);
        if (avail == 0) {
            continue;
        }
        len = valid_string_len(data, avail);
        if (len == 0) {
            continue;
        }
        sstr_push(set, bank, lb->addr, data, len);
    }
}

static void collect_strings(const struct ApexProject *p, SStrSet *set)
{
    size_t bi;

    for (bi = 0; bi < p->banks; bi++) {
        uint8_t bank_id = p->rom.data[bi * APEX_BANK_SIZE];
        collect_strings_from(p, &p->bank_labels[bi], bank_id, set);
    }
    collect_strings_from(p, &p->system_labels, 0xffu, set);
}

static SStr *sstr_find_content(SStrSet *set, uint32_t hash, size_t len)
{
    size_t i;
    for (i = 0; i < set->count; i++) {
        if (!set->items[i].used && set->items[i].hash == hash &&
            set->items[i].len == len) {
            return &set->items[i];
        }
    }
    return NULL;
}

static int sstr_has_content(const SStrSet *set, uint32_t hash, size_t len)
{
    size_t i;
    for (i = 0; i < set->count; i++) {
        if (set->items[i].hash == hash && set->items[i].len == len) {
            return 1;
        }
    }
    return 0;
}

static int sstr_has_addr(const SStrSet *set, uint8_t bank, uint32_t addr)
{
    size_t i;
    for (i = 0; i < set->count; i++) {
        if (set->items[i].bank == bank && set->items[i].addr == addr) {
            return 1;
        }
    }
    return 0;
}

/* Mark the B-side string discovered at (bank,addr) consumed, if any. */
static void sstr_consume_addr(SStrSet *set, uint8_t bank, uint32_t addr)
{
    size_t i;
    for (i = 0; i < set->count; i++) {
        if (set->items[i].bank == bank && set->items[i].addr == addr) {
            set->items[i].used = 1;
            return;
        }
    }
}

static void compare_strings(const struct ApexProject *a, const struct ApexProject *b,
                            const ApexCompareOptions *opt, ApexCompareReport *r)
{
    SStrSet sa = {0}, sb = {0};
    size_t i;

    collect_strings(a, &sa);
    collect_strings(b, &sb);

    for (i = 0; i < sa.count; i++) {
        SStr *x = &sa.items[i];
        SStr *y;
        const uint8_t *bdata;
        size_t bavail;
        char det[APEX_CMP_DETAIL_MAX];

        snprintf(det, sizeof(det), "\"%s\"", x->preview);

        /* Compare A's string against B's raw bytes at the same address, so an
           unchanged string matches regardless of whether B's analysis flagged
           it. */
        bavail = locate(b, x->bank, x->addr, &bdata);
        if (bavail >= x->len && fnv32(bdata, x->len) == x->hash) {
            sstr_consume_addr(&sb, x->bank, x->addr);
            if (opt->include_identical) {
                emit(r, APEX_CMP_STRING, APEX_CMP_IDENTICAL, 1, x->bank, x->addr,
                     1, x->bank, x->addr, 100, NULL, det);
            } else {
                r->n_identical++;
            }
            continue;
        }
        if (bavail > 0 && valid_string_len(bdata, bavail) > 0) {
            sstr_consume_addr(&sb, x->bank, x->addr);
            emit(r, APEX_CMP_STRING, APEX_CMP_CHANGED, 1, x->bank, x->addr,
                 1, x->bank, x->addr, 60, NULL, det);
            continue;
        }
        /* Not a string in place — look for the same content elsewhere in B. */
        y = sstr_find_content(&sb, x->hash, x->len);
        if (y) {
            y->used = 1;
            emit(r, APEX_CMP_STRING, APEX_CMP_MOVED, 1, x->bank, x->addr,
                 1, y->bank, y->addr, 100, NULL, det);
            continue;
        }
        emit(r, APEX_CMP_STRING, APEX_CMP_REMOVED, 1, x->bank, x->addr,
             0, 0, 0, 0, NULL, det);
    }

    /* B strings with no A counterpart by content or address -> added */
    for (i = 0; i < sb.count; i++) {
        SStr *y = &sb.items[i];
        char det[APEX_CMP_DETAIL_MAX];
        if (y->used) {
            continue;
        }
        if (sstr_has_content(&sa, y->hash, y->len) || sstr_has_addr(&sa, y->bank, y->addr)) {
            continue;
        }
        snprintf(det, sizeof(det), "\"%s\"", y->preview);
        emit(r, APEX_CMP_STRING, APEX_CMP_ADDED, 0, 0, 0,
             1, y->bank, y->addr, 0, NULL, det);
    }

    free(sa.items);
    free(sb.items);
}

/* ------------------------------------------------------------------ */
/* table comparison                                                     */
/* ------------------------------------------------------------------ */

static size_t table_byte_len(const TableDef *t, const uint8_t *data, size_t avail)
{
    size_t width = table_schema_width(&t->schema);

    if (t->has_header) {
        uint16_t rows;
        uint8_t rw;
        if (avail < 3) {
            return 0;
        }
        rows = read_be16(data);
        rw = data[2];
        return 3u + (size_t)rows * rw;
    }
    return (size_t)t->rows * width;
}

static const TableDef *table_at(const TableDefs *tables, uint8_t bank, uint32_t addr)
{
    size_t i;
    for (i = 0; i < tables->count; i++) {
        if (tables->items[i].bank == bank && tables->items[i].addr == addr) {
            return &tables->items[i];
        }
    }
    return NULL;
}

static void compare_tables(const struct ApexProject *a, const struct ApexProject *b,
                           const ApexCompareOptions *opt, ApexCompareReport *r)
{
    size_t i;

    for (i = 0; i < a->tables.count; i++) {
        const TableDef *t = &a->tables.items[i];
        const uint8_t *ad, *bd;
        size_t aavail, bavail, span;
        char det[APEX_CMP_DETAIL_MAX];

        aavail = locate(a, t->bank, t->addr, &ad);
        if (aavail == 0) {
            continue;
        }
        span = table_byte_len(t, ad, aavail);
        if (span == 0 || span > aavail) {
            continue;
        }
        snprintf(det, sizeof(det), "%lu bytes", (unsigned long)span);

        bavail = locate(b, t->bank, t->addr, &bd);
        if (bavail < span) {
            emit(r, APEX_CMP_TABLE, APEX_CMP_REMOVED, 1, t->bank, t->addr,
                 0, 0, 0, 0, NULL, det);
            continue;
        }
        if (memcmp(ad, bd, span) == 0) {
            if (opt->include_identical) {
                emit(r, APEX_CMP_TABLE, APEX_CMP_IDENTICAL, 1, t->bank, t->addr,
                     1, t->bank, t->addr, 100, NULL, det);
            } else {
                r->n_identical++;
            }
        } else {
            emit(r, APEX_CMP_TABLE, APEX_CMP_CHANGED, 1, t->bank, t->addr,
                 1, t->bank, t->addr, 60, NULL, det);
        }
    }

    /* B-only tables (only discoverable when B carries its own table config) */
    for (i = 0; i < b->tables.count; i++) {
        const TableDef *t = &b->tables.items[i];
        char det[APEX_CMP_DETAIL_MAX];
        if (table_at(&a->tables, t->bank, t->addr)) {
            continue;
        }
        snprintf(det, sizeof(det), "rows=%lu", (unsigned long)t->rows);
        emit(r, APEX_CMP_TABLE, APEX_CMP_ADDED, 0, 0, 0,
             1, t->bank, t->addr, 0, NULL, det);
    }
}

/* ------------------------------------------------------------------ */
/* public                                                               */
/* ------------------------------------------------------------------ */

void apex_compare_default_options(ApexCompareOptions *o)
{
    if (!o) {
        return;
    }
    o->include_code = 1;
    o->include_strings = 1;
    o->include_tables = 1;
    o->include_identical = 0;
    o->min_instrs = 5;
}

int apex_compare_run(const struct ApexProject *a, const struct ApexProject *b,
                     const ApexCompareOptions *opt, ApexCompareReport *out)
{
    ApexCompareOptions defaults;

    if (!a || !b || !out) {
        return 1;
    }
    if (!opt) {
        apex_compare_default_options(&defaults);
        opt = &defaults;
    }
    memset(out, 0, sizeof(*out));

    if (opt->include_code) {
        compare_code(a, b, opt, out);
    }
    if (opt->include_strings) {
        compare_strings(a, b, opt, out);
    }
    if (opt->include_tables) {
        compare_tables(a, b, opt, out);
    }
    return 0;
}

void apex_compare_report_free(ApexCompareReport *r)
{
    if (!r) {
        return;
    }
    free(r->items);
    memset(r, 0, sizeof(*r));
}

const char *apex_compare_status_name(ApexCompareStatus s)
{
    switch (s) {
    case APEX_CMP_IDENTICAL: return "identical";
    case APEX_CMP_MOVED:     return "moved";
    case APEX_CMP_CHANGED:   return "changed";
    case APEX_CMP_REMOVED:   return "removed";
    case APEX_CMP_ADDED:     return "added";
    }
    return "?";
}

const char *apex_compare_kind_name(ApexCompareKind k)
{
    switch (k) {
    case APEX_CMP_CODE:   return "code";
    case APEX_CMP_STRING: return "string";
    case APEX_CMP_TABLE:  return "table";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* cursor sidebar candidates                                            */
/* ------------------------------------------------------------------ */

size_t apex_compare_candidates(const ApexFingerprintDB *a_db,
                               const ApexFingerprintDB *b_db,
                               uint8_t a_bank, uint32_t a_addr,
                               ApexCompareCandidate *out, size_t max_out)
{
    const ApexFunctionFingerprint *fp = apex_fingerprint_at(a_db, a_bank, a_addr);
    size_t nb, i, n = 0;

    if (!fp || !out || max_out == 0) {
        return 0;
    }
    nb = apex_fingerprint_count(b_db);
    for (i = 0; i < nb; i++) {
        const ApexFunctionFingerprint *b = apex_fingerprint_get(b_db, i);
        int conf = 0, exact = 0;
        size_t j, ins;

        if (fp->l1_hash == b->l1_hash) {
            if (fp->l2_hash == b->l2_hash) {
                conf = 100;
                exact = 1;
            } else {
                conf = 60;
            }
            if (b->bank == a_bank && b->addr == a_addr) {
                conf = conf < 95 ? conf + 5 : conf;
            }
        } else if (fp->callee_hash && fp->callee_hash == b->callee_hash) {
            conf = 40;
        } else {
            continue;
        }

        /* insertion sort into top-N by confidence (descending) */
        if (n < max_out) {
            ins = n++;
        } else if (conf > out[max_out - 1].confidence) {
            ins = max_out - 1;
        } else {
            continue;
        }
        for (j = ins; j > 0 && out[j - 1].confidence < conf; j--) {
            out[j] = out[j - 1];
        }
        out[j].bank = b->bank;
        out[j].addr = b->addr;
        out[j].confidence = conf;
        out[j].exact = exact;
    }
    return n;
}
