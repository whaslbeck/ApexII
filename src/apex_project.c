#include "apex_project.h"
#include "apex_render.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void build_system_labels(const uint8_t *data, const VectorInfo *vectors, size_t vector_count,
                         InlineSignatures *inline_sigs, const uint8_t *paged_rom,
                         size_t banks, LabelSet *bank_labels, LabelSet *labels,
                         const ConfigLabels *config_labels,
                         const ConfigEntries *config_entries,
                         const DataRanges *data_ranges, const ConfigOptions *options,
                         ReferenceSet *refs);
void collect_bank_code_targets(const uint8_t *paged_rom, size_t banks,
                               const InlineSignatures *inline_sigs,
                               LabelSet *bank_labels, LabelSet *system_labels,
                               const DataRanges *data_ranges, ReferenceSet *refs,
                               const ConfigEntries *ref_exclusions);
void apply_config_bank_labels(const ConfigLabels *config_labels, const uint8_t *paged_rom,
                              size_t banks, LabelSet *bank_labels,
                              const ConfigOptions *options);
void apply_config_bank_entries(const ConfigEntries *config_entries, const uint8_t *paged_rom,
                               size_t banks, LabelSet *bank_labels);
void apply_data_range_labels(const DataRanges *data_ranges, const uint8_t *paged_rom,
                             size_t banks, const uint8_t *system_rom,
                             LabelSet *bank_labels, LabelSet *system_labels,
                             ReferenceSet *refs);
void apply_table_labels(const TableDefs *tables, const uint8_t *paged_rom, size_t banks,
                        LabelSet *bank_labels, LabelSet *system_labels,
                        const uint8_t *system_rom, ReferenceSet *refs);
static void analyze_full_project(ApexProject *project);

/* For every TABLE_FAR_DMD_FULLFRAME / TABLE_PTR16_DMD_FULLFRAME field in all
   configured tables, add a DATA_DMD_FULLFRAME data-range entry for the target
   if none exists yet.  Called after apply_table_labels so the far pointers can
   be read from ROM. */
static void inject_dmd_table_data_ranges(ApexProject *project)
{
    size_t i;

    for (i = 0; i < project->tables.count; i++) {
        const TableDef *table = &project->tables.items[i];
        const uint8_t *bank_data;
        size_t bank_data_len;
        size_t pos;
        uint16_t row_count;
        uint8_t row_width;
        size_t row;

        if (table->bank == 0xffu) {
            if (!in_system_addr(table->addr))
                continue;
            bank_data     = project->rom.data + project->paged_size;
            bank_data_len = project->rom.size  - project->paged_size;
            pos           = (size_t)(table->addr - APEX_SYSTEM_ORG);
        } else {
            int bidx = bank_index_for_id(project->rom.data, project->banks, table->bank);

            if (bidx < 0 || table->addr < APEX_PAGED_ORG || table->addr >= 0x8000u)
                continue;
            bank_data     = project->rom.data + (size_t)bidx * APEX_BANK_SIZE;
            bank_data_len = APEX_BANK_SIZE;
            pos           = (size_t)(table->addr - APEX_PAGED_ORG);
        }

        if (table->has_header) {
            if (pos + 3u > bank_data_len)
                continue;
            row_count = read_be16(bank_data + pos);
            row_width = bank_data[pos + 2u];
            pos += 3u;
        } else {
            row_count = (uint16_t)table->rows;
            row_width = (uint8_t)table_schema_width(&table->schema);
        }

        for (row = 0; row < row_count && pos + row_width <= bank_data_len; row++) {
            size_t fp = pos;
            size_t fi;

            for (fi = 0; fi < table->schema.count; fi++) {
                size_t n;
                TableFieldKind kind = table->schema.items[fi].kind;

                for (n = 0; n < table->schema.items[fi].count; n++) {
                    if (kind == TABLE_FAR_DMD_FULLFRAME && fp + 3u <= bank_data_len) {
                        uint16_t addr    = read_be16(bank_data + fp);
                        uint8_t tgt_bank = bank_data[fp + 2u];

                        if (!data_range_at(tgt_bank, addr, &project->data_ranges))
                            add_data_range(&project->data_ranges, tgt_bank, addr,
                                           DATA_DMD_FULLFRAME, 0);
                        fp += 3u;
                    } else if (kind == TABLE_PTR16_DMD_FULLFRAME && fp + 2u <= bank_data_len) {
                        uint16_t ptr     = read_be16(bank_data + fp);
                        uint8_t tgt_bank = in_system_addr(ptr) ? 0xffu : table->bank;

                        if (!data_range_at(tgt_bank, ptr, &project->data_ranges))
                            add_data_range(&project->data_ranges, tgt_bank, ptr,
                                           DATA_DMD_FULLFRAME, 0);
                        fp += 2u;
                    } else if (kind == TABLE_BYTE) {
                        fp++;
                    } else if (kind == TABLE_WORD) {
                        fp += 2u;
                    } else if (table_kind_is_far(kind)) {
                        fp += 3u;
                    } else {
                        fp += 2u; /* ptr16 kinds */
                    }
                }
            }
            pos += row_width;
        }
    }
}

static const char *basename_ptr(const char *path)
{
    const char *slash;

    if (!path || !*path) {
        return "";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void write_config_address(FILE *out, int has_bank, uint8_t bank, uint32_t addr)
{
    if (has_bank) {
        fprintf(out, "B%02x_A%04x", bank, (unsigned)addr & 0xffffu);
    } else if ((addr & 0xffffu) >= 0x8000u) {
        /* System bank: deterministically bank 0xFF even without explicit has_bank. */
        fprintf(out, "Bff_A%04x", (unsigned)addr & 0xffffu);
    } else {
        /* RAM (<0x4000) or ambiguous paged address — keep legacy format. */
        fprintf(out, "0x%04x", (unsigned)addr & 0xffffu);
    }
}

static void write_escaped_value(FILE *out, const char *value)
{
    int needs_quotes = 0;
    const char *p;

    if (!value) {
        fputs("\"\"", out);
        return;
    }
    for (p = value; *p; p++) {
        if (*p == '\n' || *p == ';' || *p == '#' || *p == '\\' || *p == '"' ||
            isspace((unsigned char)*p)) {
            needs_quotes = 1;
            break;
        }
    }
    if (!needs_quotes) {
        fputs(value, out);
        return;
    }
    fputc('"', out);
    for (p = value; *p; p++) {
        switch (*p) {
        case '\n':
            fputs("\\n", out);
            break;
        case ';':
            fputs("\\;", out);
            break;
        case '#':
            fputs("\\#", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '"':
            fputs("\\\"", out);
            break;
        default:
            fputc(*p, out);
            break;
        }
    }
    fputc('"', out);
}

static const char *table_field_kind_name(TableFieldKind kind)
{
    switch (kind) {
    case TABLE_PTR16_STRING:
        return "ptr16_string";
    case TABLE_PTR16_DATA:
        return "ptr16_data";
    case TABLE_PTR16_CODE:
        return "ptr16_code";
    case TABLE_PTR16_TABLE:
        return "ptr16_table";
    case TABLE_PTR16_DMD_FULLFRAME:
        return "ptr16_dmd_fullframe";
    case TABLE_PTR16_SPRITE:
        return "ptr16_sprite";
    case TABLE_FAR_STRING:
        return "far_string";
    case TABLE_FAR_DATA:
        return "far_data";
    case TABLE_FAR_TABLE:
        return "far_table";
    case TABLE_FAR_CODE:
        return "far_code";
    case TABLE_FAR_DMD_FULLFRAME:
        return "far_dmd_fullframe";
    case TABLE_FAR_SPRITE:
        return "far_sprite";
    case TABLE_BYTE:
        return "byte";
    case TABLE_WORD:
        return "word";
    }
    return "byte";
}

static void write_table_schema_text(FILE *out, const TableSchema *schema)
{
    size_t i;

    for (i = 0; i < schema->count; i++) {
        if (i != 0) {
            fputs(", ", out);
        }
        fputs(schema->items[i].type_name ? schema->items[i].type_name
                                         : table_field_kind_name(schema->items[i].kind), out);
        if (schema->items[i].count != 1u) {
            fprintf(out, "[%lu]", (unsigned long)schema->items[i].count);
        }
    }
}

static void write_inline_signature_value(FILE *out, const InlineSignature *sig)
{
    write_table_schema_text(out, &sig->schema);
    if (sig->flow_stop) {
        fputs(", flow_stop", out);
    }
}

static void write_data_range_value(FILE *out, const DataRange *range)
{
    switch (range->kind) {
    case DATA_BYTES:
        fprintf(out, "bytes[%lu]", (unsigned long)range->length);
        break;
    case DATA_STRING:
        fputs("string", out);
        break;
    case DATA_STRING_LP:
        fputs("string_lp", out);
        break;
    case DATA_STRING_FIXED:
        fprintf(out, "string[%lu]", (unsigned long)range->length);
        break;
    case DATA_DMD_FULLFRAME:
        fputs("dmd_fullframe", out);
        break;
    case DATA_PTR16_STRING:
        fputs("ptr16_string", out);
        break;
    case DATA_PTR16_DATA:
        fputs("ptr16_data", out);
        break;
    case DATA_PTR16_CODE:
        fputs("ptr16_code", out);
        break;
    case DATA_PTR16_TABLE:
        fputs("ptr16_table", out);
        break;
    case DATA_FAR_STRING:
        fputs("far_string", out);
        break;
    case DATA_FAR_DATA:
        fputs("far_data", out);
        break;
    case DATA_FAR_TABLE:
        fputs("far_table", out);
        break;
    case DATA_FAR_CODE:
        fputs("far_code", out);
        break;
    case DATA_FAR_DMD_FULLFRAME:
        fputs("far_dmd_fullframe", out);
        break;
    case DATA_SPRITE:
        fputs("sprite", out);
        break;
    case DATA_SPRITE_NOHEADER:
        fprintf(out, "sprite_noheader[%lu]", (unsigned long)range->length);
        break;
    case DATA_PTR16_SPRITE:
        fputs("ptr16_sprite", out);
        break;
    case DATA_FAR_SPRITE:
        fputs("far_sprite", out);
        break;
    }
}

static void write_table_def_value(FILE *out, const TableDef *table)
{
    if (table->has_header) {
        fputs("counted(", out);
        write_table_schema_text(out, &table->schema);
        fputc(')', out);
    } else {
        fprintf(out, "rows[%lu](", (unsigned long)table->rows);
        write_table_schema_text(out, &table->schema);
        fputc(')', out);
    }
}

static void write_labels_section(FILE *out, const ConfigLabels *labels)
{
    size_t i;

    if (labels->count == 0) {
        return;
    }
    fputs("\n[labels]\n", out);
    for (i = 0; i < labels->count; i++) {
        write_config_address(out, labels->items[i].has_bank, labels->items[i].bank,
                             labels->items[i].addr);
        fputs(" = ", out);
        write_escaped_value(out, labels->items[i].name);
        fputc('\n', out);
    }
}

static void write_entries_section(FILE *out, const ConfigEntries *entries)
{
    size_t i;

    if (entries->count == 0) {
        return;
    }
    fputs("\n[entries]\n", out);
    for (i = 0; i < entries->count; i++) {
        write_config_address(out, entries->items[i].has_bank, entries->items[i].bank,
                             entries->items[i].addr);
        fputs(" = code\n", out);
    }
}

static void write_inline_section(FILE *out, const InlineSignatures *sigs)
{
    size_t i;

    if (sigs->count == 0) {
        return;
    }
    fputs("\n[inline]\n", out);
    for (i = 0; i < sigs->count; i++) {
        write_config_address(out, sigs->items[i].has_bank, sigs->items[i].bank, sigs->items[i].addr);
        fputs(" = ", out);
        write_inline_signature_value(out, &sigs->items[i]);
        fputc('\n', out);
    }
}

static void write_data_section(FILE *out, const DataRanges *ranges)
{
    size_t i;

    if (ranges->count == 0) {
        return;
    }
    fputs("\n[data]\n", out);
    for (i = 0; i < ranges->count; i++) {
        write_config_address(out, 1, ranges->items[i].bank, ranges->items[i].addr);
        fputs(" = ", out);
        write_data_range_value(out, &ranges->items[i]);
        fputc('\n', out);
    }
}

static void write_tables_section(FILE *out, const TableDefs *tables)
{
    size_t i;

    if (tables->count == 0) {
        return;
    }
    fputs("\n[tables]\n", out);
    for (i = 0; i < tables->count; i++) {
        write_config_address(out, 1, tables->items[i].bank, tables->items[i].addr);
        fputs(" = ", out);
        write_table_def_value(out, &tables->items[i]);
        fputc('\n', out);
    }
}

static void write_types_section(FILE *out, const ConfigTypes *types)
{
    size_t i, j;

    if (types->count == 0) {
        return;
    }
    fputs("\n[types]\n", out);
    for (i = 0; i < types->count; i++) {
        const ConfigType *t = &types->items[i];
        fprintf(out, "%s:%s =", t->name,
                t->kind == TABLE_BYTE ? "byte" : "word");
        for (j = 0; j < t->value_count; j++) {
            if (j > 0) {
                fputc(',', out);
            }
            fprintf(out, " 0x%02x:%s", t->values[j].value, t->values[j].name);
        }
        fputc('\n', out);
    }
}

static void write_docs_section(FILE *out, const char *name, const ConfigDocs *docs)
{
    size_t i;

    if (docs->count == 0) {
        return;
    }
    fprintf(out, "\n[%s]\n", name);
    for (i = 0; i < docs->count; i++) {
        write_config_address(out, docs->items[i].has_bank, docs->items[i].bank, docs->items[i].addr);
        fputs(" = ", out);
        write_escaped_value(out, docs->items[i].text);
        fputc('\n', out);
    }
}

static int config_item_is_system(int has_bank, uint8_t bank, uint32_t addr)
{
    return in_system_addr(addr) && (!has_bank || bank == 0xffu);
}

static int config_item_is_bank_local(int has_bank, uint8_t bank, uint32_t addr, uint8_t target_bank)
{
    return has_bank && bank == target_bank && addr >= APEX_PAGED_ORG && addr < 0x8000u;
}

static ConfigLabels filter_config_labels_for_system(const ConfigLabels *src)
{
    ConfigLabels dst = {0};
    size_t i;

    for (i = 0; i < src->count; i++) {
        if (config_item_is_system(src->items[i].has_bank, src->items[i].bank, src->items[i].addr)) {
            dst.count++;
        }
    }
    if (dst.count == 0) {
        return dst;
    }
    dst.items = xmalloc(dst.count * sizeof(dst.items[0]));
    dst.cap = dst.count;
    dst.count = 0;
    for (i = 0; i < src->count; i++) {
        if (config_item_is_system(src->items[i].has_bank, src->items[i].bank, src->items[i].addr)) {
            dst.items[dst.count++] = src->items[i];
        }
    }
    return dst;
}

static ConfigLabels filter_config_labels_for_bank(const ConfigLabels *src, uint8_t bank_id)
{
    ConfigLabels dst = {0};
    size_t i;

    for (i = 0; i < src->count; i++) {
        if (config_item_is_bank_local(src->items[i].has_bank, src->items[i].bank,
                                      src->items[i].addr, bank_id)) {
            dst.count++;
        }
    }
    if (dst.count == 0) {
        return dst;
    }
    dst.items = xmalloc(dst.count * sizeof(dst.items[0]));
    dst.cap = dst.count;
    dst.count = 0;
    for (i = 0; i < src->count; i++) {
        if (config_item_is_bank_local(src->items[i].has_bank, src->items[i].bank,
                                      src->items[i].addr, bank_id)) {
            dst.items[dst.count++] = src->items[i];
        }
    }
    return dst;
}

static ConfigEntries filter_config_entries_for_system(const ConfigEntries *src)
{
    ConfigEntries dst = {0};
    size_t i;

    for (i = 0; i < src->count; i++) {
        if (config_item_is_system(src->items[i].has_bank, src->items[i].bank, src->items[i].addr)) {
            dst.count++;
        }
    }
    if (dst.count == 0) {
        return dst;
    }
    dst.items = xmalloc(dst.count * sizeof(dst.items[0]));
    dst.cap = dst.count;
    dst.count = 0;
    for (i = 0; i < src->count; i++) {
        if (config_item_is_system(src->items[i].has_bank, src->items[i].bank, src->items[i].addr)) {
            dst.items[dst.count++] = src->items[i];
        }
    }
    return dst;
}

static ConfigEntries filter_config_entries_for_bank(const ConfigEntries *src, uint8_t bank_id)
{
    ConfigEntries dst = {0};
    size_t i;

    for (i = 0; i < src->count; i++) {
        if (config_item_is_bank_local(src->items[i].has_bank, src->items[i].bank,
                                      src->items[i].addr, bank_id)) {
            dst.count++;
        }
    }
    if (dst.count == 0) {
        return dst;
    }
    dst.items = xmalloc(dst.count * sizeof(dst.items[0]));
    dst.cap = dst.count;
    dst.count = 0;
    for (i = 0; i < src->count; i++) {
        if (config_item_is_bank_local(src->items[i].has_bank, src->items[i].bank,
                                      src->items[i].addr, bank_id)) {
            dst.items[dst.count++] = src->items[i];
        }
    }
    return dst;
}

static DataRanges filter_data_ranges_for_bank(const DataRanges *src, uint8_t bank_id)
{
    DataRanges dst = {0};
    size_t i;

    for (i = 0; i < src->count; i++) {
        if (src->items[i].bank == bank_id &&
            src->items[i].addr >= APEX_PAGED_ORG && src->items[i].addr < 0x8000u) {
            dst.count++;
        }
    }
    if (dst.count == 0) {
        return dst;
    }
    dst.items = xmalloc(dst.count * sizeof(dst.items[0]));
    dst.cap = dst.count;
    dst.count = 0;
    for (i = 0; i < src->count; i++) {
        if (src->items[i].bank == bank_id &&
            src->items[i].addr >= APEX_PAGED_ORG && src->items[i].addr < 0x8000u) {
            dst.items[dst.count++] = src->items[i];
        }
    }
    return dst;
}

static DataRanges filter_data_ranges_for_system(const DataRanges *src)
{
    DataRanges dst = {0};
    size_t i;

    for (i = 0; i < src->count; i++) {
        if (src->items[i].bank == 0xffu && in_system_addr(src->items[i].addr)) {
            dst.count++;
        }
    }
    if (dst.count == 0) {
        return dst;
    }
    dst.items = xmalloc(dst.count * sizeof(dst.items[0]));
    dst.cap = dst.count;
    dst.count = 0;
    for (i = 0; i < src->count; i++) {
        if (src->items[i].bank == 0xffu && in_system_addr(src->items[i].addr)) {
            dst.items[dst.count++] = src->items[i];
        }
    }
    return dst;
}

static TableDefs filter_tables_for_bank(const TableDefs *src, uint8_t bank_id)
{
    TableDefs dst = {0};
    size_t i;

    for (i = 0; i < src->count; i++) {
        if (src->items[i].bank == bank_id &&
            src->items[i].addr >= APEX_PAGED_ORG && src->items[i].addr < 0x8000u) {
            dst.count++;
        }
    }
    if (dst.count == 0) {
        return dst;
    }
    dst.items = xmalloc(dst.count * sizeof(dst.items[0]));
    dst.cap = dst.count;
    dst.count = 0;
    for (i = 0; i < src->count; i++) {
        if (src->items[i].bank == bank_id &&
            src->items[i].addr >= APEX_PAGED_ORG && src->items[i].addr < 0x8000u) {
            dst.items[dst.count++] = src->items[i];
        }
    }
    return dst;
}

static TableDefs filter_tables_for_system(const TableDefs *src)
{
    TableDefs dst = {0};
    size_t i;

    for (i = 0; i < src->count; i++) {
        if (src->items[i].bank == 0xffu && in_system_addr(src->items[i].addr)) {
            dst.count++;
        }
    }
    if (dst.count == 0) {
        return dst;
    }
    dst.items = xmalloc(dst.count * sizeof(dst.items[0]));
    dst.cap = dst.count;
    dst.count = 0;
    for (i = 0; i < src->count; i++) {
        if (src->items[i].bank == 0xffu && in_system_addr(src->items[i].addr)) {
            dst.items[dst.count++] = src->items[i];
        }
    }
    return dst;
}

static void free_filtered_config_labels(ConfigLabels *labels)
{
    free(labels->items);
    memset(labels, 0, sizeof(*labels));
}

static void free_filtered_config_entries(ConfigEntries *entries)
{
    free(entries->items);
    memset(entries, 0, sizeof(*entries));
}

static void free_filtered_data_ranges(DataRanges *ranges)
{
    free(ranges->items);
    memset(ranges, 0, sizeof(*ranges));
}

static void free_filtered_table_defs(TableDefs *tables)
{
    free(tables->items);
    memset(tables, 0, sizeof(*tables));
}

static char *project_dup_string(const char *s)
{
    size_t len = strlen(s) + 1u;
    char *copy = xmalloc(len);

    memcpy(copy, s, len);
    return copy;
}

static void free_table_schema(TableSchema *schema)
{
    free(schema->items);
    schema->items = NULL;
    schema->count = 0;
    schema->cap = 0;
}

static void free_inline_signatures(InlineSignatures *sigs)
{
    size_t i;

    for (i = 0; i < sigs->count; i++) {
        free_table_schema(&sigs->items[i].schema);
    }
    free(sigs->items);
    memset(sigs, 0, sizeof(*sigs));
}

static void free_config_labels(ConfigLabels *labels)
{
    size_t i;

    for (i = 0; i < labels->count; i++) {
        free((char *)labels->items[i].name);
    }
    free(labels->items);
    memset(labels, 0, sizeof(*labels));
}

static void free_table_defs(TableDefs *tables)
{
    size_t i;

    for (i = 0; i < tables->count; i++) {
        free_table_schema(&tables->items[i].schema);
    }
    free(tables->items);
    memset(tables, 0, sizeof(*tables));
}

static void free_config_entries(ConfigEntries *entries)
{
    free(entries->items);
    memset(entries, 0, sizeof(*entries));
}

static void free_schema_defs(SchemaDefs *schemas)
{
    size_t i;

    for (i = 0; i < schemas->count; i++) {
        free((char *)schemas->items[i].name);
        free_table_schema(&schemas->items[i].schema);
    }
    free(schemas->items);
    memset(schemas, 0, sizeof(*schemas));
}

static void free_config_docs(ConfigDocs *docs)
{
    size_t i;

    for (i = 0; i < docs->count; i++) {
        free((char *)docs->items[i].text);
    }
    free(docs->items);
    memset(docs, 0, sizeof(*docs));
}

static void free_config_symbols(ConfigSymbols *symbols)
{
    size_t i;

    for (i = 0; i < symbols->count; i++) {
        free((char *)symbols->items[i].name);
    }
    free(symbols->items);
    memset(symbols, 0, sizeof(*symbols));
}

static void free_label_set(LabelSet *labels)
{
    free(labels->items);
    memset(labels, 0, sizeof(*labels));
}

static void free_reference_set(ReferenceSet *refs)
{
    free(refs->items);
    memset(refs, 0, sizeof(*refs));
}

static int config_addr_matches(int item_has_bank, uint8_t item_bank, uint32_t item_addr,
                               int has_bank, uint8_t bank, uint32_t addr)
{
    if (item_addr != addr) {
        return 0;
    }
    if (item_has_bank == has_bank && (!has_bank || item_bank == bank)) {
        return 1;
    }
    if (addr >= 0x8000u && addr <= 0xffffu &&
        ((item_has_bank && item_bank == 0xffu && !has_bank) ||
         (!item_has_bank && has_bank && bank == 0xffu))) {
        return 1;
    }
    return 0;
}

static int project_addr_is_system(int has_bank, uint8_t bank, uint32_t addr)
{
    return in_system_addr(addr) && (!has_bank || bank == 0xffu);
}

static int project_addr_is_bank_local(int has_bank, uint8_t bank, uint32_t addr)
{
    return has_bank && bank != 0xffu && addr >= APEX_PAGED_ORG && addr < 0x8000u;
}

static void mark_analysis_scope(ApexProject *project, ApexAnalyzeScope scope)
{
    if (!project) {
        return;
    }
    if (project->analysis_scope == APEX_ANALYZE_SCOPE_FULL || scope == APEX_ANALYZE_SCOPE_NONE) {
        return;
    }
    if (scope == APEX_ANALYZE_SCOPE_FULL || project->analysis_scope == APEX_ANALYZE_SCOPE_NONE) {
        project->analysis_scope = scope;
    }
}

static void mark_bank_analysis_scope(ApexProject *project, uint8_t bank)
{
    if (!project || project->analysis_scope == APEX_ANALYZE_SCOPE_FULL) {
        return;
    }
    if (project->analysis_scope == APEX_ANALYZE_SCOPE_NONE) {
        project->analysis_scope = APEX_ANALYZE_SCOPE_BANK_ONLY;
        project->analysis_bank_id = bank;
        return;
    }
    if (project->analysis_scope == APEX_ANALYZE_SCOPE_BANK_ONLY &&
        project->analysis_bank_id == bank) {
        return;
    }
    project->analysis_scope = APEX_ANALYZE_SCOPE_FULL;
}

static LabelSet *project_label_set(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr)
{
    int bank_index;

    if (!has_bank || (bank == 0xffu && in_system_addr(addr))) {
        return &project->system_labels;
    }
    bank_index = bank_index_for_id(project->rom.data, project->banks, bank);
    if (bank_index < 0) {
        return NULL;
    }
    return &project->bank_labels[bank_index];
}

static void remove_runtime_label(LabelSet *labels, uint32_t addr, const char *name)
{
    size_t i;

    if (!labels || !name) {
        return;
    }
    for (i = 0; i < labels->count; i++) {
        if (labels->items[i].addr == addr && strcmp(labels->items[i].name, name) == 0) {
            memmove(&labels->items[i], &labels->items[i + 1],
                    (labels->count - i - 1u) * sizeof(labels->items[0]));
            labels->count--;
            return;
        }
    }
}

static ConfigLabel *upsert_config_label(ConfigLabels *labels, int has_bank, uint8_t bank,
                                        uint32_t addr, const char *name, char **old_name)
{
    size_t i;

    if (old_name) {
        *old_name = NULL;
    }

    for (i = 0; i < labels->count; i++) {
        if (config_addr_matches(labels->items[i].has_bank, labels->items[i].bank,
                                labels->items[i].addr, has_bank, bank, addr)) {
            if (old_name) {
                *old_name = (char *)labels->items[i].name;
            } else {
                free((char *)labels->items[i].name);
            }
            labels->items[i].name = project_dup_string(name);
            return &labels->items[i];
        }
    }
    if (labels->count == labels->cap) {
        size_t new_cap = labels->cap == 0 ? 16u : labels->cap * 2u;
        ConfigLabel *items = realloc(labels->items, new_cap * sizeof(labels->items[0]));

        if (!items) {
            die("out of memory");
        }
        labels->items = items;
        labels->cap = new_cap;
    }
    labels->items[labels->count].has_bank = has_bank;
    labels->items[labels->count].bank = bank;
    labels->items[labels->count].addr = addr;
    labels->items[labels->count].name = project_dup_string(name);
    labels->count++;
    return &labels->items[labels->count - 1u];
}

static char *remove_config_label(ConfigLabels *labels, int has_bank, uint8_t bank, uint32_t addr)
{
    size_t i;
    char *removed;

    for (i = 0; i < labels->count; i++) {
        if (config_addr_matches(labels->items[i].has_bank, labels->items[i].bank,
                                labels->items[i].addr, has_bank, bank, addr)) {
            removed = (char *)labels->items[i].name;
            memmove(&labels->items[i], &labels->items[i + 1],
                    (labels->count - i - 1u) * sizeof(labels->items[0]));
            labels->count--;
            return removed;
        }
    }
    return NULL;
}

static ConfigDoc *upsert_config_doc(ConfigDocs *docs, int has_bank, uint8_t bank, uint32_t addr,
                                    const char *text)
{
    size_t i;

    for (i = 0; i < docs->count; i++) {
        if (config_addr_matches(docs->items[i].has_bank, docs->items[i].bank, docs->items[i].addr,
                                has_bank, bank, addr)) {
            free((char *)docs->items[i].text);
            docs->items[i].text = project_dup_string(text);
            return &docs->items[i];
        }
    }
    if (docs->count == docs->cap) {
        size_t new_cap = docs->cap == 0 ? 16u : docs->cap * 2u;
        ConfigDoc *items = realloc(docs->items, new_cap * sizeof(docs->items[0]));

        if (!items) {
            die("out of memory");
        }
        docs->items = items;
        docs->cap = new_cap;
    }
    docs->items[docs->count].has_bank = has_bank;
    docs->items[docs->count].bank = bank;
    docs->items[docs->count].addr = addr;
    docs->items[docs->count].text = project_dup_string(text);
    docs->count++;
    return &docs->items[docs->count - 1u];
}

static int remove_config_doc(ConfigDocs *docs, int has_bank, uint8_t bank, uint32_t addr)
{
    size_t i;

    for (i = 0; i < docs->count; i++) {
        if (config_addr_matches(docs->items[i].has_bank, docs->items[i].bank, docs->items[i].addr,
                                has_bank, bank, addr)) {
            free((char *)docs->items[i].text);
            memmove(&docs->items[i], &docs->items[i + 1],
                    (docs->count - i - 1u) * sizeof(docs->items[0]));
            docs->count--;
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Undo/redo
 *
 * Each edit is recorded as a full deep copy of every mutable config section.
 * Restoring a snapshot replaces the live config wholesale and forces a full
 * re-analysis; the derived label/reference/render state is rebuilt from it.
 *
 * The only borrowed pointer in the config is TableField.type_name, which aims
 * into a ConfigType.name owned by config_types. A snapshot deep-copies its own
 * config_types first and rebinds every schema's type_name into that copy, so
 * each snapshot is internally self-consistent and can be moved around (and into
 * the project) without dangling.
 * ------------------------------------------------------------------------- */

#define APEX_UNDO_MAX 50

typedef struct {
    InlineSignatures inline_sigs;
    ConfigLabels     config_labels;
    ConfigEntries    config_entries;
    TableDefs        tables;
    SchemaDefs       schemas;
    ConfigDocs       docs;
    ConfigSymbols    symbols;
    DataRanges       data_ranges;
    ConfigTypes      config_types;
    ConfigEntries    ref_exclusions;
    ConfigOptions    options;
    char             action[24];
} ApexConfigSnapshot;

struct ApexUndo {
    ApexConfigSnapshot undo[APEX_UNDO_MAX];
    size_t             undo_count;
    ApexConfigSnapshot redo[APEX_UNDO_MAX];
    size_t             redo_count;
    int                group_depth;
    int                group_captured;
    char               group_label[24];
};

static const char *rebind_type_name(const ConfigTypes *types, const char *old_name)
{
    size_t i;

    if (!old_name) {
        return NULL;
    }
    for (i = 0; i < types->count; i++) {
        if (strcmp(types->items[i].name, old_name) == 0) {
            return types->items[i].name;
        }
    }
    return NULL; /* type no longer exists — degrade to raw kind */
}

static TableSchema copy_schema_rebind(const TableSchema *src, const ConfigTypes *types)
{
    TableSchema dst;
    size_t i;

    memset(&dst, 0, sizeof(dst));
    for (i = 0; i < src->count; i++) {
        add_table_field(&dst, src->items[i].kind, src->items[i].count);
        dst.items[dst.count - 1u].type_name = rebind_type_name(types, src->items[i].type_name);
    }
    return dst;
}

static ConfigTypes copy_config_types(const ConfigTypes *src)
{
    ConfigTypes dst;
    size_t i, j;

    memset(&dst, 0, sizeof(dst));
    if (src->count == 0) {
        return dst;
    }
    dst.items = xmalloc(src->count * sizeof(dst.items[0]));
    dst.cap = src->count;
    dst.count = src->count;
    for (i = 0; i < src->count; i++) {
        const ConfigType *s = &src->items[i];
        ConfigType *t = &dst.items[i];

        t->name = project_dup_string(s->name);
        t->kind = s->kind;
        t->value_count = s->value_count;
        t->value_cap = s->value_count;
        t->values = s->value_count ? xmalloc(s->value_count * sizeof(t->values[0])) : NULL;
        for (j = 0; j < s->value_count; j++) {
            t->values[j].value = s->values[j].value;
            t->values[j].name = project_dup_string(s->values[j].name);
        }
    }
    return dst;
}

static InlineSignatures copy_inline_sigs(const InlineSignatures *src, const ConfigTypes *types)
{
    InlineSignatures dst;
    size_t i;

    memset(&dst, 0, sizeof(dst));
    if (src->count == 0) {
        return dst;
    }
    dst.items = xmalloc(src->count * sizeof(dst.items[0]));
    dst.cap = src->count;
    dst.count = src->count;
    for (i = 0; i < src->count; i++) {
        dst.items[i].has_bank = src->items[i].has_bank;
        dst.items[i].bank = src->items[i].bank;
        dst.items[i].addr = src->items[i].addr;
        dst.items[i].length = src->items[i].length;
        dst.items[i].flow_stop = src->items[i].flow_stop;
        dst.items[i].schema = copy_schema_rebind(&src->items[i].schema, types);
    }
    return dst;
}

static TableDefs copy_table_defs(const TableDefs *src, const ConfigTypes *types)
{
    TableDefs dst;
    size_t i;

    memset(&dst, 0, sizeof(dst));
    if (src->count == 0) {
        return dst;
    }
    dst.items = xmalloc(src->count * sizeof(dst.items[0]));
    dst.cap = src->count;
    dst.count = src->count;
    for (i = 0; i < src->count; i++) {
        dst.items[i].bank = src->items[i].bank;
        dst.items[i].addr = src->items[i].addr;
        dst.items[i].has_header = src->items[i].has_header;
        dst.items[i].rows = src->items[i].rows;
        dst.items[i].schema = copy_schema_rebind(&src->items[i].schema, types);
    }
    return dst;
}

static SchemaDefs copy_schema_defs(const SchemaDefs *src, const ConfigTypes *types)
{
    SchemaDefs dst;
    size_t i;

    memset(&dst, 0, sizeof(dst));
    if (src->count == 0) {
        return dst;
    }
    dst.items = xmalloc(src->count * sizeof(dst.items[0]));
    dst.cap = src->count;
    dst.count = src->count;
    for (i = 0; i < src->count; i++) {
        dst.items[i].name = project_dup_string(src->items[i].name);
        dst.items[i].schema = copy_schema_rebind(&src->items[i].schema, types);
    }
    return dst;
}

static ConfigLabels copy_config_labels(const ConfigLabels *src)
{
    ConfigLabels dst;
    size_t i;

    memset(&dst, 0, sizeof(dst));
    if (src->count == 0) {
        return dst;
    }
    dst.items = xmalloc(src->count * sizeof(dst.items[0]));
    dst.cap = src->count;
    dst.count = src->count;
    for (i = 0; i < src->count; i++) {
        dst.items[i] = src->items[i];
        dst.items[i].name = project_dup_string(src->items[i].name);
    }
    return dst;
}

static ConfigDocs copy_config_docs(const ConfigDocs *src)
{
    ConfigDocs dst;
    size_t i;

    memset(&dst, 0, sizeof(dst));
    if (src->count == 0) {
        return dst;
    }
    dst.items = xmalloc(src->count * sizeof(dst.items[0]));
    dst.cap = src->count;
    dst.count = src->count;
    for (i = 0; i < src->count; i++) {
        dst.items[i] = src->items[i];
        dst.items[i].text = project_dup_string(src->items[i].text);
    }
    return dst;
}

static ConfigSymbols copy_config_symbols(const ConfigSymbols *src)
{
    ConfigSymbols dst;
    size_t i;

    memset(&dst, 0, sizeof(dst));
    if (src->count == 0) {
        return dst;
    }
    dst.items = xmalloc(src->count * sizeof(dst.items[0]));
    dst.cap = src->count;
    dst.count = src->count;
    for (i = 0; i < src->count; i++) {
        dst.items[i] = src->items[i];
        dst.items[i].name = project_dup_string(src->items[i].name);
    }
    return dst;
}

static ConfigEntries copy_config_entries(const ConfigEntries *src)
{
    ConfigEntries dst;

    memset(&dst, 0, sizeof(dst));
    if (src->count == 0) {
        return dst;
    }
    dst.items = xmalloc(src->count * sizeof(dst.items[0]));
    dst.cap = src->count;
    dst.count = src->count;
    memcpy(dst.items, src->items, src->count * sizeof(dst.items[0]));
    return dst;
}

static DataRanges copy_data_ranges(const DataRanges *src)
{
    DataRanges dst;

    memset(&dst, 0, sizeof(dst));
    if (src->count == 0) {
        return dst;
    }
    dst.items = xmalloc(src->count * sizeof(dst.items[0]));
    dst.cap = src->count;
    dst.count = src->count;
    memcpy(dst.items, src->items, src->count * sizeof(dst.items[0]));
    return dst;
}

static void snapshot_capture(const ApexProject *p, const char *action, ApexConfigSnapshot *out)
{
    memset(out, 0, sizeof(*out));
    out->config_types   = copy_config_types(&p->config_types);
    out->inline_sigs    = copy_inline_sigs(&p->inline_sigs, &out->config_types);
    out->tables         = copy_table_defs(&p->tables, &out->config_types);
    out->schemas        = copy_schema_defs(&p->schemas, &out->config_types);
    out->config_labels  = copy_config_labels(&p->config_labels);
    out->config_entries = copy_config_entries(&p->config_entries);
    out->docs           = copy_config_docs(&p->docs);
    out->symbols        = copy_config_symbols(&p->symbols);
    out->data_ranges    = copy_data_ranges(&p->data_ranges);
    out->ref_exclusions = copy_config_entries(&p->ref_exclusions);
    out->options        = p->options;
    snprintf(out->action, sizeof(out->action), "%s", action ? action : "edit");
}

static void snapshot_free(ApexConfigSnapshot *s)
{
    free_inline_signatures(&s->inline_sigs);
    free_config_labels(&s->config_labels);
    free_config_entries(&s->config_entries);
    free_table_defs(&s->tables);
    free_schema_defs(&s->schemas);
    free_config_docs(&s->docs);
    free_config_symbols(&s->symbols);
    free(s->data_ranges.items);
    free_config_types(&s->config_types);
    free_config_entries(&s->ref_exclusions);
    memset(s, 0, sizeof(*s));
}

/* Free the project's current config and move the snapshot's owned arrays in.
   Leaves *s zeroed (ownership transferred to the project). */
static void snapshot_apply(ApexProject *p, ApexConfigSnapshot *s)
{
    free_inline_signatures(&p->inline_sigs);
    free_config_labels(&p->config_labels);
    free_config_entries(&p->config_entries);
    free_table_defs(&p->tables);
    free_schema_defs(&p->schemas);
    free_config_docs(&p->docs);
    free_config_symbols(&p->symbols);
    free(p->data_ranges.items);
    memset(&p->data_ranges, 0, sizeof(p->data_ranges));
    free_config_types(&p->config_types);
    free_config_entries(&p->ref_exclusions);

    p->inline_sigs    = s->inline_sigs;
    p->config_labels  = s->config_labels;
    p->config_entries = s->config_entries;
    p->tables         = s->tables;
    p->schemas        = s->schemas;
    p->docs           = s->docs;
    p->symbols        = s->symbols;
    p->data_ranges    = s->data_ranges;
    p->config_types   = s->config_types;
    p->ref_exclusions = s->ref_exclusions;
    p->options        = s->options;
    memset(s, 0, sizeof(*s));
}

static struct ApexUndo *undo_ensure(ApexProject *p)
{
    if (!p->undo) {
        p->undo = xmalloc(sizeof(*p->undo));
        memset(p->undo, 0, sizeof(*p->undo));
    }
    return p->undo;
}

/* Push *snap onto a fixed-capacity stack, dropping the oldest entry on
   overflow. Moves ownership: *snap is zeroed. */
static void undo_stack_push(ApexConfigSnapshot *stack, size_t *count, ApexConfigSnapshot *snap)
{
    if (*count == APEX_UNDO_MAX) {
        snapshot_free(&stack[0]);
        memmove(&stack[0], &stack[1], (APEX_UNDO_MAX - 1u) * sizeof(stack[0]));
        (*count)--;
    }
    stack[(*count)++] = *snap;
    memset(snap, 0, sizeof(*snap));
}

static void undo_clear_redo(struct ApexUndo *u)
{
    while (u->redo_count) {
        snapshot_free(&u->redo[--u->redo_count]);
    }
}

/* Record a pre-edit checkpoint. Called at the top of every mutator. Inside an
   edit group only the first mutation captures (so the group is one undo step). */
static void project_record_edit(ApexProject *p, const char *action)
{
    struct ApexUndo *u;
    ApexConfigSnapshot snap;

    if (!p) {
        return;
    }
    u = undo_ensure(p);
    if (u->group_depth > 0) {
        if (u->group_captured) {
            return;
        }
        u->group_captured = 1;
        if (u->group_label[0]) {
            action = u->group_label;
        }
    }
    snapshot_capture(p, action, &snap);
    undo_stack_push(u->undo, &u->undo_count, &snap);
    undo_clear_redo(u);
}

void apex_project_begin_edit_group(ApexProject *project, const char *label)
{
    struct ApexUndo *u;

    if (!project) {
        return;
    }
    u = undo_ensure(project);
    if (u->group_depth == 0) {
        u->group_captured = 0;
        snprintf(u->group_label, sizeof(u->group_label), "%s", label ? label : "edit");
    }
    u->group_depth++;
}

void apex_project_end_edit_group(ApexProject *project)
{
    struct ApexUndo *u;

    if (!project || !project->undo) {
        return;
    }
    u = project->undo;
    if (u->group_depth > 0) {
        u->group_depth--;
    }
    if (u->group_depth == 0) {
        u->group_captured = 0;
        u->group_label[0] = '\0';
    }
}

int apex_project_can_undo(const ApexProject *project)
{
    return project && project->undo && project->undo->undo_count > 0;
}

int apex_project_can_redo(const ApexProject *project)
{
    return project && project->undo && project->undo->redo_count > 0;
}

const char *apex_project_undo_label(const ApexProject *project)
{
    if (!apex_project_can_undo(project)) {
        return NULL;
    }
    return project->undo->undo[project->undo->undo_count - 1u].action;
}

const char *apex_project_redo_label(const ApexProject *project)
{
    if (!apex_project_can_redo(project)) {
        return NULL;
    }
    return project->undo->redo[project->undo->redo_count - 1u].action;
}

int apex_project_undo(ApexProject *project)
{
    struct ApexUndo *u;
    ApexConfigSnapshot cur, snap;

    if (!apex_project_can_undo(project)) {
        return 1;
    }
    u = project->undo;
    snapshot_capture(project, u->undo[u->undo_count - 1u].action, &cur);
    undo_stack_push(u->redo, &u->redo_count, &cur);
    snap = u->undo[--u->undo_count];
    snapshot_apply(project, &snap);
    apex_project_invalidate(project, APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER);
    return 0;
}

int apex_project_redo(ApexProject *project)
{
    struct ApexUndo *u;
    ApexConfigSnapshot cur, snap;

    if (!apex_project_can_redo(project)) {
        return 1;
    }
    u = project->undo;
    snapshot_capture(project, u->redo[u->redo_count - 1u].action, &cur);
    undo_stack_push(u->undo, &u->undo_count, &cur);
    snap = u->redo[--u->redo_count];
    snapshot_apply(project, &snap);
    apex_project_invalidate(project, APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER);
    return 0;
}

ApexProject *apex_project_open(const char *rom_path, const char *config_path)
{
    ApexProject *project = xmalloc(sizeof(*project));

    memset(project, 0, sizeof(*project));
    project->rom_path = project_dup_string(rom_path);
    project->config_path = config_path ? project_dup_string(config_path) : NULL;
    project->options.labels_are_entries = 0;

    load_config(project->config_path, &project->inline_sigs, &project->config_labels,
                &project->config_entries, &project->tables, &project->schemas,
                &project->docs, &project->symbols,
                &project->data_ranges, &project->options, &project->config_types,
                &project->ref_exclusions);
    validate_config_classification(&project->config_entries, &project->tables,
                                   &project->data_ranges);

    project->rom = read_file(project->rom_path);
    if (project->rom.size != 512u * 1024u && project->rom.size != 1024u * 1024u) {
        die("unsupported ROM size %lu; expected 512 KB or 1 MB",
            (unsigned long)project->rom.size);
    }
    if (project->rom.size < APEX_SYSTEM_SIZE ||
        (project->rom.size - APEX_SYSTEM_SIZE) % APEX_BANK_SIZE != 0) {
        die("ROM size does not match WPC bank layout");
    }

    project->paged_size = project->rom.size - APEX_SYSTEM_SIZE;
    project->banks = project->paged_size / APEX_BANK_SIZE;
    project->bank_labels = xmalloc(project->banks * sizeof(project->bank_labels[0]));
    memset(project->bank_labels, 0, project->banks * sizeof(project->bank_labels[0]));
    collect_vectors(project->rom.data + project->paged_size, project->vectors,
                    sizeof(project->vectors) / sizeof(project->vectors[0]));
    project->dirty_flags = APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    project->analysis_scope = APEX_ANALYZE_SCOPE_FULL;
    return project;
}

void apex_project_free(ApexProject *project)
{
    size_t i;

    if (!project) {
        return;
    }
    for (i = 0; i < project->banks; i++) {
        free_label_set(&project->bank_labels[i]);
    }
    free(project->bank_labels);
    free_label_set(&project->system_labels);
    free_reference_set(&project->refs);
    free_inline_signatures(&project->inline_sigs);
    free_config_types(&project->config_types);
    free_config_labels(&project->config_labels);
    free_config_entries(&project->config_entries);
    free_config_entries(&project->ref_exclusions);
    free_table_defs(&project->tables);
    free_schema_defs(&project->schemas);
    free_config_docs(&project->docs);
    free_config_symbols(&project->symbols);
    free(project->data_ranges.items);
    if (project->render_cache) {
        apex_render_document_free(project->render_cache);
        free(project->render_cache);
    }
    if (project->undo) {
        while (project->undo->undo_count) {
            snapshot_free(&project->undo->undo[--project->undo->undo_count]);
        }
        while (project->undo->redo_count) {
            snapshot_free(&project->undo->redo[--project->undo->redo_count]);
        }
        free(project->undo);
    }
    free(project->rom.data);
    free((char *)project->rom_path);
    free((char *)project->config_path);
    free(project);
}

void apex_project_invalidate(ApexProject *project, unsigned dirty_flags)
{
    if (!project) {
        return;
    }
    project->dirty_flags |= dirty_flags;
    if (dirty_flags & APEX_DIRTY_ANALYSIS) {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    }
}

static void analyze_system_region(ApexProject *project)
{
    ConfigLabels system_config_labels = filter_config_labels_for_system(&project->config_labels);
    ConfigEntries system_config_entries = filter_config_entries_for_system(&project->config_entries);
    DataRanges system_data_ranges = filter_data_ranges_for_system(&project->data_ranges);
    TableDefs system_tables = filter_tables_for_system(&project->tables);

    free_label_set(&project->system_labels);
    remove_references_from_source_range(&project->refs, 0xffu, APEX_SYSTEM_ORG, 0xffffu);

    build_system_labels(project->rom.data + project->paged_size, project->vectors,
                        sizeof(project->vectors) / sizeof(project->vectors[0]),
                        &project->inline_sigs, project->rom.data, project->banks,
                        project->bank_labels, &project->system_labels, &system_config_labels,
                        &system_config_entries, &system_data_ranges, &project->options,
                        &project->refs);
    sort_table_defs(&project->tables);
    sort_inline_signatures(&project->inline_sigs);
    sort_data_ranges(&project->data_ranges);
    apply_data_range_labels(&system_data_ranges, project->rom.data, project->banks,
                            project->rom.data + project->paged_size, project->bank_labels,
                            &project->system_labels, &project->refs);
    apply_table_labels(&system_tables, project->rom.data, project->banks, project->bank_labels,
                       &project->system_labels, project->rom.data + project->paged_size,
                       &project->refs);
    collect_code_targets(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE,
                         APEX_SYSTEM_ORG, &project->system_labels, &project->inline_sigs,
                         project->rom.data, project->banks, project->bank_labels,
                         &project->system_labels, &project->data_ranges, 0xffu, &project->refs,
                         &project->ref_exclusions);
    apply_string_content_labels(&project->system_labels, project->rom.data + project->paged_size,
                                last_non_ff(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE),
                                APEX_SYSTEM_ORG);
    sort_label_set(&project->system_labels);
    sort_and_dedup_refs(&project->refs);
    prune_unreferenced_generated_labels(&project->system_labels, 0xffu, &project->refs);
    {
        size_t i;
        for (i = 0; i < project->banks; i++) {
            uint8_t bank_id = project->rom.data[i * APEX_BANK_SIZE];
            prune_unreferenced_generated_labels(&project->bank_labels[i], bank_id, &project->refs);
        }
    }
    free_filtered_table_defs(&system_tables);
    free_filtered_data_ranges(&system_data_ranges);
    free_filtered_config_entries(&system_config_entries);
    free_filtered_config_labels(&system_config_labels);
}

static void analyze_bank_region(ApexProject *project, uint8_t bank_id)
{
    int bank_index = bank_index_for_id(project->rom.data, project->banks, bank_id);
    const uint8_t *bank;
    size_t used;
    ConfigLabels bank_config_labels = filter_config_labels_for_bank(&project->config_labels, bank_id);
    ConfigEntries bank_config_entries =
        filter_config_entries_for_bank(&project->config_entries, bank_id);
    DataRanges bank_data_ranges = filter_data_ranges_for_bank(&project->data_ranges, bank_id);
    TableDefs bank_tables = filter_tables_for_bank(&project->tables, bank_id);

    if (bank_index < 0) {
        free_filtered_table_defs(&bank_tables);
        free_filtered_data_ranges(&bank_data_ranges);
        free_filtered_config_entries(&bank_config_entries);
        free_filtered_config_labels(&bank_config_labels);
        analyze_full_project(project);
        return;
    }

    free_label_set(&project->bank_labels[bank_index]);
    remove_references_from_source_range(&project->refs, bank_id, APEX_PAGED_ORG, 0x7fffu);

    sort_table_defs(&project->tables);
    sort_inline_signatures(&project->inline_sigs);
    sort_data_ranges(&project->data_ranges);
    apply_config_bank_labels(&bank_config_labels, project->rom.data, project->banks,
                             project->bank_labels, &project->options);
    apply_config_bank_entries(&bank_config_entries, project->rom.data, project->banks,
                              project->bank_labels);
    apply_data_range_labels(&bank_data_ranges, project->rom.data, project->banks,
                            project->rom.data + project->paged_size, project->bank_labels,
                            &project->system_labels, &project->refs);
    apply_table_labels(&bank_tables, project->rom.data, project->banks, project->bank_labels,
                       &project->system_labels, project->rom.data + project->paged_size,
                       &project->refs);

    bank = project->rom.data + (size_t)bank_index * APEX_BANK_SIZE;
    used = last_non_ff(bank, APEX_BANK_SIZE);
    if (used > 1) {
        collect_code_targets(bank, used, APEX_PAGED_ORG, &project->bank_labels[bank_index],
                             &project->inline_sigs, project->rom.data, project->banks,
                             project->bank_labels, &project->system_labels, &project->data_ranges,
                             bank_id, &project->refs, &project->ref_exclusions);
    }
    apply_string_content_labels(&project->bank_labels[bank_index], bank, used, APEX_PAGED_ORG);
    sort_label_set(&project->bank_labels[bank_index]);
    sort_and_dedup_refs(&project->refs);
    prune_unreferenced_generated_labels(&project->bank_labels[bank_index], bank_id, &project->refs);
    prune_unreferenced_generated_labels(&project->system_labels, 0xffu, &project->refs);
    {
        size_t i;
        for (i = 0; i < project->banks; i++) {
            if ((int)i == bank_index) {
                continue;
            }
            prune_unreferenced_generated_labels(&project->bank_labels[i],
                                                project->rom.data[i * APEX_BANK_SIZE],
                                                &project->refs);
        }
    }
    free_filtered_table_defs(&bank_tables);
    free_filtered_data_ranges(&bank_data_ranges);
    free_filtered_config_entries(&bank_config_entries);
    free_filtered_config_labels(&bank_config_labels);
}

static void analyze_full_project(ApexProject *project)
{
    size_t i;
    size_t banks = project->banks;

    sort_table_defs(&project->tables);
    sort_inline_signatures(&project->inline_sigs);

    free_label_set(&project->system_labels);
    free_reference_set(&project->refs);
    for (i = 0; i < banks; i++) {
        free_label_set(&project->bank_labels[i]);
    }
    build_system_labels(project->rom.data + project->paged_size, project->vectors,
                        sizeof(project->vectors) / sizeof(project->vectors[0]),
                        &project->inline_sigs, project->rom.data, banks, project->bank_labels,
                        &project->system_labels, &project->config_labels, &project->config_entries,
                        &project->data_ranges, &project->options, &project->refs);
    apply_config_bank_labels(&project->config_labels, project->rom.data, banks,
                             project->bank_labels, &project->options);
    apply_config_bank_entries(&project->config_entries, project->rom.data, banks,
                              project->bank_labels);
    apply_data_range_labels(&project->data_ranges, project->rom.data, banks,
                            project->rom.data + project->paged_size, project->bank_labels,
                            &project->system_labels, &project->refs);
    apply_table_labels(&project->tables, project->rom.data, banks, project->bank_labels,
                       &project->system_labels, project->rom.data + project->paged_size,
                       &project->refs);
    inject_dmd_table_data_ranges(project);
    sort_data_ranges(&project->data_ranges);
    collect_code_targets(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE,
                         APEX_SYSTEM_ORG, &project->system_labels, &project->inline_sigs,
                         project->rom.data, banks, project->bank_labels, &project->system_labels,
                         &project->data_ranges, 0xff, &project->refs, &project->ref_exclusions);
    collect_bank_code_targets(project->rom.data, banks, &project->inline_sigs,
                              project->bank_labels, &project->system_labels,
                              &project->data_ranges, &project->refs, &project->ref_exclusions);
    {
        size_t prev_count, prev_code;
        do {
            size_t j;
            prev_count = project->system_labels.count;
            prev_code = 0;
            for (j = 0; j < project->system_labels.count; j++) {
                if (project->system_labels.items[j].is_code) prev_code++;
            }
            collect_code_targets(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE,
                                 APEX_SYSTEM_ORG, &project->system_labels, &project->inline_sigs,
                                 project->rom.data, banks, project->bank_labels,
                                 &project->system_labels, &project->data_ranges, 0xff,
                                 &project->refs, &project->ref_exclusions);
            {
                size_t j2, curr_code = 0;
                for (j2 = 0; j2 < project->system_labels.count; j2++) {
                    if (project->system_labels.items[j2].is_code) curr_code++;
                }
                if (curr_code == prev_code && project->system_labels.count == prev_count) break;
            }
        } while (1);
    }
    for (i = 0; i < banks; i++) {
        const uint8_t *bank = project->rom.data + i * APEX_BANK_SIZE;

        apply_string_content_labels(&project->bank_labels[i], bank,
                                    last_non_ff(bank, APEX_BANK_SIZE), APEX_PAGED_ORG);
    }
    apply_string_content_labels(&project->system_labels, project->rom.data + project->paged_size,
                                last_non_ff(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE),
                                APEX_SYSTEM_ORG);
    for (i = 0; i < banks; i++) {
        sort_label_set(&project->bank_labels[i]);
    }
    sort_label_set(&project->system_labels);
    sort_and_dedup_refs(&project->refs);
}

int apex_project_analyze(ApexProject *project)
{
    if (!project) {
        return 1;
    }
    if (project->analysis_ready && !(project->dirty_flags & APEX_DIRTY_ANALYSIS)) {
        return 0;
    }
    if (!project->analysis_ready || project->analysis_scope == APEX_ANALYZE_SCOPE_FULL ||
        project->analysis_scope == APEX_ANALYZE_SCOPE_NONE) {
        analyze_full_project(project);
    } else if (project->analysis_scope == APEX_ANALYZE_SCOPE_SYSTEM_ONLY) {
        analyze_system_region(project);
    } else if (project->analysis_scope == APEX_ANALYZE_SCOPE_BANK_ONLY) {
        analyze_bank_region(project, project->analysis_bank_id);
    }
    project->analysis_ready = 1;
    project->dirty_flags &= ~APEX_DIRTY_ANALYSIS;
    project->analysis_scope = APEX_ANALYZE_SCOPE_NONE;
    project->analysis_bank_id = 0;
    return 0;
}

int apex_project_set_label(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr,
                           const char *name)
{
    LabelSet *labels;
    ConfigLabel *config_label;
    char *old_name = NULL;
    size_t i;
    int updated = 0;

    if (!project || !name || !*name) {
        return 1;
    }
    labels = project_label_set(project, has_bank, bank, addr);
    if (!labels) {
        return 1;
    }
    project_record_edit(project, "set label");
    config_label = upsert_config_label(&project->config_labels, has_bank, bank, addr, name, &old_name);
    /* Rename an existing runtime label at this address rather than adding a
       second one: when renaming match the previous config name, otherwise take
       over the auto-generated placeholder (Bxx_Ayyyy).  Without this a manual
       label at an auto-labelled address leaves the placeholder behind and the
       line renders two labels. */
    for (i = 0; i < labels->count; i++) {
        if (labels->items[i].addr == addr &&
            (old_name ? strcmp(labels->items[i].name, old_name) == 0
                      : generated_any_label_name(labels->items[i].name))) {
            labels->items[i].name = config_label->name;
            updated = 1;
            break;
        }
    }
    if (!updated) {
        Label *label = add_label(labels, addr, config_label->name,
                                 code_label_at(addr, labels->items, labels->count, 0));

        label->name = config_label->name;
    }
    /* Drop any other now-redundant label at this address (a stray generated
       placeholder or a duplicate of the new name). */
    for (i = 0; i < labels->count; ) {
        if (labels->items[i].addr == addr &&
            labels->items[i].name != config_label->name &&
            (generated_any_label_name(labels->items[i].name) ||
             strcmp(labels->items[i].name, config_label->name) == 0)) {
            memmove(&labels->items[i], &labels->items[i + 1],
                    (labels->count - i - 1u) * sizeof(labels->items[0]));
            labels->count--;
        } else {
            i++;
        }
    }
    free(old_name);
    apex_project_invalidate(project, APEX_DIRTY_LABELS | APEX_DIRTY_RENDER);
    return 0;
}

int apex_project_clear_label(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr)
{
    LabelSet *labels;
    char *removed_name;

    if (!project) {
        return 1;
    }
    labels = project_label_set(project, has_bank, bank, addr);
    if (!labels) {
        return 1;
    }
    project_record_edit(project, "clear label");
    removed_name = remove_config_label(&project->config_labels, has_bank, bank, addr);
    if (!removed_name) {
        return 1;
    }
    remove_runtime_label(labels, addr, removed_name);
    free(removed_name);
    apex_project_invalidate(project, APEX_DIRTY_LABELS | APEX_DIRTY_RENDER);
    return 0;
}

int apex_project_set_doc(ApexProject *project, int has_bank, uint8_t bank,
                         uint32_t addr, const char *text)
{
    if (!project || !text || !*text) {
        return 1;
    }
    project_record_edit(project, "set doc");
    upsert_config_doc(&project->docs, has_bank, bank, addr, text);
    apex_project_invalidate(project, APEX_DIRTY_DOCS | APEX_DIRTY_RENDER);
    return 0;
}

int apex_project_clear_doc(ApexProject *project, int has_bank, uint8_t bank,
                           uint32_t addr)
{
    if (!project) {
        return 1;
    }
    project_record_edit(project, "clear doc");
    if (!remove_config_doc(&project->docs, has_bank, bank, addr)) {
        return 1;
    }
    apex_project_invalidate(project, APEX_DIRTY_DOCS | APEX_DIRTY_RENDER);
    return 0;
}

int apex_project_set_kind(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr,
                          ApexConfigKind kind, const char *spec)
{
    const char *effective_spec = spec;

    if (!project) {
        return 1;
    }
    project_record_edit(project, "set kind");
    config_clear_entry(&project->config_entries, has_bank, bank, addr);
    config_clear_data(&project->data_ranges, bank, addr);
    config_clear_table(&project->tables, bank, addr);

    switch (kind) {
    case APEX_KIND_CODE:
        if (config_set_entry(&project->config_entries, has_bank, bank, addr) != 0) {
            return 1;
        }
        break;
    case APEX_KIND_DATA:
        if (!effective_spec || !*effective_spec) {
            effective_spec = "bytes[1]";
        }
        if (config_set_data_spec(&project->data_ranges, bank, addr, effective_spec) != 0) {
            return 1;
        }
        break;
    case APEX_KIND_STRING:
        if (config_set_data_spec(&project->data_ranges, bank, addr, "string") != 0) {
            return 1;
        }
        break;
    case APEX_KIND_TABLE:
        if (!effective_spec || !*effective_spec) {
            effective_spec = "counted(ptr16_data)";
        }
        if (config_set_table_spec(&project->tables, &project->schemas, bank, addr,
                                  effective_spec, &project->config_types) != 0) {
            return 1;
        }
        break;
    default:
        return 1;
    }
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    if (project_addr_is_system(has_bank, bank, addr)) {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_SYSTEM_ONLY);
    } else if (project_addr_is_bank_local(has_bank, bank, addr)) {
        mark_bank_analysis_scope(project, bank);
    } else {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    }
    return 0;
}

int apex_project_clear_kind(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr)
{
    if (!project) {
        return 1;
    }
    project_record_edit(project, "clear kind");
    config_clear_entry(&project->config_entries, has_bank, bank, addr);
    config_clear_data(&project->data_ranges, bank, addr);
    config_clear_table(&project->tables, bank, addr);
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    if (project_addr_is_system(has_bank, bank, addr)) {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_SYSTEM_ONLY);
    } else if (project_addr_is_bank_local(has_bank, bank, addr)) {
        mark_bank_analysis_scope(project, bank);
    } else {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    }
    return 0;
}

int apex_project_set_inline(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr,
                            const char *spec)
{
    if (!project || !spec || !*spec) {
        return 1;
    }
    project_record_edit(project, "set inline");
    if (config_set_inline_spec(&project->inline_sigs, has_bank, bank, addr, spec,
                               &project->config_types) != 0) {
        return 1;
    }
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    if (project_addr_is_system(has_bank, bank, addr)) {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_SYSTEM_ONLY);
    } else if (project_addr_is_bank_local(has_bank, bank, addr)) {
        mark_bank_analysis_scope(project, bank);
    } else {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    }
    return 0;
}

int apex_project_clear_inline(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr)
{
    if (!project) {
        return 1;
    }
    project_record_edit(project, "clear inline");
    if (config_clear_inline(&project->inline_sigs, has_bank, bank, addr) != 0) {
        return 1;
    }
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    if (project_addr_is_system(has_bank, bank, addr)) {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_SYSTEM_ONLY);
    } else if (project_addr_is_bank_local(has_bank, bank, addr)) {
        mark_bank_analysis_scope(project, bank);
    } else {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    }
    return 0;
}

int apex_project_set_table(ApexProject *project, uint8_t bank, uint32_t addr, const char *spec)
{
    if (!project || !spec || !*spec) {
        return 1;
    }
    project_record_edit(project, "set table");
    if (config_set_table_spec(&project->tables, &project->schemas, bank, addr, spec,
                               &project->config_types) != 0) {
        return 1;
    }
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    if (project_addr_is_system(1, bank, addr)) {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_SYSTEM_ONLY);
    } else if (project_addr_is_bank_local(1, bank, addr)) {
        mark_bank_analysis_scope(project, bank);
    } else {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    }
    return 0;
}

int apex_project_clear_table(ApexProject *project, uint8_t bank, uint32_t addr)
{
    if (!project) {
        return 1;
    }
    project_record_edit(project, "clear table");
    if (config_clear_table(&project->tables, bank, addr) != 0) {
        return 1;
    }
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    if (project_addr_is_system(1, bank, addr)) {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_SYSTEM_ONLY);
    } else if (project_addr_is_bank_local(1, bank, addr)) {
        mark_bank_analysis_scope(project, bank);
    } else {
        mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    }
    return 0;
}

const ApexRenderedDocument *apex_project_render(ApexProject *project, int emit_xrefs,
                                                int emit_explain)
{
    if (!project) {
        return NULL;
    }
    if (apex_project_analyze(project) != 0) {
        return NULL;
    }
    if (!project->render_cache) {
        project->render_cache = xmalloc(sizeof(*project->render_cache));
        memset(project->render_cache, 0, sizeof(*project->render_cache));
    }
    if ((project->dirty_flags & APEX_DIRTY_RENDER) || !project->render_cache->text ||
        project->render_cache_emit_xrefs != emit_xrefs ||
        project->render_cache_emit_explain != emit_explain) {
        apex_render_project(project, emit_xrefs, emit_explain, project->render_cache);
        project->render_cache_emit_xrefs = emit_xrefs;
        project->render_cache_emit_explain = emit_explain;
        project->dirty_flags &= ~(APEX_DIRTY_RENDER | APEX_DIRTY_LABELS | APEX_DIRTY_DOCS);
    }
    return project->render_cache;
}

/* ---- deterministic config ordering (mirrors apexini's sorted output) ---- */

/* Effective-bank address compare: a no-bank item sorts as the system bank. */
static int cfg_addr_cmp(int ha, uint8_t ba, uint32_t aa, int hb, uint8_t bb, uint32_t ab)
{
    uint8_t ea = ha ? ba : 0xffu;
    uint8_t eb = hb ? bb : 0xffu;
    if (ea != eb) return (int)(unsigned)ea - (int)(unsigned)eb;
    if (aa != ab) return aa < ab ? -1 : 1;
    return 0;
}

static int cfg_cmp_label(const void *a, const void *b)
{
    const ConfigLabel *x = (const ConfigLabel *)a, *y = (const ConfigLabel *)b;
    return cfg_addr_cmp(x->has_bank, x->bank, x->addr, y->has_bank, y->bank, y->addr);
}

static int cfg_cmp_entry(const void *a, const void *b)
{
    const ConfigEntry *x = (const ConfigEntry *)a, *y = (const ConfigEntry *)b;
    return cfg_addr_cmp(x->has_bank, x->bank, x->addr, y->has_bank, y->bank, y->addr);
}

static int cfg_cmp_doc(const void *a, const void *b)
{
    const ConfigDoc *x = (const ConfigDoc *)a, *y = (const ConfigDoc *)b;
    return cfg_addr_cmp(x->has_bank, x->bank, x->addr, y->has_bank, y->bank, y->addr);
}

static int cfg_cmp_sig(const void *a, const void *b)
{
    const InlineSignature *x = (const InlineSignature *)a, *y = (const InlineSignature *)b;
    return cfg_addr_cmp(x->has_bank, x->bank, x->addr, y->has_bank, y->bank, y->addr);
}

static int cfg_cmp_type(const void *a, const void *b)
{
    return strcmp(((const ConfigType *)a)->name, ((const ConfigType *)b)->name);
}

static int cfg_cmp_typeval(const void *a, const void *b)
{
    uint32_t va = ((const ConfigTypeValue *)a)->value;
    uint32_t vb = ((const ConfigTypeValue *)b)->value;
    if (va != vb) return va < vb ? -1 : 1;
    return 0;
}

static int cfg_cmp_sym(const void *a, const void *b)
{
    return strcmp(((const ConfigSymbol *)a)->name, ((const ConfigSymbol *)b)->name);
}

static int cfg_cmp_schema(const void *a, const void *b)
{
    return strcmp(((const SchemaDef *)a)->name, ((const SchemaDef *)b)->name);
}

/*
 * Sort every config section into the same deterministic order apexini emits:
 * address-keyed sections by (effective bank, address), name-keyed sections by
 * name, and enum values by value.  Reorders the project's owned arrays in place
 * (order is not semantically significant; lookups are by key, not index).
 * inline/data/table sections reuse the analysis comparators so their order stays
 * compatible with the binary-search lookups built on them.
 */
void apex_project_sort_config(ApexProject *project)
{
    size_t i;

    if (!project) {
        return;
    }
    if (project->config_types.count) {
        qsort(project->config_types.items, project->config_types.count,
              sizeof(project->config_types.items[0]), cfg_cmp_type);
        for (i = 0; i < project->config_types.count; i++) {
            ConfigType *t = &project->config_types.items[i];
            if (t->value_count > 1) {
                qsort(t->values, t->value_count, sizeof(t->values[0]), cfg_cmp_typeval);
            }
        }
    }
    if (project->schemas.count > 1) {
        qsort(project->schemas.items, project->schemas.count,
              sizeof(project->schemas.items[0]), cfg_cmp_schema);
    }
    if (project->symbols.count > 1) {
        qsort(project->symbols.items, project->symbols.count,
              sizeof(project->symbols.items[0]), cfg_cmp_sym);
    }
    if (project->config_labels.count > 1) {
        qsort(project->config_labels.items, project->config_labels.count,
              sizeof(project->config_labels.items[0]), cfg_cmp_label);
    }
    if (project->config_entries.count > 1) {
        qsort(project->config_entries.items, project->config_entries.count,
              sizeof(project->config_entries.items[0]), cfg_cmp_entry);
    }
    if (project->docs.count > 1) {
        qsort(project->docs.items, project->docs.count,
              sizeof(project->docs.items[0]), cfg_cmp_doc);
    }
    if (project->ref_exclusions.count > 1) {
        qsort(project->ref_exclusions.items, project->ref_exclusions.count,
              sizeof(project->ref_exclusions.items[0]), cfg_cmp_entry);
    }
    /* inline by (bank, addr) to match apexini; the lookup is order-independent.
       data/table sections are already (bank, addr)-sorted by these helpers. */
    if (project->inline_sigs.count > 1) {
        qsort(project->inline_sigs.items, project->inline_sigs.count,
              sizeof(project->inline_sigs.items[0]), cfg_cmp_sig);
    }
    sort_data_ranges(&project->data_ranges);
    sort_table_defs(&project->tables);
}

int apex_project_save_overlay(ApexProject *project, const char *path, const char *include_path)
{
    FILE *out;
    const char *inc;

    if (!project || !path || !*path) {
        return 1;
    }
    apex_project_sort_config(project);
    out = fopen(path, "w");
    if (!out) {
        return 1;
    }
    fputs("; Apex ImGui overlay\n", out);
    inc = (include_path && *include_path) ? include_path : project->config_path;
    if (inc && *inc) {
        fprintf(out, "include = %s\n", basename_ptr(inc));
    }
    write_types_section(out, &project->config_types);
    write_labels_section(out, &project->config_labels);
    write_entries_section(out, &project->config_entries);
    write_inline_section(out, &project->inline_sigs);
    write_data_section(out, &project->data_ranges);
    write_tables_section(out, &project->tables);
    write_docs_section(out, "docs", &project->docs);
    if (fclose(out) != 0) {
        return 1;
    }
    return 0;
}

int apex_project_set_type(ApexProject *project, const char *name, int is_word,
                          const char *values_str)
{
    if (!project || !name || !*name) {
        return 1;
    }
    project_record_edit(project, "set type");
    TableFieldKind kind = is_word ? TABLE_WORD : TABLE_BYTE;
    config_set_type(&project->config_types, name, kind, values_str ? values_str : "");
    apex_project_invalidate(project, APEX_DIRTY_RENDER);
    return 0;
}

int apex_project_remove_type(ApexProject *project, const char *name)
{
    if (!project || !name) {
        return 1;
    }
    project_record_edit(project, "remove type");
    return config_remove_type(&project->config_types, name) ? 0 : 1;
}

int apex_project_add_ref_exclusion(ApexProject *project, int has_bank, uint8_t bank,
                                   uint32_t addr)
{
    if (!project) {
        return 1;
    }
    project_record_edit(project, "add exclusion");
    config_set_entry(&project->ref_exclusions, has_bank, bank, addr);
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    return 0;
}

int apex_project_remove_ref_exclusion(ApexProject *project, int has_bank, uint8_t bank,
                                      uint32_t addr)
{
    if (!project) {
        return 1;
    }
    project_record_edit(project, "remove exclusion");
    if (config_clear_entry(&project->ref_exclusions, has_bank, bank, addr) != 0) {
        return 1;
    }
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    return 0;
}

int apex_project_set_symbol(ApexProject *project, const char *name, uint32_t value)
{
    if (!project) return 1;
    project_record_edit(project, "set symbol");
    if (config_set_symbol(&project->symbols, name, value) != 0) return 1;
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    return 0;
}

int apex_project_clear_symbol(ApexProject *project, const char *name)
{
    if (!project) return 1;
    project_record_edit(project, "clear symbol");
    if (config_clear_symbol(&project->symbols, name) != 0) return 1;
    project->dirty_flags |= APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER;
    mark_analysis_scope(project, APEX_ANALYZE_SCOPE_FULL);
    return 0;
}

/* -------------------------------------------------------------------------
 * Code candidate scanner
 * ------------------------------------------------------------------------- */

static void candidate_push(ApexCodeCandidates *out, uint8_t bank, uint32_t addr,
                            int score, int tier, int instr_count, const char *preview)
{
    size_t i;
    /* deduplicate: if same address already present, keep higher score */
    for (i = 0; i < out->count; i++) {
        if (out->items[i].bank == bank && out->items[i].addr == addr) {
            if (score > out->items[i].score) {
                out->items[i].score       = score;
                out->items[i].tier        = tier;
                out->items[i].instr_count = instr_count;
                if (preview && preview[0])
                    strncpy(out->items[i].preview, preview,
                            APEX_CANDIDATE_PREVIEW - 1);
            }
            return;
        }
    }
    if (out->count == out->cap) {
        size_t new_cap = out->cap == 0 ? 64 : out->cap * 2;
        ApexCodeCandidate *ni = (ApexCodeCandidate *)realloc(
            out->items, new_cap * sizeof(out->items[0]));
        if (!ni) return;
        out->items = ni;
        out->cap   = new_cap;
    }
    out->items[out->count].bank        = bank;
    out->items[out->count].addr        = addr;
    out->items[out->count].score       = score;
    out->items[out->count].tier        = tier;
    out->items[out->count].instr_count = instr_count;
    out->items[out->count].preview[0]  = '\0';
    if (preview && preview[0])
        strncpy(out->items[out->count].preview, preview,
                APEX_CANDIDATE_PREVIEW - 1);
    out->count++;
}

/* Probe up to MAX_PROBE bytes of ROM starting at base_addr.
   Returns score 0-100; 0 means "not code".
   Sets *instr_count_out and writes first instruction text to preview. */
#define SCAN_MAX_PROBE_BYTES 160
#define SCAN_MAX_INSTRS      24
#define SCAN_MIN_INSTRS_TERM  3   /* min instrs required if terminator found   */
#define SCAN_MIN_INSTRS_NOTERM 10 /* min instrs required without terminator    */

/* PSHS (0x34) and PSHU (0x36) are classic 6809 function prologues —
   strong evidence of a real subroutine start. */
#define OPCODE_PSHS 0x34u
#define OPCODE_PSHU 0x36u

static int probe_code(const uint8_t *data, size_t len, uint32_t base_addr,
                       int *instr_count_out, char *preview, size_t preview_size)
{
    int    instr_count   = 0;
    size_t pos           = 0;
    int    unique[256]   = {0};
    int    unique_count  = 0;
    int    prologue_bonus = 0;
    char   buf[80];

    /* Check for PSHS/PSHU at the very first byte — subroutine prologue */
    if (len >= 2 &&
        (data[0] == OPCODE_PSHS || data[0] == OPCODE_PSHU))
        prologue_bonus = 15;

    while (pos < SCAN_MAX_PROBE_BYTES && pos < len && instr_count < SCAN_MAX_INSTRS) {
        Cpu6809InstrInfo info = cpu6809_disassemble_info(
            data + pos, len - pos, base_addr + (uint32_t)pos, buf, sizeof(buf));

        if (info.size == 0) return 0;   /* invalid opcode → disqualify */

        if (instr_count == 0 && preview && preview_size > 0) {
            strncpy(preview, buf, preview_size - 1);
            preview[preview_size - 1] = '\0';
        }

        if (!unique[(unsigned char)data[pos]]) {
            unique[(unsigned char)data[pos]] = 1;
            unique_count++;
        }
        instr_count++;
        pos += info.size;

        if (info.flags & CPU6809_FLOW_STOP) {
            if (instr_count < SCAN_MIN_INSTRS_TERM) return 0;
            int score = 50 + instr_count * 3 + unique_count * 2 + prologue_bonus;
            if (score > 100) score = 100;
            if (instr_count_out) *instr_count_out = instr_count;
            return score;
        }
    }

    if (instr_count < SCAN_MIN_INSTRS_NOTERM) return 0;
    /* Long clean stream without explicit terminator: lower ceiling */
    int score = 20 + instr_count * 2 + unique_count * 3 + prologue_bonus;
    if (score > 80) score = 80;
    if (instr_count_out) *instr_count_out = instr_count;
    return score;
}

static int addr_is_classified(const ApexProject *p, uint8_t bank, uint32_t addr,
                               const LabelSet *labels)
{
    if (code_label_at(addr, labels->items, labels->count, labels->sorted))
        return 1;
    if (data_range_at(bank, addr, &p->data_ranges))
        return 1;
    if (table_def_at(bank, addr, &p->tables))
        return 1;
    return 0;
}

/* Scan one bank for candidates.
   src_bank_id = the bank byte (0xff for system).
   data / size / base_addr describe the bank's ROM slice.
   valid_bank[256] = precomputed "this bank byte exists in ROM" table. */
static void scan_bank(const ApexProject  *p,
                       uint8_t             src_bank_id,
                       const uint8_t      *data,
                       size_t              size,
                       uint32_t            base_addr,
                       const LabelSet     *labels,
                       const uint8_t       valid_bank[256],
                       ApexCodeCandidates *out)
{
    size_t  i;
    int     prev_classified = 1; /* treat bank start as "came from classified" */

    for (i = 0; i + 2 < size; i++) {
        uint32_t addr = base_addr + (uint32_t)i;
        int curr_classified = addr_is_classified(p, src_bank_id, addr, labels);

        /* ---- Tier 1: far-pointer scan ----------------------------------- */
        /* A far pointer is addr_hi, addr_lo, bank_byte.
           We only scan this in paged banks (not inside the system bank itself)
           to avoid blowing up the candidate count. */
        if (i + 2 < size) {
            uint16_t tgt_addr = ((uint16_t)data[i] << 8) | data[i + 1];
            uint8_t  tgt_bank = data[i + 2];

            /* target must be a valid far-code address */
            if (valid_bank[tgt_bank] &&
                ((tgt_bank == 0xffu && tgt_addr >= 0x8000u) ||
                 (tgt_bank != 0xffu && tgt_addr >= APEX_PAGED_ORG &&
                  tgt_addr < 0x8000u))) {

                /* resolve target */
                const uint8_t  *tgt_data  = NULL;
                size_t          tgt_size  = 0;
                uint32_t        tgt_base  = 0;
                const LabelSet *tgt_lbls  = NULL;

                if (tgt_bank == 0xffu) {
                    tgt_data = p->rom.data + p->paged_size;
                    tgt_size = p->rom.size - p->paged_size;
                    tgt_base = APEX_SYSTEM_ORG;
                    tgt_lbls = &p->system_labels;
                } else {
                    int bi = bank_index_for_id(p->rom.data, p->banks, tgt_bank);
                    if (bi >= 0) {
                        tgt_data = p->rom.data + (size_t)bi * APEX_BANK_SIZE;
                        tgt_size = APEX_BANK_SIZE;
                        tgt_base = APEX_PAGED_ORG;
                        tgt_lbls = &p->bank_labels[(size_t)bi];
                    }
                }

                if (tgt_data && !addr_is_classified(p, tgt_bank, tgt_addr, tgt_lbls)) {
                    size_t tgt_off = tgt_addr - tgt_base;
                    if (tgt_off < tgt_size) {
                        char preview[APEX_CANDIDATE_PREVIEW] = "";
                        int  ic  = 0;
                        int  sc  = probe_code(tgt_data + tgt_off, tgt_size - tgt_off,
                                              tgt_addr, &ic, preview, sizeof(preview));
                        if (sc >= 30) {
                            int boosted = sc + 20;
                            if (boosted > 100) boosted = 100;
                            candidate_push(out, tgt_bank, tgt_addr,
                                           boosted, 1, ic, preview);
                        }
                    }
                }
            }
        }

        /* ---- Tier 2: probe at unclassified region starts ---------------- */
        /* Only probe at the first byte of each unclassified run to limit
           the total number of probes and avoid mid-code probes. */
        if (!curr_classified && prev_classified) {
            char preview[APEX_CANDIDATE_PREVIEW] = "";
            int  ic = 0;
            int  sc = probe_code(data + i, size - i, addr, &ic, preview, sizeof(preview));
            if (sc >= 50) {
                candidate_push(out, src_bank_id, addr, sc, 2, ic, preview);
            }
        }

        prev_classified = curr_classified;
    }
}

static int cand_cmp_score(const void *a, const void *b)
{
    const ApexCodeCandidate *ca = (const ApexCodeCandidate *)a;
    const ApexCodeCandidate *cb = (const ApexCodeCandidate *)b;
    return cb->score - ca->score;   /* descending */
}

void apex_scan_code_candidates(const ApexProject *p, ApexCodeCandidates *out)
{
    size_t  i;
    uint8_t valid_bank[256];

    if (!p || !out) return;
    out->items = NULL;
    out->count = 0;
    out->cap   = 0;

    /* precompute valid bank IDs */
    for (i = 0; i < 256; i++) valid_bank[i] = 0;
    valid_bank[0xffu] = 1;
    for (i = 0; i < p->banks; i++)
        valid_bank[p->rom.data[i * APEX_BANK_SIZE]] = 1;

    /* scan paged banks */
    for (i = 0; i < p->banks; i++) {
        uint8_t  bid  = p->rom.data[i * APEX_BANK_SIZE];
        scan_bank(p, bid,
                  p->rom.data + i * APEX_BANK_SIZE,
                  APEX_BANK_SIZE,
                  APEX_PAGED_ORG,
                  &p->bank_labels[i],
                  valid_bank, out);
    }

    /* scan system bank */
    if (p->rom.size > p->paged_size) {
        scan_bank(p, 0xffu,
                  p->rom.data + p->paged_size,
                  p->rom.size  - p->paged_size,
                  APEX_SYSTEM_ORG,
                  &p->system_labels,
                  valid_bank, out);
    }

    /* sort by score descending */
    if (out->count > 0)
        qsort(out->items, out->count, sizeof(out->items[0]), cand_cmp_score);
}

void apex_free_code_candidates(ApexCodeCandidates *candidates)
{
    if (!candidates) return;
    free(candidates->items);
    candidates->items = NULL;
    candidates->count = 0;
    candidates->cap   = 0;
}

/* -------------------------------------------------------------------------
 * Inline-dispatcher candidate scanner
 *
 * Looks for functions matching the WPC 6809 inline-data convention:
 *   PULS X           ; pull return-addr into X (points at inline bytes)
 *   LDB  ,X+  ...    ; read inline fields with post-increment
 *   JMP  ,X          ; jump past inline data
 * or the PSHS X / RTS variant.
 * ------------------------------------------------------------------------- */

/* 6809 opcodes used by the scanner */
#define OPC_PULS  0x35u   /* PULS  rlist  (S-stack) */
#define OPC_PULU  0x37u   /* PULU  rlist  (U-stack) */
#define OPC_PSHS  0x34u   /* PSHS  rlist */
#define OPC_PSHU  0x36u   /* PSHU  rlist */
#define MASK_X    0x10u   /* X bit in PULS/PSHS register mask */
#define OPC_LDA_IDX 0xA6u
#define OPC_LDB_IDX 0xE6u
#define OPC_LDD_IDX 0xECu
#define OPC_LDX_IDX 0xAEu
#define OPC_LDU_IDX 0xEEu
#define OPC_LDY_IDX 0xAEu  /* prefix 0x10 + 0xAE */
#define OPC_STD_IDX 0xEDu
#define OPC_STX_IDX 0xAFu
#define OPC_JMP_IDX 0x6Eu
#define OPC_RTS     0x39u
#define OPC_PREFIX  0x10u

#define POSTBYTE_X_PLUS1 0x80u  /* ,X+  */
#define POSTBYTE_X_PLUS2 0x81u  /* ,X++ */
#define POSTBYTE_X_NONE  0x84u  /* ,X   (no offset) */

/* Field kinds used to build spec strings */
#define FKIND_BYTE  0
#define FKIND_WORD  1
#define FKIND_FAR   2   /* merged byte+word → far_code */
#define MAX_FIELDS  8

/* Build spec string from field array; returns 0 on success */
static int build_spec(const int *fields, int nfields, char *out, size_t outsz)
{
    int pos = 0;
    for (int i = 0; i < nfields && pos < (int)outsz - 1; i++) {
        if (i > 0) {
            int r = snprintf(out + pos, outsz - (size_t)pos, ", ");
            if (r < 0) return 1;
            pos += r;
        }
        const char *name = fields[i] == FKIND_BYTE ? "byte" :
                           fields[i] == FKIND_WORD ? "word" : "far_code";
        int r = snprintf(out + pos, outsz - (size_t)pos, "%s", name);
        if (r < 0) return 1;
        pos += r;
    }
    out[pos] = '\0';
    return pos == 0 ? 1 : 0;
}

/* Analyse up to ~25 instructions of the function at (data, len, base_addr).
   Returns 1 if this looks like an inline dispatcher; fills fields/nfields. */
static int scan_detect_inline_dispatcher(const uint8_t *data, size_t len,
                                     uint32_t base_addr,
                                     int *fields, int *nfields)
{
    (void)base_addr;
    size_t pos       = 0;
    int found_puls_x = 0;     /* seen PULS/PULU with X bit */
    int found_term   = 0;     /* seen JMP ,X or PSHS X;RTS */
    int instr_cnt    = 0;
    *nfields         = 0;

    while (pos < len && instr_cnt < 25) {
        uint8_t op = data[pos];
        instr_cnt++;

        /* PULS / PULU with X bit in mask */
        if ((op == OPC_PULS || op == OPC_PULU) && pos + 1 < len) {
            if (data[pos + 1] & MASK_X) found_puls_x = 1;
            pos += 2;
            continue;
        }

        /* Prefixed 2-byte opcode (0x10 prefix) */
        if (op == OPC_PREFIX && pos + 2 < len) {
            uint8_t op2  = data[pos + 1];
            uint8_t pb   = data[pos + 2];
            /* LDY ,X++ = 0x10 0xAE 0x81 */
            if (op2 == 0xAEu && pb == POSTBYTE_X_PLUS2 && *nfields < MAX_FIELDS) {
                fields[(*nfields)++] = FKIND_WORD;
                pos += 3;
                continue;
            }
            /* LDS ,X++ = 0x10 0xEE 0x81 */
            if (op2 == 0xEEu && pb == POSTBYTE_X_PLUS2 && *nfields < MAX_FIELDS) {
                fields[(*nfields)++] = FKIND_WORD;
                pos += 3;
                continue;
            }
            /* otherwise: advance past prefixed instruction (2+operand bytes) */
            Cpu6809InstrInfo info = cpu6809_disassemble_info(
                data + pos, len - pos, 0, NULL, 0);
            pos += info.size > 0 ? info.size : 1;
            continue;
        }

        /* Post-increment byte load: LDA/LDB ,X+ */
        if ((op == OPC_LDA_IDX || op == OPC_LDB_IDX) &&
            pos + 1 < len && data[pos + 1] == POSTBYTE_X_PLUS1) {
            if (*nfields < MAX_FIELDS) fields[(*nfields)++] = FKIND_BYTE;
            pos += 2;
            continue;
        }

        /* Post-increment word load: LDD/LDX/LDU ,X++ */
        if ((op == OPC_LDD_IDX || op == OPC_LDX_IDX || op == OPC_LDU_IDX) &&
            pos + 1 < len && data[pos + 1] == POSTBYTE_X_PLUS2) {
            if (*nfields < MAX_FIELDS) fields[(*nfields)++] = FKIND_WORD;
            pos += 2;
            continue;
        }

        /* JMP ,X — clean terminator */
        if (op == OPC_JMP_IDX && pos + 1 < len &&
            data[pos + 1] == POSTBYTE_X_NONE) {
            found_term = 1;
            break;
        }

        /* RTS — check if preceded by PSHS with X bit (other variant) */
        if (op == OPC_RTS) {
            found_term = (found_puls_x && *nfields > 0); /* lenient: accept if we saw fields */
            break;
        }

        /* PSHS with X bit: sets up new return address past inline data */
        if ((op == OPC_PSHS || op == OPC_PSHU) && pos + 1 < len &&
            (data[pos + 1] & MASK_X)) {
            pos += 2;
            continue;   /* expect RTS to follow */
        }

        /* Any other instruction: advance generically */
        Cpu6809InstrInfo info = cpu6809_disassemble_info(
            data + pos, len - pos, 0, NULL, 0);
        if (info.size == 0) return 0;   /* invalid opcode → not code */
        pos += info.size;
    }

    if (!found_puls_x || *nfields == 0 || !found_term) return 0;

    /* Merge consecutive (BYTE, WORD) pairs → FAR field */
    int merged[MAX_FIELDS];
    int nm = 0;
    for (int i = 0; i < *nfields; ) {
        if (i + 1 < *nfields &&
            fields[i] == FKIND_BYTE && fields[i + 1] == FKIND_WORD) {
            merged[nm++] = FKIND_FAR;
            i += 2;
        } else {
            merged[nm++] = fields[i++];
        }
    }
    for (int i = 0; i < nm; i++) fields[i] = merged[i];
    *nfields = nm;

    return 1;
}

/* Validate a single callsite: are the bytes after the JSR consistent
   with the spec?  Returns 1 = valid, 0 = invalid / can't tell. */
static int validate_callsite_bytes(const uint8_t *after, size_t avail,
                                    const int *fields, int nfields,
                                    const uint8_t valid_bank[256])
{
    size_t need = 0;
    for (int i = 0; i < nfields; i++)
        need += (fields[i] == FKIND_BYTE) ? 1 : (fields[i] == FKIND_WORD) ? 2 : 3;
    if (avail < need) return 0;

    size_t off = 0;
    for (int i = 0; i < nfields; i++) {
        if (fields[i] == FKIND_BYTE) {
            off++;   /* any byte is valid */
        } else if (fields[i] == FKIND_WORD) {
            uint16_t v = ((uint16_t)after[off] << 8) | after[off + 1];
            /* word must be a plausible ROM address */
            if (v < APEX_PAGED_ORG) return 0;
            off += 2;
        } else { /* FKIND_FAR */
            uint16_t addr = ((uint16_t)after[off] << 8) | after[off + 1];
            uint8_t  bank = after[off + 2];
            if (!valid_bank[bank]) return 0;
            if (bank == 0xFFu) {
                if (addr < APEX_SYSTEM_ORG) return 0;
            } else {
                if (addr < APEX_PAGED_ORG || addr >= 0x8000u) return 0;
            }
            off += 3;
        }
    }
    return 1;
}

/* Count JSR-extended (0xBD hi lo) callsites for a system-bank function
   (cpu_addr in 0x8000-0xFFFF); fills callsite_count and callsite_valid. */
static void count_callsites(const ApexProject *p, uint32_t func_addr,
                              const int *fields, int nfields,
                              const uint8_t valid_bank[256],
                              int *cnt, int *valid)
{
    *cnt   = 0;
    *valid = 0;
    if (p->rom.size < 3) return;

    uint8_t hi = (uint8_t)((func_addr >> 8) & 0xFFu);
    uint8_t lo = (uint8_t)( func_addr        & 0xFFu);

    for (size_t i = 0; i + 2 < p->rom.size; i++) {
        if (p->rom.data[i] == 0xBDu &&
            p->rom.data[i + 1] == hi &&
            p->rom.data[i + 2] == lo) {
            (*cnt)++;
            size_t after_off = i + 3;
            size_t avail     = p->rom.size - after_off;
            if (validate_callsite_bytes(p->rom.data + after_off, avail,
                                         fields, nfields, valid_bank))
                (*valid)++;
        }
    }
}

static int has_inline_sig(const InlineSignatures *sigs, int has_bank,
                           uint8_t bank, uint32_t addr)
{
    size_t i;
    for (i = 0; i < sigs->count; i++) {
        if (sigs->items[i].addr == addr &&
            (!has_bank || sigs->items[i].bank == bank)) return 1;
    }
    return 0;
}

static void inline_push(ApexInlineCandidates *out,
                          uint8_t bank, uint32_t addr,
                          const char *spec, int score,
                          int callsite_count, int callsite_valid)
{
    /* deduplicate */
    for (size_t i = 0; i < out->count; i++) {
        if (out->items[i].bank == bank && out->items[i].addr == addr) {
            if (score > out->items[i].score) {
                out->items[i].score          = score;
                out->items[i].callsite_count = callsite_count;
                out->items[i].callsite_valid = callsite_valid;
                snprintf(out->items[i].spec, APEX_INLINE_SPEC_MAX, "%s", spec);
            }
            return;
        }
    }
    if (out->count == out->cap) {
        size_t nc = out->cap == 0 ? 32 : out->cap * 2;
        ApexInlineCandidate *ni = (ApexInlineCandidate *)realloc(
            out->items, nc * sizeof(out->items[0]));
        if (!ni) return;
        out->items = ni;
        out->cap   = nc;
    }
    ApexInlineCandidate *c = &out->items[out->count++];
    c->bank           = bank;
    c->addr           = addr;
    c->score          = score;
    c->callsite_count = callsite_count;
    c->callsite_valid = callsite_valid;
    strncpy(c->spec, spec, APEX_INLINE_SPEC_MAX - 1);
    c->spec[APEX_INLINE_SPEC_MAX - 1] = '\0';
}

static int inline_cmp(const void *a, const void *b)
{
    return ((const ApexInlineCandidate *)b)->score -
           ((const ApexInlineCandidate *)a)->score;
}

void apex_scan_inline_candidates(const ApexProject *p, ApexInlineCandidates *out)
{
    size_t  i, j;
    uint8_t valid_bank[256];

    if (!p || !out) return;
    out->items = NULL;
    out->count = 0;
    out->cap   = 0;

    for (i = 0; i < 256; i++) valid_bank[i] = 0;
    valid_bank[0xFFu] = 1;
    for (i = 0; i < p->banks; i++)
        valid_bank[p->rom.data[i * APEX_BANK_SIZE]] = 1;

    /* Scan every known code label in every bank */
    /* Helper: process one LabelSet */
    for (int pass = 0; pass < 2; pass++) {
        size_t lset_count = (pass == 0) ? p->banks : 1;

        for (i = 0; i < lset_count; i++) {
            const LabelSet *ls;
            uint8_t  bank_id;
            uint32_t base_addr;
            const uint8_t *bdata;
            size_t   bsize;

            if (pass == 0) {
                ls        = &p->bank_labels[i];
                bank_id   = p->rom.data[i * APEX_BANK_SIZE];
                base_addr = APEX_PAGED_ORG;
                bdata     = p->rom.data + i * APEX_BANK_SIZE;
                bsize     = APEX_BANK_SIZE;
            } else {
                ls        = &p->system_labels;
                bank_id   = 0xFFu;
                base_addr = APEX_SYSTEM_ORG;
                bdata     = p->rom.data + p->paged_size;
                bsize     = p->rom.size - p->paged_size;
            }

            for (j = 0; j < ls->count; j++) {
                const Label *lbl = &ls->items[j];
                if (!lbl->is_code) continue;
                if (lbl->addr < base_addr) continue;
                size_t off = lbl->addr - base_addr;
                if (off >= bsize) continue;

                /* Skip if already has an inline sig defined */
                if (has_inline_sig(&p->inline_sigs, 1, bank_id, lbl->addr))
                    continue;

                int fields[MAX_FIELDS];
                int nfields = 0;
                if (!scan_detect_inline_dispatcher(bdata + off, bsize - off,
                                               lbl->addr, fields, &nfields))
                    continue;

                char spec[APEX_INLINE_SPEC_MAX] = "";
                if (build_spec(fields, nfields, spec, sizeof(spec)) != 0)
                    continue;

                /* Score: base 40, +10 if >1 field, callsite factor */
                int score = 40 + (nfields > 1 ? 10 : 0);

                int cnt = 0, valid_cs = 0;
                /* Callsite search only reliable for system-bank functions */
                if (bank_id == 0xFFu)
                    count_callsites(p, lbl->addr, fields, nfields,
                                    valid_bank, &cnt, &valid_cs);

                if (cnt > 0) {
                    score += (cnt >= 3 ? 20 : 10);
                    if (valid_cs == cnt)          score += 25;
                    else if (valid_cs * 2 >= cnt) score += 12;
                }
                if (score > 100) score = 100;

                /* Require at least some confidence */
                if (cnt == 0 && score < 50) score = 45; /* show but mark uncertain */

                inline_push(out, bank_id, lbl->addr, spec, score, cnt, valid_cs);
            }
        }
    }

    if (out->count > 0)
        qsort(out->items, out->count, sizeof(out->items[0]), inline_cmp);
}

void apex_free_inline_candidates(ApexInlineCandidates *candidates)
{
    if (!candidates) return;
    free(candidates->items);
    candidates->items = NULL;
    candidates->count = 0;
    candidates->cap   = 0;
}
