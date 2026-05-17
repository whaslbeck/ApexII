/* apexini — INI config utilities for ApexII projects.
 *
 * Subcommands:
 *   check   <file.ini> ...        syntax-check one or more config files
 *   overlaps <file.ini>           report address conflicts and range overlaps
 *   merge   <out.ini> <file.ini>... merge multiple configs into one sorted file
 */
#include "apex.h"
#include "apex_config.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── die() interception ─────────────────────────────────────────────────── */

static jmp_buf s_jmp;
static char    s_err[512];
static int     s_catching;

static void catch_die(const char *msg)
{
    if (s_catching) {
        strncpy(s_err, msg, sizeof(s_err) - 1u);
        s_err[sizeof(s_err) - 1u] = '\0';
        longjmp(s_jmp, 1);
    }
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

/* ── config bundle ──────────────────────────────────────────────────────── */

typedef struct {
    InlineSignatures sigs;
    ConfigLabels     labels;
    ConfigEntries    entries;
    TableDefs        tables;
    SchemaDefs       schemas;
    ConfigDocs       rdocs;
    ConfigDocs       tdocs;
    ConfigSymbols    syms;
    DataRanges       data;
    ConfigOptions    opts;
    ConfigTypes      types;
} Cfg;

static int cfg_load(Cfg *c, const char *path)
{
    int failed;

    s_catching = 1;
    failed = setjmp(s_jmp);
    if (!failed) {
        load_config(path, &c->sigs, &c->labels, &c->entries, &c->tables,
                    &c->schemas, &c->rdocs, &c->tdocs, &c->syms, &c->data,
                    &c->opts, &c->types, NULL);
    }
    s_catching = 0;
    return failed;
}

static void cfg_free(Cfg *c)
{
    size_t i;

    for (i = 0; i < c->sigs.count; i++)
        free(c->sigs.items[i].schema.items);
    free(c->sigs.items);

    for (i = 0; i < c->labels.count; i++)
        free(c->labels.items[i].name);
    free(c->labels.items);

    free(c->entries.items);

    for (i = 0; i < c->tables.count; i++)
        free(c->tables.items[i].schema.items);
    free(c->tables.items);

    for (i = 0; i < c->schemas.count; i++) {
        free((char *)c->schemas.items[i].name);
        free(c->schemas.items[i].schema.items);
    }
    free(c->schemas.items);

    for (i = 0; i < c->rdocs.count; i++)
        free(c->rdocs.items[i].text);
    free(c->rdocs.items);

    for (i = 0; i < c->tdocs.count; i++)
        free(c->tdocs.items[i].text);
    free(c->tdocs.items);

    for (i = 0; i < c->syms.count; i++)
        free((char *)c->syms.items[i].name);
    free(c->syms.items);

    free(c->data.items);
    free_config_types(&c->types);
    memset(c, 0, sizeof(*c));
}

/* ── address comparison ─────────────────────────────────────────────────── */

static int addr_cmp(int ha, uint8_t ba, uint32_t aa,
                    int hb, uint8_t bb, uint32_t ab)
{
    uint8_t ea = ha ? ba : 0xffu;
    uint8_t eb = hb ? bb : 0xffu;

    if (ea != eb)
        return (int)(unsigned)ea - (int)(unsigned)eb;
    if (aa != ab)
        return aa < ab ? -1 : 1;
    return 0;
}

/* ── write helpers ──────────────────────────────────────────────────────── */

static void w_addr(FILE *f, int has_bank, uint8_t bank, uint32_t addr)
{
    if (has_bank)
        fprintf(f, "B%02x_A%04x", bank, (unsigned)addr & 0xffffu);
    else
        fprintf(f, "0x%04x", (unsigned)addr & 0xffffu);
}

static void w_escaped(FILE *f, const char *v)
{
    const char *p;
    int q = 0;

    if (!v) { fputs("\"\"", f); return; }
    for (p = v; *p; p++) {
        if (*p == '\n' || *p == ';' || *p == '#' || *p == '\\' ||
            *p == '"' || isspace((unsigned char)*p)) {
            q = 1; break;
        }
    }
    if (!q) { fputs(v, f); return; }
    fputc('"', f);
    for (p = v; *p; p++) {
        switch (*p) {
        case '\n': fputs("\\n",  f); break;
        case ';':  fputs("\\;",  f); break;
        case '#':  fputs("\\#",  f); break;
        case '\\': fputs("\\\\", f); break;
        case '"':  fputs("\\\"", f); break;
        default:   fputc(*p,     f); break;
        }
    }
    fputc('"', f);
}

static const char *kind_name(TableFieldKind k)
{
    switch (k) {
    case TABLE_PTR16_STRING:        return "ptr16_string";
    case TABLE_PTR16_DATA:          return "ptr16_data";
    case TABLE_PTR16_CODE:          return "ptr16_code";
    case TABLE_PTR16_TABLE:         return "ptr16_table";
    case TABLE_PTR16_DMD_FULLFRAME: return "ptr16_dmd_fullframe";
    case TABLE_FAR_STRING:          return "far_string";
    case TABLE_FAR_DATA:            return "far_data";
    case TABLE_FAR_TABLE:           return "far_table";
    case TABLE_FAR_CODE:            return "far_code";
    case TABLE_FAR_DMD_FULLFRAME:   return "far_dmd_fullframe";
    case TABLE_BYTE:                return "byte";
    case TABLE_WORD:                return "word";
    }
    return "byte";
}

static void w_schema(FILE *f, const TableSchema *s)
{
    size_t i;

    for (i = 0; i < s->count; i++) {
        if (i) fputs(", ", f);
        fputs(s->items[i].type_name ? s->items[i].type_name
                                    : kind_name(s->items[i].kind), f);
        if (s->items[i].count != 1u)
            fprintf(f, "[%lu]", (unsigned long)s->items[i].count);
    }
}

static void w_data_val(FILE *f, const DataRange *r)
{
    switch (r->kind) {
    case DATA_BYTES:           fprintf(f, "bytes[%lu]", (unsigned long)r->length); break;
    case DATA_STRING:          fputs("string",           f); break;
    case DATA_DMD_FULLFRAME:   fputs("dmd_fullframe",    f); break;
    case DATA_FAR_STRING:      fputs("far_string",       f); break;
    case DATA_FAR_DATA:        fputs("far_data",         f); break;
    case DATA_FAR_TABLE:       fputs("far_table",        f); break;
    case DATA_FAR_CODE:        fputs("far_code",         f); break;
    case DATA_FAR_DMD_FULLFRAME: fputs("far_dmd_fullframe", f); break;
    }
}

static void w_table_val(FILE *f, const TableDef *t)
{
    if (t->has_header) {
        fputs("counted(", f);
        w_schema(f, &t->schema);
        fputc(')', f);
    } else if (t->schema.count == 1 && t->schema.items[0].kind == TABLE_FAR_CODE &&
               t->schema.items[0].type_name == NULL) {
        fprintf(f, "far_code[%lu]", (unsigned long)t->rows);
    } else {
        fprintf(f, "rows[%lu](", (unsigned long)t->rows);
        w_schema(f, &t->schema);
        fputc(')', f);
    }
}

/* ── qsort comparators ──────────────────────────────────────────────────── */

static int cmp_label(const void *a, const void *b)
{
    const ConfigLabel *la = (const ConfigLabel *)a;
    const ConfigLabel *lb = (const ConfigLabel *)b;
    return addr_cmp(la->has_bank, la->bank, la->addr,
                    lb->has_bank, lb->bank, lb->addr);
}

static int cmp_entry(const void *a, const void *b)
{
    const ConfigEntry *ea = (const ConfigEntry *)a;
    const ConfigEntry *eb = (const ConfigEntry *)b;
    return addr_cmp(ea->has_bank, ea->bank, ea->addr,
                    eb->has_bank, eb->bank, eb->addr);
}

static int cmp_sig(const void *a, const void *b)
{
    const InlineSignature *sa = (const InlineSignature *)a;
    const InlineSignature *sb = (const InlineSignature *)b;
    return addr_cmp(sa->has_bank, sa->bank, sa->addr,
                    sb->has_bank, sb->bank, sb->addr);
}

static int cmp_table(const void *a, const void *b)
{
    const TableDef *ta = (const TableDef *)a;
    const TableDef *tb = (const TableDef *)b;
    return addr_cmp(1, ta->bank, ta->addr, 1, tb->bank, tb->addr);
}

static int cmp_data(const void *a, const void *b)
{
    const DataRange *da = (const DataRange *)a;
    const DataRange *db = (const DataRange *)b;
    return addr_cmp(1, da->bank, da->addr, 1, db->bank, db->addr);
}

static int cmp_type(const void *a, const void *b)
{
    return strcmp(((const ConfigType *)a)->name, ((const ConfigType *)b)->name);
}

static int cmp_schema(const void *a, const void *b)
{
    return strcmp(((const SchemaDef *)a)->name, ((const SchemaDef *)b)->name);
}

static int cmp_sym(const void *a, const void *b)
{
    return strcmp(((const ConfigSymbol *)a)->name, ((const ConfigSymbol *)b)->name);
}

static int cmp_doc(const void *a, const void *b)
{
    const ConfigDoc *da = (const ConfigDoc *)a;
    const ConfigDoc *db = (const ConfigDoc *)b;
    return addr_cmp(da->has_bank, da->bank, da->addr,
                    db->has_bank, db->bank, db->addr);
}

/* ── write a complete config ────────────────────────────────────────────── */

static void write_cfg(FILE *f, Cfg *c)
{
    size_t i, j;

    if (c->types.count)   qsort(c->types.items,   c->types.count,   sizeof(c->types.items[0]),   cmp_type);
    if (c->schemas.count) qsort(c->schemas.items,  c->schemas.count, sizeof(c->schemas.items[0]), cmp_schema);
    if (c->syms.count)    qsort(c->syms.items,     c->syms.count,    sizeof(c->syms.items[0]),    cmp_sym);
    if (c->labels.count)  qsort(c->labels.items,   c->labels.count,  sizeof(c->labels.items[0]),  cmp_label);
    if (c->entries.count) qsort(c->entries.items,   c->entries.count, sizeof(c->entries.items[0]), cmp_entry);
    if (c->sigs.count)    qsort(c->sigs.items,     c->sigs.count,    sizeof(c->sigs.items[0]),    cmp_sig);
    if (c->data.count)    qsort(c->data.items,     c->data.count,    sizeof(c->data.items[0]),    cmp_data);
    if (c->tables.count)  qsort(c->tables.items,   c->tables.count,  sizeof(c->tables.items[0]),  cmp_table);
    if (c->rdocs.count)   qsort(c->rdocs.items,    c->rdocs.count,   sizeof(c->rdocs.items[0]),   cmp_doc);
    if (c->tdocs.count)   qsort(c->tdocs.items,    c->tdocs.count,   sizeof(c->tdocs.items[0]),   cmp_doc);

    if (c->opts.labels_are_entries)
        fputs("[options]\nlabels_are_entries = true\n", f);

    if (c->types.count) {
        fputs("\n[types]\n", f);
        for (i = 0; i < c->types.count; i++) {
            const ConfigType *t = &c->types.items[i];
            fprintf(f, "%s:%s =", t->name, t->kind == TABLE_BYTE ? "byte" : "word");
            for (j = 0; j < t->value_count; j++) {
                if (j) fputc(',', f);
                fprintf(f, " 0x%02x:%s", t->values[j].value, t->values[j].name);
            }
            fputc('\n', f);
        }
    }

    if (c->schemas.count) {
        fputs("\n[schemas]\n", f);
        for (i = 0; i < c->schemas.count; i++) {
            fputs(c->schemas.items[i].name, f);
            fputs(" = ", f);
            w_schema(f, &c->schemas.items[i].schema);
            fputc('\n', f);
        }
    }

    if (c->syms.count) {
        fputs("\n[symbols]\n", f);
        for (i = 0; i < c->syms.count; i++)
            fprintf(f, "%s = 0x%04x\n", c->syms.items[i].name, c->syms.items[i].value);
    }

    if (c->labels.count) {
        fputs("\n[labels]\n", f);
        for (i = 0; i < c->labels.count; i++) {
            w_addr(f, c->labels.items[i].has_bank, c->labels.items[i].bank,
                   c->labels.items[i].addr);
            fputs(" = ", f);
            w_escaped(f, c->labels.items[i].name);
            fputc('\n', f);
        }
    }

    if (c->entries.count) {
        fputs("\n[entries]\n", f);
        for (i = 0; i < c->entries.count; i++) {
            w_addr(f, c->entries.items[i].has_bank, c->entries.items[i].bank,
                   c->entries.items[i].addr);
            fputs(" = code\n", f);
        }
    }

    if (c->sigs.count) {
        fputs("\n[inline]\n", f);
        for (i = 0; i < c->sigs.count; i++) {
            w_addr(f, c->sigs.items[i].has_bank, c->sigs.items[i].bank,
                   c->sigs.items[i].addr);
            fputs(" = ", f);
            w_schema(f, &c->sigs.items[i].schema);
            fputc('\n', f);
        }
    }

    if (c->data.count) {
        fputs("\n[data]\n", f);
        for (i = 0; i < c->data.count; i++) {
            w_addr(f, 1, c->data.items[i].bank, c->data.items[i].addr);
            fputs(" = ", f);
            w_data_val(f, &c->data.items[i]);
            fputc('\n', f);
        }
    }

    if (c->tables.count) {
        fputs("\n[tables]\n", f);
        for (i = 0; i < c->tables.count; i++) {
            w_addr(f, 1, c->tables.items[i].bank, c->tables.items[i].addr);
            fputs(" = ", f);
            w_table_val(f, &c->tables.items[i]);
            fputc('\n', f);
        }
    }

    if (c->rdocs.count) {
        fputs("\n[routine_docs]\n", f);
        for (i = 0; i < c->rdocs.count; i++) {
            w_addr(f, c->rdocs.items[i].has_bank, c->rdocs.items[i].bank,
                   c->rdocs.items[i].addr);
            fputs(" = ", f);
            w_escaped(f, c->rdocs.items[i].text);
            fputc('\n', f);
        }
    }

    if (c->tdocs.count) {
        fputs("\n[table_docs]\n", f);
        for (i = 0; i < c->tdocs.count; i++) {
            w_addr(f, c->tdocs.items[i].has_bank, c->tdocs.items[i].bank,
                   c->tdocs.items[i].addr);
            fputs(" = ", f);
            w_escaped(f, c->tdocs.items[i].text);
            fputc('\n', f);
        }
    }
}

/* ── check command ──────────────────────────────────────────────────────── */

static int cmd_check(int argc, char **argv)
{
    int any_err = 0;
    int i;

    if (argc < 1) {
        fputs("usage: apexini check <file.ini> ...\n", stderr);
        return 2;
    }
    for (i = 0; i < argc; i++) {
        Cfg c;
        memset(&c, 0, sizeof(c));
        if (cfg_load(&c, argv[i])) {
            fprintf(stderr, "%s: error: %s\n", argv[i], s_err);
            any_err = 1;
        } else {
            printf("%s: OK  labels=%zu  entries=%zu  inline=%zu  data=%zu  tables=%zu  types=%zu\n",
                   argv[i], c.labels.count, c.entries.count, c.sigs.count,
                   c.data.count, c.tables.count, c.types.count);
        }
        cfg_free(&c);
    }
    return any_err ? 1 : 0;
}

/* ── overlaps command ───────────────────────────────────────────────────── */

typedef struct {
    int      has_bank;
    uint8_t  bank;
    uint32_t addr;
    uint32_t end;    /* exclusive; 0 = unknown length */
    char     section[12];
    char     spec[64];
} OvlEntry;

static int cmp_ovl(const void *a, const void *b)
{
    const OvlEntry *oa = (const OvlEntry *)a;
    const OvlEntry *ob = (const OvlEntry *)b;
    int r = addr_cmp(oa->has_bank, oa->bank, oa->addr,
                     ob->has_bank, ob->bank, ob->addr);

    return r ? r : strcmp(oa->section, ob->section);
}

static uint32_t data_end(const DataRange *r)
{
    size_t len;

    switch (r->kind) {
    case DATA_BYTES:           len = r->length; break;
    case DATA_DMD_FULLFRAME:   len = 512u;      break;
    case DATA_FAR_STRING:
    case DATA_FAR_DATA:
    case DATA_FAR_TABLE:
    case DATA_FAR_CODE:
    case DATA_FAR_DMD_FULLFRAME: len = 3u;      break;
    default:                   len = 0u;         break;
    }
    return len ? r->addr + (uint32_t)len : 0u;
}

static uint32_t table_end(const TableDef *t)
{
    size_t w;

    if (t->has_header || t->rows == 0)
        return 0u;
    w = table_schema_width(&t->schema);
    return w ? t->addr + (uint32_t)(t->rows * w) : 0u;
}

static OvlEntry *ovl_push(OvlEntry **arr, size_t *cnt, size_t *cap)
{
    if (*cnt == *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        OvlEntry *na = realloc(*arr, nc * sizeof(*na));
        if (!na) { fputs("out of memory\n", stderr); exit(1); }
        *arr = na;
        *cap = nc;
    }
    return &(*arr)[(*cnt)++];
}

static void fmt_addr(char *buf, size_t cap, int has_bank, uint8_t bank, uint32_t addr)
{
    if (has_bank)
        snprintf(buf, cap, "B%02x_A%04x", bank, (unsigned)addr & 0xffffu);
    else
        snprintf(buf, cap, "0x%04x", (unsigned)addr & 0xffffu);
}

static int cmd_overlaps(int argc, char **argv)
{
    Cfg c;
    OvlEntry *ovl = NULL;
    memset(&c, 0, sizeof(c));
    size_t count = 0, cap = 0;
    size_t i, j;
    int issues = 0;

    if (argc < 1) {
        fputs("usage: apexini overlaps <file.ini>\n", stderr);
        return 2;
    }
    if (cfg_load(&c, argv[0])) {
        fprintf(stderr, "%s: error: %s\n", argv[0], s_err);
        return 1;
    }

    /* collect classified address ranges */
    for (i = 0; i < c.sigs.count; i++) {
        OvlEntry *e = ovl_push(&ovl, &count, &cap);
        e->has_bank = c.sigs.items[i].has_bank;
        e->bank     = c.sigs.items[i].bank;
        e->addr     = c.sigs.items[i].addr;
        e->end      = c.sigs.items[i].addr + c.sigs.items[i].length;
        strcpy(e->section, "inline");
        snprintf(e->spec, sizeof(e->spec), "%u bytes", c.sigs.items[i].length);
    }
    for (i = 0; i < c.data.count; i++) {
        OvlEntry *e = ovl_push(&ovl, &count, &cap);
        e->has_bank = 1;
        e->bank     = c.data.items[i].bank;
        e->addr     = c.data.items[i].addr;
        e->end      = data_end(&c.data.items[i]);
        strcpy(e->section, "data");
        /* reuse w_data_val via snprintf/FILE is awkward; inline it */
        switch (c.data.items[i].kind) {
        case DATA_BYTES:
            snprintf(e->spec, sizeof(e->spec), "bytes[%lu]",
                     (unsigned long)c.data.items[i].length); break;
        case DATA_STRING:          strcpy(e->spec, "string");           break;
        case DATA_DMD_FULLFRAME:   strcpy(e->spec, "dmd_fullframe");    break;
        case DATA_FAR_STRING:      strcpy(e->spec, "far_string");       break;
        case DATA_FAR_DATA:        strcpy(e->spec, "far_data");         break;
        case DATA_FAR_TABLE:       strcpy(e->spec, "far_table");        break;
        case DATA_FAR_CODE:        strcpy(e->spec, "far_code");         break;
        case DATA_FAR_DMD_FULLFRAME: strcpy(e->spec, "far_dmd_fullframe"); break;
        }
    }
    for (i = 0; i < c.tables.count; i++) {
        OvlEntry *e = ovl_push(&ovl, &count, &cap);
        e->has_bank = 1;
        e->bank     = c.tables.items[i].bank;
        e->addr     = c.tables.items[i].addr;
        e->end      = table_end(&c.tables.items[i]);
        strcpy(e->section, "tables");
        if (c.tables.items[i].has_header)
            strcpy(e->spec, "counted(...)");
        else if (c.tables.items[i].schema.count == 1 &&
                 c.tables.items[i].schema.items[0].kind == TABLE_FAR_CODE &&
                 c.tables.items[i].schema.items[0].type_name == NULL)
            snprintf(e->spec, sizeof(e->spec), "far_code[%zu]", c.tables.items[i].rows);
        else
            snprintf(e->spec, sizeof(e->spec), "rows[%zu](...)", c.tables.items[i].rows);
    }
    for (i = 0; i < c.entries.count; i++) {
        OvlEntry *e = ovl_push(&ovl, &count, &cap);
        e->has_bank = c.entries.items[i].has_bank;
        e->bank     = c.entries.items[i].bank;
        e->addr     = c.entries.items[i].addr;
        e->end      = 0;
        strcpy(e->section, "entries");
        strcpy(e->spec, "code");
    }

    if (count == 0) {
        printf("%s: no classified addresses\n", argv[0]);
        cfg_free(&c);
        return 0;
    }

    qsort(ovl, count, sizeof(ovl[0]), cmp_ovl);

    /* pass 1: same-address conflicts (different sections at same addr) */
    for (i = 0; i < count; ) {
        /* find all entries at this address */
        j = i + 1;
        while (j < count &&
               addr_cmp(ovl[i].has_bank, ovl[i].bank, ovl[i].addr,
                        ovl[j].has_bank, ovl[j].bank, ovl[j].addr) == 0)
            j++;
        if (j > i + 1) {
            /* Multiple entries at same address - check for different sections */
            size_t k, l;
            char key[16];
            fmt_addr(key, sizeof(key), ovl[i].has_bank, ovl[i].bank, ovl[i].addr);
            for (k = i; k < j; k++) {
                for (l = k + 1; l < j; l++) {
                    if (strcmp(ovl[k].section, ovl[l].section) != 0) {
                        printf("conflict: %s  [%s] %s  vs  [%s] %s\n",
                               key, ovl[k].section, ovl[k].spec,
                               ovl[l].section, ovl[l].spec);
                        issues++;
                    }
                }
            }
        }
        i = j;
    }

    /* pass 2: range overlaps (entry A's range extends into entry B's start) */
    for (i = 0; i < count; i++) {
        uint8_t ba;

        if (ovl[i].end == 0) continue; /* unknown length */
        ba = ovl[i].has_bank ? ovl[i].bank : 0xffu;

        for (j = i + 1; j < count; j++) {
            uint8_t bb = ovl[j].has_bank ? ovl[j].bank : 0xffu;

            if (bb != ba) break;          /* different bank (sorted) */
            if (ovl[j].addr >= ovl[i].end) break; /* no more overlaps */
            if (ovl[j].addr == ovl[i].addr) continue; /* same-addr, already reported */

            {
                char ka[16], kb[16];
                fmt_addr(ka, sizeof(ka), ovl[i].has_bank, ovl[i].bank, ovl[i].addr);
                fmt_addr(kb, sizeof(kb), ovl[j].has_bank, ovl[j].bank, ovl[j].addr);
                printf("overlap: [%s] %s (%s, ends 0x%04x) into [%s] %s (%s)\n",
                       ovl[i].section, ka, ovl[i].spec, ovl[i].end - 1u,
                       ovl[j].section, kb, ovl[j].spec);
                issues++;
            }
        }
    }

    if (issues == 0)
        printf("%s: no conflicts or overlaps found\n", argv[0]);

    free(ovl);
    cfg_free(&c);
    return issues > 0 ? 1 : 0;
}

/* ── merge command ──────────────────────────────────────────────────────── */

static int cmd_merge(int argc, char **argv)
{
    Cfg c;
    FILE *out;
    memset(&c, 0, sizeof(c));
    int i;

    if (argc < 2) {
        fputs("usage: apexini merge <out.ini> <file.ini> ...\n", stderr);
        return 2;
    }

    /* Load all input files sequentially into one config.
     * load_config's add_* helpers handle deduplication (same address = update).
     * Conflicts (e.g. same label name at different addresses) cause die(),
     * which we catch and report. */
    for (i = 1; i < argc; i++) {
        if (cfg_load(&c, argv[i])) {
            fprintf(stderr, "%s: merge conflict: %s\n", argv[i], s_err);
            cfg_free(&c);
            return 1;
        }
    }

    out = fopen(argv[0], "w");
    if (!out) {
        perror(argv[0]);
        cfg_free(&c);
        return 1;
    }
    write_cfg(out, &c);
    fclose(out);

    printf("merged %d file(s) into %s\n", argc - 1, argv[0]);
    cfg_free(&c);
    return 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    apex_die_hook = catch_die;

    if (argc < 2) {
        fputs("usage: apexini <check|overlaps|merge> ...\n", stderr);
        return 2;
    }
    if (strcmp(argv[1], "check") == 0)
        return cmd_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "overlaps") == 0)
        return cmd_overlaps(argc - 2, argv + 2);
    if (strcmp(argv[1], "merge") == 0)
        return cmd_merge(argc - 2, argv + 2);

    fprintf(stderr, "apexini: unknown command '%s'\n", argv[1]);
    fputs("usage: apexini <check|overlaps|merge> ...\n", stderr);
    return 2;
}
