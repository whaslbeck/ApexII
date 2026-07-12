/* apexini — INI config utilities for ApexII projects.
 *
 * Run `apexini help` for the full list of subcommands and their arguments
 * (see print_help() near main()).
 */
#include "apex.h"
#include "apex_config.h"
#include "apex_project.h"
#include "apex_render.h"
#include "apex_nvram.h"

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
    ConfigDocs       docs;
    ConfigSymbols    syms;
    DataRanges       data;
    ConfigOptions    opts;
    ConfigTypes      types;
    ConfigEntries    ref_exclusions;
    ConfigEntries    literals;
    ConfigEntries    ack_warnings;
    ConfigEntries    far_imm;
} Cfg;

static int cfg_load(Cfg *c, const char *path)
{
    int failed;

    s_catching = 1;
    failed = setjmp(s_jmp);
    if (!failed) {
        load_config(path, &c->sigs, &c->labels, &c->entries, &c->tables,
                    &c->schemas, &c->docs, &c->syms, &c->data,
                    &c->opts, &c->types, &c->ref_exclusions, &c->literals,
                    &c->ack_warnings, &c->far_imm);
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

    for (i = 0; i < c->docs.count; i++)
        free(c->docs.items[i].text);
    free(c->docs.items);

    for (i = 0; i < c->syms.count; i++)
        free((char *)c->syms.items[i].name);
    free(c->syms.items);

    free(c->data.items);
    free(c->ref_exclusions.items);
    free(c->literals.items);
    free(c->ack_warnings.items);
    free(c->far_imm.items);
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

static void w_addr_norm(FILE *f, int has_bank, uint8_t bank, uint32_t addr)
{
    uint8_t b = has_bank ? bank : 0xffu;
    fprintf(f, "B%02x_A%04x", b, (unsigned)addr & 0xffffu);
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
    case TABLE_PTR16_SPRITE:        return "ptr16_sprite";
    case TABLE_FAR_STRING:          return "far_string";
    case TABLE_FAR_DATA:            return "far_data";
    case TABLE_FAR_TABLE:           return "far_table";
    case TABLE_FAR_CODE:            return "far_code";
    case TABLE_FAR_DMD_FULLFRAME:   return "far_dmd_fullframe";
    case TABLE_FAR_SPRITE:          return "far_sprite";
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
        if (!s->items[i].type_name && s->items[i].param &&
            (s->items[i].kind == TABLE_PTR16_SPRITE || s->items[i].kind == TABLE_FAR_SPRITE))
            fprintf(f, "(%u)", s->items[i].param);
        if (s->items[i].count != 1u)
            fprintf(f, "[%lu]", (unsigned long)s->items[i].count);
    }
}

static void w_data_val(FILE *f, const DataRange *r)
{
    char buf[48];
    fputs(data_range_spec(r, buf, sizeof(buf)), f);
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

static int cmp_type_value(const void *a, const void *b)
{
    const ConfigTypeValue *va = (const ConfigTypeValue *)a;
    const ConfigTypeValue *vb = (const ConfigTypeValue *)b;
    if (va->value != vb->value)
        return va->value < vb->value ? -1 : 1;
    return 0;
}

/* ── write a complete config ────────────────────────────────────────────── */

/* addr_fn: either w_addr (preserve format) or w_addr_norm (always Bxx_Ayyyy) */
typedef void (*AddrFn)(FILE *, int, uint8_t, uint32_t);

static void write_cfg_ex(FILE *f, Cfg *c, AddrFn addr_fn)
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
    if (c->docs.count)    qsort(c->docs.items,     c->docs.count,    sizeof(c->docs.items[0]),    cmp_doc);
    if (c->ref_exclusions.count)
        qsort(c->ref_exclusions.items, c->ref_exclusions.count,
              sizeof(c->ref_exclusions.items[0]), cmp_entry);
    if (c->literals.count)
        qsort(c->literals.items, c->literals.count,
              sizeof(c->literals.items[0]), cmp_entry);
    if (c->ack_warnings.count)
        qsort(c->ack_warnings.items, c->ack_warnings.count,
              sizeof(c->ack_warnings.items[0]), cmp_entry);
    if (c->far_imm.count)
        qsort(c->far_imm.items, c->far_imm.count,
              sizeof(c->far_imm.items[0]), cmp_entry);

    if (c->opts.labels_are_entries)
        fputs("[options]\nlabels_are_entries = true\n", f);

    if (c->types.count) {
        fputs("\n[types]\n", f);
        for (i = 0; i < c->types.count; i++) {
            const ConfigType *t = &c->types.items[i];
            if (t->value_count)
                qsort(t->values, t->value_count, sizeof(t->values[0]), cmp_type_value);
            fprintf(f, "%s:%s =\n", t->name, t->kind == TABLE_BYTE ? "byte" : "word");
            for (j = 0; j < t->value_count; j++) {
                fprintf(f, t->kind == TABLE_WORD ? "\t0x%04x:%s\n" : "\t0x%02x:%s\n",
                        t->values[j].value, t->values[j].name);
            }
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
            addr_fn(f, c->labels.items[i].has_bank, c->labels.items[i].bank,
                    c->labels.items[i].addr);
            fputs(" = ", f);
            w_escaped(f, c->labels.items[i].name);
            fputc('\n', f);
        }
    }

    if (c->entries.count) {
        fputs("\n[entries]\n", f);
        for (i = 0; i < c->entries.count; i++) {
            addr_fn(f, c->entries.items[i].has_bank, c->entries.items[i].bank,
                    c->entries.items[i].addr);
            fputs(" = code\n", f);
        }
    }

    if (c->sigs.count) {
        fputs("\n[inline]\n", f);
        for (i = 0; i < c->sigs.count; i++) {
            addr_fn(f, c->sigs.items[i].has_bank, c->sigs.items[i].bank,
                    c->sigs.items[i].addr);
            fputs(" = ", f);
            w_schema(f, &c->sigs.items[i].schema);
            if (c->sigs.items[i].flow_stop) {
                fputs(", flow_stop", f);
            }
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

    if (c->docs.count) {
        fputs("\n[docs]\n", f);
        for (i = 0; i < c->docs.count; i++) {
            addr_fn(f, c->docs.items[i].has_bank, c->docs.items[i].bank,
                    c->docs.items[i].addr);
            fputs(" = ", f);
            w_escaped(f, c->docs.items[i].text);
            fputc('\n', f);
        }
    }

    if (c->ref_exclusions.count) {
        fputs("\n[exclude_refs]\n", f);
        for (i = 0; i < c->ref_exclusions.count; i++) {
            addr_fn(f, c->ref_exclusions.items[i].has_bank,
                    c->ref_exclusions.items[i].bank,
                    c->ref_exclusions.items[i].addr);
            fputs(" = exclude\n", f);
        }
    }

    if (c->literals.count) {
        fputs("\n[literals]\n", f);
        for (i = 0; i < c->literals.count; i++) {
            addr_fn(f, c->literals.items[i].has_bank,
                    c->literals.items[i].bank,
                    c->literals.items[i].addr);
            fputs(" = literal\n", f);
        }
    }

    if (c->ack_warnings.count) {
        fputs("\n[ack_warnings]\n", f);
        for (i = 0; i < c->ack_warnings.count; i++) {
            addr_fn(f, c->ack_warnings.items[i].has_bank,
                    c->ack_warnings.items[i].bank,
                    c->ack_warnings.items[i].addr);
            fputs(" = ack\n", f);
        }
    }

    if (c->far_imm.count) {
        fputs("\n[far_imm]\n", f);
        for (i = 0; i < c->far_imm.count; i++) {
            const ConfigEntry *e = &c->far_imm.items[i];
            addr_fn(f, e->has_bank, e->bank, e->addr);
            fprintf(f, " = %s 0x%02x", far_imm_type_name((FarImmType)e->value2), e->value);
            if (e->aux_addr) {
                fprintf(f, " B%02x_A%04x", (unsigned)(e->has_bank ? e->bank : 0xffu),
                        (unsigned)e->aux_addr);
            }
            fputc('\n', f);
        }
    }
}

static void write_cfg(FILE *f, Cfg *c)
{
    write_cfg_ex(f, c, w_addr);
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
            printf("%s: OK  labels=%zu  entries=%zu  inline=%zu  data=%zu  tables=%zu  types=%zu  exclude_refs=%zu  literals=%zu  ack_warnings=%zu  far_imm=%zu\n",
                   argv[i], c.labels.count, c.entries.count, c.sigs.count,
                   c.data.count, c.tables.count, c.types.count, c.ref_exclusions.count,
                   c.literals.count, c.ack_warnings.count, c.far_imm.count);
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

/* Statically-known byte length of a data range, or 0 when the end can only be
   determined by reading the ROM (null-terminated string, sprite). */
static size_t data_range_len(const DataRange *r)
{
    switch (r->kind) {
    case DATA_BYTES:
    case DATA_STRING_FIXED:
    case DATA_BCD:               return r->length;
    case DATA_DMD_FULLFRAME:     return 512u;
    case DATA_PTR16_STRING:
    case DATA_PTR16_DATA:
    case DATA_PTR16_CODE:
    case DATA_PTR16_TABLE:
    case DATA_PTR16_SPRITE:      return 2u;
    case DATA_FAR_STRING:
    case DATA_FAR_DATA:
    case DATA_FAR_TABLE:
    case DATA_FAR_CODE:
    case DATA_FAR_DMD_FULLFRAME:
    case DATA_FAR_SPRITE:        return 3u;
    default:                     return 0u; /* string, sprite, sprite_noheader */
    }
}

static uint32_t data_end(const DataRange *r)
{
    size_t len = data_range_len(r);
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
        data_range_spec(&c.data.items[i], e->spec, sizeof(e->spec));
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
        j = i + 1;
        while (j < count &&
               addr_cmp(ovl[i].has_bank, ovl[i].bank, ovl[i].addr,
                        ovl[j].has_bank, ovl[j].bank, ovl[j].addr) == 0)
            j++;
        if (j > i + 1) {
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

    /* pass 2: range overlaps */
    for (i = 0; i < count; i++) {
        uint8_t ba;

        if (ovl[i].end == 0) continue;
        ba = ovl[i].has_bank ? ovl[i].bank : 0xffu;

        for (j = i + 1; j < count; j++) {
            uint8_t bb = ovl[j].has_bank ? ovl[j].bank : 0xffu;

            if (bb != ba) break;
            if (ovl[j].addr >= ovl[i].end) break;
            if (ovl[j].addr == ovl[i].addr) continue;

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

/* ── normalize command ──────────────────────────────────────────────────── */

static int cmd_normalize(int argc, char **argv)
{
    Cfg c;
    FILE *out;
    const char *outpath;
    size_t i;

    memset(&c, 0, sizeof(c));
    if (argc < 1) {
        fputs("usage: apexini normalize <file.ini> [<out.ini>]\n", stderr);
        return 2;
    }
    if (cfg_load(&c, argv[0])) {
        fprintf(stderr, "%s: error: %s\n", argv[0], s_err);
        return 1;
    }

    /* Upgrade has_bank=0 addresses (system bank) to explicit Bff form. */
    for (i = 0; i < c.labels.count; i++)
        if (!c.labels.items[i].has_bank) { c.labels.items[i].has_bank = 1; c.labels.items[i].bank = 0xff; }
    for (i = 0; i < c.entries.count; i++)
        if (!c.entries.items[i].has_bank) { c.entries.items[i].has_bank = 1; c.entries.items[i].bank = 0xff; }
    for (i = 0; i < c.sigs.count; i++)
        if (!c.sigs.items[i].has_bank) { c.sigs.items[i].has_bank = 1; c.sigs.items[i].bank = 0xff; }
    for (i = 0; i < c.docs.count; i++)
        if (!c.docs.items[i].has_bank) { c.docs.items[i].has_bank = 1; c.docs.items[i].bank = 0xff; }
    for (i = 0; i < c.ref_exclusions.count; i++)
        if (!c.ref_exclusions.items[i].has_bank) { c.ref_exclusions.items[i].has_bank = 1; c.ref_exclusions.items[i].bank = 0xff; }
    for (i = 0; i < c.literals.count; i++)
        if (!c.literals.items[i].has_bank) { c.literals.items[i].has_bank = 1; c.literals.items[i].bank = 0xff; }
    for (i = 0; i < c.ack_warnings.count; i++)
        if (!c.ack_warnings.items[i].has_bank) { c.ack_warnings.items[i].has_bank = 1; c.ack_warnings.items[i].bank = 0xff; }
    for (i = 0; i < c.far_imm.count; i++)
        if (!c.far_imm.items[i].has_bank) { c.far_imm.items[i].has_bank = 1; c.far_imm.items[i].bank = 0xff; }

    outpath = (argc >= 2) ? argv[1] : argv[0];
    out = fopen(outpath, "w");
    if (!out) {
        perror(outpath);
        cfg_free(&c);
        return 1;
    }
    write_cfg_ex(out, &c, w_addr_norm);
    fclose(out);

    printf("normalized → %s\n", outpath);
    cfg_free(&c);
    return 0;
}

/* ── redundant-entry helpers ────────────────────────────────────────────── */

/*
 * An entry at (bank, addr) is redundant when code already flows naturally into
 * that address from preceding instructions, making the explicit [entries] line
 * unnecessary.  We detect this by scanning backward in the rendered document
 * from the entry's address:
 *
 *   - If the first substantial line is a CODE instruction → code flows in
 *     → redundant.
 *   - If the first substantial line is a transition comment → block boundary
 *     → the entry is genuinely needed.
 *   - If we reach the start of the document → needed.
 */
/* RTS, RTI, JMP, BRA, LBRA, SWI*, CWAI, SYNC do not fall through. */
static int is_terminal_instruction(const ApexRenderedLine *l)
{
    static const struct { const char *s; size_t n; } terms[] = {
        {"LBRA", 4}, {"CWAI", 4}, {"SYNC", 4},
        {"SWI2", 4}, {"SWI3", 4},
        {"RTS",  3}, {"RTI",  3}, {"JMP",  3}, {"BRA",  3}, {"SWI",  3},
    };
    const char *t = l->text;
    size_t len = l->length;
    size_t i;
    while (len > 0 && (*t == ' ' || *t == '\t')) { t++; len--; }
    for (i = 0; i < sizeof(terms)/sizeof(terms[0]); i++) {
        size_t n = terms[i].n;
        if (len >= n && strncmp(t, terms[i].s, n) == 0 &&
            (len == n || t[n] == ' ' || t[n] == '\t'))
            return 1;
    }
    return 0;
}

static int is_entry_redundant(const ApexRenderedDocument *doc,
                              uint8_t bank, uint32_t addr)
{
    size_t li, i;

    if (!apex_render_find_line_by_address(doc, bank, addr, &li))
        return 0; /* not in document at all → not redundant */

    i = li;
    while (i > 0) {
        const ApexRenderedLine *l;
        i--;
        l = &doc->lines[i];

        if (l->kind == APEX_RENDER_LINE_BLANK)
            continue;

        /* Skip label lines stacked at the same address */
        if (l->kind == APEX_RENDER_LINE_LABEL &&
            l->has_location && l->bank == bank && l->cpu_addr == addr)
            continue;

        /* Skip plain (non-transition) comment lines */
        if (l->kind == APEX_RENDER_LINE_COMMENT &&
            l->transition_kind == APEX_RENDER_TRANSITION_NONE)
            continue;

        /* Any block-boundary transition → entry is needed */
        if (l->transition_kind != APEX_RENDER_TRANSITION_NONE)
            return 0;

        /* A non-terminal instruction flows through → entry is redundant.
           Terminal instructions (RTS, JMP, BRA, etc.) end the block without
           falling through, so an adjacent code entry is genuinely needed. */
        if (l->kind == APEX_RENDER_LINE_INSTRUCTION)
            return !is_terminal_instruction(l);

        /* Location header or other structural line → be conservative */
        return 0;
    }
    return 0; /* start of document → needed */
}

/* Open a project (ROM + config), analyze, render.  Returns NULL on failure. */
static const ApexRenderedDocument *open_and_render(const char *rom, const char *cfg_path,
                                                   ApexProject **out_proj)
{
    ApexProject *p = apex_project_open(rom, cfg_path);
    if (!p) {
        fprintf(stderr, "error: cannot open ROM '%s' with config '%s'\n", rom, cfg_path);
        return NULL;
    }
    if (apex_project_analyze(p) != 0) {
        fprintf(stderr, "error: analysis failed\n");
        apex_project_free(p);
        return NULL;
    }
    const ApexRenderedDocument *doc = apex_project_render(p, 0, 0);
    if (!doc) {
        fprintf(stderr, "error: render failed\n");
        apex_project_free(p);
        return NULL;
    }
    *out_proj = p;
    return doc;
}

/* ── find-redundant command ─────────────────────────────────────────────── */

static int cmd_find_redundant(int argc, char **argv)
{
    ApexProject *p;
    const ApexRenderedDocument *doc;
    size_t i, found = 0;

    if (argc < 2) {
        fputs("usage: apexini find-redundant <rom> <file.ini>\n", stderr);
        return 2;
    }

    doc = open_and_render(argv[0], argv[1], &p);
    if (!doc) return 1;

    for (i = 0; i < p->config_entries.count; i++) {
        const ConfigEntry *e = &p->config_entries.items[i];
        uint8_t bank = e->has_bank ? e->bank : 0xffu;
        if (is_entry_redundant(doc, bank, e->addr)) {
            char buf[20];
            fmt_addr(buf, sizeof(buf), e->has_bank, e->bank, e->addr);
            printf("redundant: %s\n", buf);
            found++;
        }
    }

    if (found == 0)
        printf("%s: no redundant entries found\n", argv[1]);
    else
        printf("%zu redundant entry/entries found\n", found);

    apex_project_free(p);
    return found > 0 ? 1 : 0;
}

/* ── strip-redundant command ────────────────────────────────────────────── */

static int cmd_strip_redundant(int argc, char **argv)
{
    ApexProject *p;
    const ApexRenderedDocument *doc;
    size_t i, removed = 0;
    Cfg c;
    FILE *out;

    if (argc < 2) {
        fputs("usage: apexini strip-redundant <rom> <file.ini>\n", stderr);
        return 2;
    }

    doc = open_and_render(argv[0], argv[1], &p);
    if (!doc) return 1;

    /* Collect redundant (has_bank, bank, addr) tuples from the project.
       Then load the config into a Cfg, filter, and write back. */
    memset(&c, 0, sizeof(c));
    if (cfg_load(&c, argv[1])) {
        fprintf(stderr, "%s: error: %s\n", argv[1], s_err);
        apex_project_free(p);
        return 1;
    }

    {
        size_t new_count = 0;
        for (i = 0; i < c.entries.count; i++) {
            const ConfigEntry *e = &c.entries.items[i];
            uint8_t bank = e->has_bank ? e->bank : 0xffu;
            /* Check against the rendered document from the project */
            if (is_entry_redundant(doc, bank, e->addr)) {
                char buf[20];
                fmt_addr(buf, sizeof(buf), e->has_bank, e->bank, e->addr);
                printf("removing: %s\n", buf);
                removed++;
            } else {
                c.entries.items[new_count++] = *e;
            }
        }
        c.entries.count = new_count;
    }

    apex_project_free(p);

    if (removed == 0) {
        printf("%s: no redundant entries found, file unchanged\n", argv[1]);
        cfg_free(&c);
        return 0;
    }

    out = fopen(argv[1], "w");
    if (!out) {
        perror(argv[1]);
        cfg_free(&c);
        return 1;
    }
    write_cfg(out, &c);
    fclose(out);

    printf("removed %zu redundant entry/entries from %s\n", removed, argv[1]);
    cfg_free(&c);
    return 0;
}

/* ── coverage command ───────────────────────────────────────────────────── */

static int cmd_coverage(int argc, char **argv)
{
    ApexProject *p;
    const ApexRenderedDocument *doc;
    uint8_t *kinds;
    size_t rom_size, i;
    /* per-kind totals */
    size_t tot[6] = {0, 0, 0, 0, 0, 0}; /* UNKNOWN, CODE, DATA, TABLE, UNCL, FREE */

    if (argc < 2) {
        fputs("usage: apexini coverage <rom> <file.ini>\n", stderr);
        return 2;
    }

    doc = open_and_render(argv[0], argv[1], &p);
    if (!doc) return 1;

    rom_size = p->rom.size;
    kinds = (uint8_t *)calloc(rom_size, 1);
    if (!kinds) {
        fputs("out of memory\n", stderr);
        apex_project_free(p);
        return 1;
    }

    /* Forward pass: assign block_kind to every ROM byte */
    {
        uint8_t cur = (uint8_t)APEX_RENDER_BLOCK_UNKNOWN;
        size_t fill = 0;
        for (i = 0; i < doc->line_count && fill < rom_size; i++) {
            const ApexRenderedLine *l = &doc->lines[i];
            if (!l->has_location) continue;
            if (l->rom_addr <= fill) {
                cur = (uint8_t)l->block_kind;
            } else {
                size_t end = l->rom_addr < rom_size ? l->rom_addr : rom_size;
                while (fill < end) kinds[fill++] = cur;
                cur = (uint8_t)l->block_kind;
            }
        }
        while (fill < rom_size) kinds[fill++] = cur;
    }

    printf("%-6s  %6s  %12s  %12s  %12s  %14s  %10s  %10s\n",
           "bank", "bytes", "code", "data", "table", "unclassified", "free", "unknown");

    /* Paged banks */
    for (i = 0; i < p->banks; i++) {
        size_t base = i * APEX_BANK_SIZE;
        size_t end  = base + APEX_BANK_SIZE;
        uint8_t bank_id = p->rom.data[base];
        size_t cnt[6] = {0, 0, 0, 0, 0, 0};
        size_t j;

        for (j = base; j < end && j < rom_size; j++) {
            int k = kinds[j];
            if (k >= 0 && k < 6) cnt[k]++;
        }

        size_t total = end <= rom_size ? APEX_BANK_SIZE : rom_size - base;
        printf("0x%02x    %6zu  %5zu(%3.0f%%)  %5zu(%3.0f%%)  %5zu(%3.0f%%)  %6zu(%3.0f%%)  %5zu(%3.0f%%)  %5zu(%3.0f%%)\n",
               bank_id, total,
               cnt[1], total ? cnt[1]*100.0/total : 0.0,
               cnt[2], total ? cnt[2]*100.0/total : 0.0,
               cnt[3], total ? cnt[3]*100.0/total : 0.0,
               cnt[4], total ? cnt[4]*100.0/total : 0.0,
               cnt[5], total ? cnt[5]*100.0/total : 0.0,
               cnt[0], total ? cnt[0]*100.0/total : 0.0);

        for (j = 0; j < 6; j++) tot[j] += cnt[j];
    }

    /* System bank */
    {
        size_t sys_start = rom_size > APEX_SYSTEM_SIZE ? rom_size - APEX_SYSTEM_SIZE : 0;
        size_t cnt[6] = {0, 0, 0, 0, 0, 0};
        size_t j;

        for (j = sys_start; j < rom_size; j++) {
            int k = kinds[j];
            if (k >= 0 && k < 6) cnt[k]++;
        }

        size_t total = rom_size - sys_start;
        printf("0xff    %6zu  %5zu(%3.0f%%)  %5zu(%3.0f%%)  %5zu(%3.0f%%)  %6zu(%3.0f%%)  %5zu(%3.0f%%)  %5zu(%3.0f%%)\n",
               total,
               cnt[1], total ? cnt[1]*100.0/total : 0.0,
               cnt[2], total ? cnt[2]*100.0/total : 0.0,
               cnt[3], total ? cnt[3]*100.0/total : 0.0,
               cnt[4], total ? cnt[4]*100.0/total : 0.0,
               cnt[5], total ? cnt[5]*100.0/total : 0.0,
               cnt[0], total ? cnt[0]*100.0/total : 0.0);

        for (j = 0; j < 6; j++) tot[j] += cnt[j];
    }

    /* Total */
    printf("%-6s  %6zu  %5zu(%3.0f%%)  %5zu(%3.0f%%)  %5zu(%3.0f%%)  %6zu(%3.0f%%)  %5zu(%3.0f%%)  %5zu(%3.0f%%)\n",
           "total", rom_size,
           tot[1], rom_size ? tot[1]*100.0/rom_size : 0.0,
           tot[2], rom_size ? tot[2]*100.0/rom_size : 0.0,
           tot[3], rom_size ? tot[3]*100.0/rom_size : 0.0,
           tot[4], rom_size ? tot[4]*100.0/rom_size : 0.0,
           tot[5], rom_size ? tot[5]*100.0/rom_size : 0.0,
           tot[0], rom_size ? tot[0]*100.0/rom_size : 0.0);

    free(kinds);
    apex_project_free(p);
    return 0;
}

/* ── orphan-labels command ──────────────────────────────────────────────── */

static int cmd_orphan_labels(int argc, char **argv)
{
    ApexProject *p;
    size_t i, j, found = 0;

    if (argc < 2) {
        fputs("usage: apexini orphan-labels <rom> <file.ini>\n", stderr);
        return 2;
    }

    p = apex_project_open(argv[0], argv[1]);
    if (!p) {
        fprintf(stderr, "error: cannot open ROM '%s' with config '%s'\n", argv[0], argv[1]);
        return 1;
    }
    if (apex_project_analyze(p) != 0) {
        fprintf(stderr, "error: analysis failed\n");
        apex_project_free(p);
        return 1;
    }

    for (i = 0; i < p->config_labels.count; i++) {
        const ConfigLabel *lbl = &p->config_labels.items[i];
        uint8_t bank = lbl->has_bank ? lbl->bank : 0xffu;
        int has_ref = 0;
        int is_entry = 0;

        /* Skip if also an explicit [entries] point — those are reachable by design */
        for (j = 0; j < p->config_entries.count; j++) {
            uint8_t eb = p->config_entries.items[j].has_bank
                         ? p->config_entries.items[j].bank : 0xffu;
            if (eb == bank && p->config_entries.items[j].addr == lbl->addr) {
                is_entry = 1;
                break;
            }
        }
        if (is_entry) continue;

        /* Check for any inbound ROM reference */
        for (j = 0; j < p->refs.count; j++) {
            if (p->refs.items[j].bank == bank && p->refs.items[j].addr == lbl->addr) {
                has_ref = 1;
                break;
            }
        }

        if (!has_ref) {
            char buf[20];
            fmt_addr(buf, sizeof(buf), lbl->has_bank, lbl->bank, lbl->addr);
            printf("orphan: %-20s  %s\n", buf, lbl->name ? lbl->name : "");
            found++;
        }
    }

    if (found == 0)
        printf("%s: no orphan labels found\n", argv[1]);
    else
        printf("%zu orphan label(s) found\n", found);

    apex_project_free(p);
    return found > 0 ? 1 : 0;
}

/* ── check-bounds command ───────────────────────────────────────────────── */

/* Returns 1 if (has_bank, bank, addr) is a valid ROM address, 0 and prints error if not. */
static int check_one_addr(const ApexProject *p, int has_bank, uint8_t bank, uint32_t addr,
                          const char *section, int *issues)
{
    char addrstr[20];
    fmt_addr(addrstr, sizeof(addrstr), has_bank, bank, addr);

    if (!has_bank) {
        /* No bank specifier: valid in paged range 0x4000-0x7fff (all banks) or
           system range 0x8000-0xffff.  RAM addresses (< 0x4000) are suspicious. */
        if (addr < APEX_PAGED_ORG || addr > 0xffffu) {
            printf("invalid: [%s] %s  address without bank out of CPU range (expected 0x4000-0xffff)\n",
                   section, addrstr);
            (*issues)++;
            return 0;
        }
    } else if (bank == 0xffu) {
        /* Explicit system bank: CPU 0x8000-0xffff */
        if (addr < APEX_SYSTEM_ORG || addr > 0xffffu) {
            printf("invalid: [%s] %s  system bank address out of range (expected 0x8000-0xffff)\n",
                   section, addrstr);
            (*issues)++;
            return 0;
        }
        if (p->rom.size < APEX_SYSTEM_SIZE) {
            printf("invalid: [%s] %s  ROM too small for a system bank\n", section, addrstr);
            (*issues)++;
            return 0;
        }
    } else {
        /* Paged bank: CPU 0x4000-0x7fff */
        if (addr < APEX_PAGED_ORG || addr >= APEX_PAGED_ORG + APEX_BANK_SIZE) {
            printf("invalid: [%s] %s  paged bank address out of range (expected 0x4000-0x7fff)\n",
                   section, addrstr);
            (*issues)++;
            return 0;
        }
        if (bank_index_for_id(p->rom.data, p->banks, bank) < 0) {
            printf("invalid: [%s] %s  bank 0x%02x not found in ROM\n",
                   section, addrstr, bank);
            (*issues)++;
            return 0;
        }
    }
    return 1;
}

/* Check that addr+length-1 stays within the same bank page. */
static void check_range_end(int has_bank, uint8_t bank, uint32_t addr,
                            size_t length, const char *section, int *issues)
{
    if (length == 0) return;
    uint32_t last = addr + (uint32_t)length - 1u;
    uint32_t page_end = (!has_bank || bank == 0xffu) ? 0xffffu
                        : (uint32_t)(APEX_PAGED_ORG + APEX_BANK_SIZE - 1u);
    if (last > page_end) {
        char addrstr[20];
        fmt_addr(addrstr, sizeof(addrstr), has_bank, bank, addr);
        printf("invalid: [%s] %s  range of %zu bytes overflows bank (ends at 0x%04x)\n",
               section, addrstr, length, (unsigned)last);
        (*issues)++;
    }
}

static int cmd_check_bounds(int argc, char **argv)
{
    ApexProject *p;
    int issues = 0;
    size_t i;

    if (argc < 2) {
        fputs("usage: apexini check-bounds <rom> <file.ini>\n", stderr);
        return 2;
    }

    p = apex_project_open(argv[0], argv[1]);
    if (!p) {
        fprintf(stderr, "error: cannot open ROM '%s' with config '%s'\n", argv[0], argv[1]);
        return 1;
    }

    /* Labels */
    for (i = 0; i < p->config_labels.count; i++) {
        const ConfigLabel *e = &p->config_labels.items[i];
        check_one_addr(p, e->has_bank, e->bank, e->addr, "labels", &issues);
    }

    /* Entries */
    for (i = 0; i < p->config_entries.count; i++) {
        const ConfigEntry *e = &p->config_entries.items[i];
        check_one_addr(p, e->has_bank, e->bank, e->addr, "entries", &issues);
    }

    /* Inline signatures */
    for (i = 0; i < p->inline_sigs.count; i++) {
        const InlineSignature *s = &p->inline_sigs.items[i];
        if (check_one_addr(p, s->has_bank, s->bank, s->addr, "inline", &issues))
            check_range_end(s->has_bank, s->bank, s->addr, s->length, "inline", &issues);
    }

    /* Data ranges */
    for (i = 0; i < p->data_ranges.count; i++) {
        const DataRange *d = &p->data_ranges.items[i];
        if (check_one_addr(p, 1, d->bank, d->addr, "data", &issues)) {
            size_t len = data_range_len(d);
            if (len) check_range_end(1, d->bank, d->addr, len, "data", &issues);
        }
    }

    /* Tables */
    for (i = 0; i < p->tables.count; i++) {
        const TableDef *t = &p->tables.items[i];
        if (check_one_addr(p, 1, t->bank, t->addr, "tables", &issues)) {
            if (!t->has_header && t->rows > 0) {
                size_t w = table_schema_width(&t->schema);
                if (w) check_range_end(1, t->bank, t->addr, t->rows * w, "tables", &issues);
            }
        }
    }

    /* Ref exclusions */
    for (i = 0; i < p->ref_exclusions.count; i++) {
        const ConfigEntry *e = &p->ref_exclusions.items[i];
        check_one_addr(p, e->has_bank, e->bank, e->addr, "exclude_refs", &issues);
    }

    /* Literals */
    for (i = 0; i < p->literals.count; i++) {
        const ConfigEntry *e = &p->literals.items[i];
        check_one_addr(p, e->has_bank, e->bank, e->addr, "literals", &issues);
    }

    /* Acked warnings */
    for (i = 0; i < p->ack_warnings.count; i++) {
        const ConfigEntry *e = &p->ack_warnings.items[i];
        check_one_addr(p, e->has_bank, e->bank, e->addr, "ack_warnings", &issues);
    }

    /* Far immediates */
    for (i = 0; i < p->far_imms.count; i++) {
        const ConfigEntry *e = &p->far_imms.items[i];
        check_one_addr(p, e->has_bank, e->bank, e->addr, "far_imm", &issues);
    }

    if (issues == 0)
        printf("%s + %s: all addresses valid\n", argv[0], argv[1]);
    else
        printf("%d invalid address(es) found\n", issues);

    apex_project_free(p);
    return issues > 0 ? 1 : 0;
}

/* ── migrate ─────────────────────────────────────────────────────────────── */
/*
 * apexini migrate <file.ini> [<file.ini> ...]
 *
 * Rewrites each file in-place, replacing [routine_docs] and [table_docs]
 * sections with a single [docs] section.  The rest of the file is unchanged.
 * A .bak backup is written before overwriting.
 */
static int cmd_migrate(int argc, char **argv)
{
    int i;
    int total_errors = 0;

    if (argc < 1) {
        fputs("usage: apexini migrate <file.ini> ...\n", stderr);
        return 2;
    }

    for (i = 0; i < argc; i++) {
        const char *path = argv[i];
        Cfg c;
        memset(&c, 0, sizeof(c));
        FILE *f;
        char bak[1040];

        if (cfg_load(&c, path)) {
            fprintf(stderr, "%s: error: %s\n", path, s_err);
            total_errors++;
            cfg_free(&c);
            continue;
        }

        /* Write backup */
        snprintf(bak, sizeof(bak), "%s.bak", path);
        {
            FILE *src = fopen(path, "rb");
            FILE *dst = src ? fopen(bak, "wb") : NULL;
            if (src && dst) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
                    fwrite(buf, 1, n, dst);
            }
            if (src) fclose(src);
            if (dst) fclose(dst);
            if (!src || !dst) {
                fprintf(stderr, "%s: warning: could not write backup to %s\n", path, bak);
            }
        }

        /* Re-emit with [docs] */
        f = fopen(path, "w");
        if (!f) {
            perror(path);
            cfg_free(&c);
            total_errors++;
            continue;
        }
        write_cfg_ex(f, &c, w_addr_norm);
        fclose(f);

        {
            size_t ndocs = c.docs.count;
            cfg_free(&c);
            if (ndocs > 0)
                printf("migrated %s  (%zu doc entries → [docs])\n", path, ndocs);
            else
                printf("migrated %s  (no doc entries)\n", path);
        }
    }
    return total_errors > 0 ? 1 : 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

static const char *doc_text_at(const Cfg *c, uint32_t addr)
{
    size_t i;
    for (i = 0; i < c->docs.count; i++) {
        if (c->docs.items[i].addr == addr) {
            return c->docs.items[i].text;
        }
    }
    return NULL;
}

static int symbol_at(const Cfg *c, uint32_t addr)
{
    size_t i;
    for (i = 0; i < c->syms.count; i++) {
        if (c->syms.items[i].value == addr) {
            return 1;
        }
    }
    return 0;
}

/* apexini nvram-export <in.ini> <out.json> — write RAM symbols/docs as an
   NVRAM-maps JSON file. */
/* Grow the RAM-location array if full (checked realloc; exits on OOM, matching
   ovl_push).  Returns the possibly-relocated array. */
static ApexNvramLoc *nvram_loc_reserve(ApexNvramLoc *arr, size_t n, size_t *cap)
{
    if (n == *cap) {
        size_t nc = *cap ? *cap * 2 : 32;
        ApexNvramLoc *na = realloc(arr, nc * sizeof(*na));
        if (!na) { fputs("out of memory\n", stderr); exit(1); }
        arr = na;
        *cap = nc;
    }
    return arr;
}

static int cmd_nvram_export(int argc, char **argv)
{
    Cfg c;
    ApexNvramLoc *arr = NULL;
    size_t n = 0, cap = 0, i;
    FILE *f;

    if (argc < 2) {
        fprintf(stderr, "usage: apexini nvram-export <in.ini> <out.json> [template.json]\n");
        return 2;
    }
    memset(&c, 0, sizeof(c));
    if (cfg_load(&c, argv[0])) {
        fprintf(stderr, "%s: error: %s\n", argv[0], s_err);
        return 1;
    }
    /* named RAM locations (symbols in the RAM range) + their docs */
    for (i = 0; i < c.syms.count; i++) {
        if (c.syms.items[i].value >= APEX_RAM_LIMIT) continue;
        arr = nvram_loc_reserve(arr, n, &cap);
        arr[n].name = (char *)c.syms.items[i].name;
        arr[n].addr = c.syms.items[i].value;
        arr[n].doc  = (char *)doc_text_at(&c, c.syms.items[i].value);
        n++;
    }
    /* documented RAM locations without a symbol name → generated name */
    for (i = 0; i < c.docs.count; i++) {
        uint32_t a = c.docs.items[i].addr;
        if (a >= APEX_RAM_LIMIT || symbol_at(&c, a)) continue;
        arr = nvram_loc_reserve(arr, n, &cap);
        {
            char *nm = malloc(16);
            snprintf(nm, 16, "RAM_%04x", a & 0xffffu);
            arr[n].name = nm; /* leaked at process exit; fine for a CLI one-shot */
        }
        arr[n].addr = a;
        arr[n].doc  = c.docs.items[i].text;
        n++;
    }
    /* Read the template fully into memory BEFORE opening the output for write,
       so `nvram-export game.ini game.json game.json` (in-place update) does not
       truncate the template before we read it. */
    char  *tt = NULL;
    size_t tgot = 0;
    if (argc >= 3) {
        FILE *tf = fopen(argv[2], "rb");
        if (!tf) { fprintf(stderr, "cannot read template %s\n", argv[2]); free(arr); cfg_free(&c); return 1; }
        fseek(tf, 0, SEEK_END); long tsz = ftell(tf); fseek(tf, 0, SEEK_SET);
        tt = malloc((size_t)(tsz < 0 ? 0 : tsz) + 1u);
        if (!tt) { fprintf(stderr, "out of memory\n"); fclose(tf); free(arr); cfg_free(&c); return 1; }
        tgot = tsz > 0 ? fread(tt, 1, (size_t)tsz, tf) : 0;
        fclose(tf);
        tt[tgot] = '\0';
    }
    f = fopen(argv[1], "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[1]); free(tt); free(arr); cfg_free(&c); return 1; }
    if (tt) {
        char terr[128] = {0};
        int rc = apex_nvram_export_merged(f, tt, tgot, arr, n, terr, sizeof(terr));
        free(tt);
        if (rc != 0) { fprintf(stderr, "%s: %s\n", argv[2], terr); fclose(f); free(arr); cfg_free(&c); return 1; }
        printf("exported %zu RAM location(s) into %s (template %s)\n", n, argv[1], argv[2]);
    } else {
        apex_nvram_write_json(f, arr, n, NULL);
        printf("exported %zu RAM location(s) to %s\n", n, argv[1]);
    }
    fclose(f);
    free(arr);
    cfg_free(&c);
    return 0;
}

static void write_ini_doc_value(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        char c = *s;
        if (c == '"' || c == '\\' || c == ';' || c == '#') fputc('\\', f);
        if (c == '\n') { fputs("\\n", f); continue; }
        fputc(c, f);
    }
    fputc('"', f);
}

/* apexini nvram-import <in.json> <out.ini> [base] — read an NVRAM-maps JSON
   file and write a config with [symbols] and [docs] for each RAM location. */
static int cmd_nvram_import(int argc, char **argv)
{
    ApexNvramLocs locs;
    char err[128] = {0};
    char *text = NULL;
    long sz;
    uint32_t base = 0;
    FILE *in, *out;
    size_t i, got;

    if (argc < 2) {
        fprintf(stderr, "usage: apexini nvram-import <in.json> <out.ini> [base_addr]\n");
        return 2;
    }
    if (argc >= 3) {
        base = (uint32_t)strtoul(argv[2], NULL, 0);
    }
    in = fopen(argv[0], "rb");
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[0]); return 1; }
    fseek(in, 0, SEEK_END);
    sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (sz < 0) { fclose(in); fprintf(stderr, "cannot size %s\n", argv[0]); return 1; }
    text = malloc((size_t)sz + 1u);
    if (!text) { fclose(in); fprintf(stderr, "out of memory\n"); return 1; }
    got = fread(text, 1, (size_t)sz, in);
    fclose(in);
    text[got] = '\0';
    if (apex_nvram_parse_json(text, got, base, &locs, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s: %s\n", argv[0], err);
        free(text);
        return 1;
    }
    free(text);
    out = fopen(argv[1], "w");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[1]); apex_nvram_locs_free(&locs); return 1; }
    fputs("; RAM map imported from an NVRAM-maps JSON file by apexini nvram-import\n\n", out);
    fputs("[symbols]\n", out);
    for (i = 0; i < locs.count; i++) {
        fprintf(out, "%s = 0x%04x\n", locs.items[i].name, locs.items[i].addr & 0xffffu);
    }
    fputs("\n[docs]\n", out);
    for (i = 0; i < locs.count; i++) {
        if (locs.items[i].doc && locs.items[i].doc[0]) {
            fprintf(out, "0x%04x = ", locs.items[i].addr & 0xffffu);
            write_ini_doc_value(out, locs.items[i].doc);
            fputc('\n', out);
        }
    }
    fclose(out);
    printf("imported %zu RAM location(s) to %s\n", locs.count, argv[1]);
    apex_nvram_locs_free(&locs);
    return 0;
}

static void print_help(FILE *out)
{
    fputs(
"apexini — INI config utilities for ApexII projects\n"
"\n"
"Usage: apexini <command> [args]\n"
"\n"
"Commands:\n"
"  check <file.ini>...              Syntax-check configs; non-zero exit on any error.\n"
"  overlaps <file.ini>             Report address conflicts and overlapping data/table ranges.\n"
"  merge <out.ini> <in.ini>...     Merge configs into one sorted file; reports conflicts.\n"
"  migrate <file.ini>...           Upgrade old syntax to the current format in place\n"
"                                  (writes a .bak backup first).\n"
"  normalize <file.ini> [out.ini]  Rewrite canonically: fixed section order, sorted entries,\n"
"                                  sorted enum values, Bxx_Ayyyy addresses. In place if\n"
"                                  out.ini is omitted.\n"
"  find-redundant <rom> <file.ini> List [entries] already reached by code flow (removable).\n"
"  strip-redundant <rom> <file.ini> Remove those redundant [entries] in place.\n"
"  coverage <rom> <file.ini>       Report ROM classification coverage (code/data/unknown/...).\n"
"  orphan-labels <rom> <file.ini>  List [labels] whose address is absent from the disassembly.\n"
"  check-bounds <rom> <file.ini>   Verify data/table ranges stay within bank/ROM bounds.\n"
"  nvram-export <in.ini> <out.json> [template.json]  Export RAM [symbols]/[docs] as PinMAME\n"
"                                  nvram-maps JSON; with a template, only names/docs are\n"
"                                  updated and every other field is preserved (zero loss).\n"
"  nvram-import <in.json> <out.ini> [base]  Import an nvram-maps JSON into [symbols]/[docs].\n"
"  help                            Show this help.\n"
"\n"
"Addresses: Bxx_Ayyyy (xx = bank in hex, yyyy = CPU address) or 0xyyyy for the system bank.\n",
        out);
}

int main(int argc, char **argv)
{
    apex_die_hook = catch_die;

    if (argc < 2) {
        print_help(stderr);
        return 2;
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "--help") == 0) {
        print_help(stdout);
        return 0;
    }
    if (strcmp(argv[1], "check") == 0)
        return cmd_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "overlaps") == 0)
        return cmd_overlaps(argc - 2, argv + 2);
    if (strcmp(argv[1], "merge") == 0)
        return cmd_merge(argc - 2, argv + 2);
    if (strcmp(argv[1], "migrate") == 0)
        return cmd_migrate(argc - 2, argv + 2);
    if (strcmp(argv[1], "normalize") == 0)
        return cmd_normalize(argc - 2, argv + 2);
    if (strcmp(argv[1], "find-redundant") == 0)
        return cmd_find_redundant(argc - 2, argv + 2);
    if (strcmp(argv[1], "strip-redundant") == 0)
        return cmd_strip_redundant(argc - 2, argv + 2);
    if (strcmp(argv[1], "coverage") == 0)
        return cmd_coverage(argc - 2, argv + 2);
    if (strcmp(argv[1], "orphan-labels") == 0)
        return cmd_orphan_labels(argc - 2, argv + 2);
    if (strcmp(argv[1], "check-bounds") == 0)
        return cmd_check_bounds(argc - 2, argv + 2);
    if (strcmp(argv[1], "nvram-export") == 0)
        return cmd_nvram_export(argc - 2, argv + 2);
    if (strcmp(argv[1], "nvram-import") == 0)
        return cmd_nvram_import(argc - 2, argv + 2);

    fprintf(stderr, "apexini: unknown command '%s'\n\n", argv[1]);
    print_help(stderr);
    return 2;
}
