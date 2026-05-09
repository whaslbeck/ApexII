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
                               const DataRanges *data_ranges, ReferenceSet *refs);
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
    } else {
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
        fputs(table_field_kind_name(schema->items[i].kind), out);
        if (schema->items[i].count != 1u) {
            fprintf(out, "[%lu]", (unsigned long)schema->items[i].count);
        }
    }
}

static void write_inline_signature_value(FILE *out, const InlineSignature *sig)
{
    if (sig->schema.count == 1u && sig->schema.items[0].count == 1u && sig->raw_param &&
        sig->schema.items[0].kind == TABLE_BYTE) {
        fprintf(out, "byte:%s", sig->raw_param);
        return;
    }
    if (sig->schema.count == 1u && sig->schema.items[0].count == 1u && sig->far_param) {
        fprintf(out, "%s:%s", table_field_kind_name(sig->schema.items[0].kind), sig->far_param);
        return;
    }
    write_table_schema_text(out, &sig->schema);
    if (sig->alias && *sig->alias) {
        fprintf(out, ", %s", sig->alias);
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
    case DATA_DMD_FULLFRAME:
        fputs("dmd_fullframe", out);
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
        free((char *)sigs->items[i].alias);
        free((char *)sigs->items[i].raw_param);
        free((char *)sigs->items[i].far_param);
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

ApexProject *apex_project_open(const char *rom_path, const char *config_path)
{
    ApexProject *project = xmalloc(sizeof(*project));

    memset(project, 0, sizeof(*project));
    project->rom_path = project_dup_string(rom_path);
    project->config_path = config_path ? project_dup_string(config_path) : NULL;
    project->options.labels_are_entries = 1;

    load_config(project->config_path, &project->inline_sigs, &project->config_labels,
                &project->config_entries, &project->tables, &project->schemas,
                &project->routine_docs, &project->table_docs, &project->symbols,
                &project->data_ranges, &project->options);
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
    free_config_labels(&project->config_labels);
    free_config_entries(&project->config_entries);
    free_table_defs(&project->tables);
    free_schema_defs(&project->schemas);
    free_config_docs(&project->routine_docs);
    free_config_docs(&project->table_docs);
    free_config_symbols(&project->symbols);
    free(project->data_ranges.items);
    if (project->render_cache) {
        apex_render_document_free(project->render_cache);
        free(project->render_cache);
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
    apply_data_range_labels(&system_data_ranges, project->rom.data, project->banks,
                            project->rom.data + project->paged_size, project->bank_labels,
                            &project->system_labels, &project->refs);
    apply_table_labels(&system_tables, project->rom.data, project->banks, project->bank_labels,
                       &project->system_labels, project->rom.data + project->paged_size,
                       &project->refs);
    collect_code_targets(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE,
                         APEX_SYSTEM_ORG, &project->system_labels, &project->inline_sigs,
                         project->rom.data, project->banks, project->bank_labels,
                         &project->system_labels, &project->data_ranges, 0xffu, &project->refs);
    collect_code_targets(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE,
                         APEX_SYSTEM_ORG, &project->system_labels, &project->inline_sigs,
                         project->rom.data, project->banks, project->bank_labels,
                         &project->system_labels, &project->data_ranges, 0xffu, &project->refs);
    apply_string_content_labels(&project->system_labels, project->rom.data + project->paged_size,
                                last_non_ff(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE),
                                APEX_SYSTEM_ORG);
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
                             bank_id, &project->refs);
    }
    apply_string_content_labels(&project->bank_labels[bank_index], bank, used, APEX_PAGED_ORG);
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
    collect_code_targets(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE,
                         APEX_SYSTEM_ORG, &project->system_labels, &project->inline_sigs,
                         project->rom.data, banks, project->bank_labels, &project->system_labels,
                         &project->data_ranges, 0xff, &project->refs);
    collect_bank_code_targets(project->rom.data, banks, &project->inline_sigs,
                              project->bank_labels, &project->system_labels,
                              &project->data_ranges, &project->refs);
    collect_code_targets(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE,
                         APEX_SYSTEM_ORG, &project->system_labels, &project->inline_sigs,
                         project->rom.data, banks, project->bank_labels, &project->system_labels,
                         &project->data_ranges, 0xff, &project->refs);
    for (i = 0; i < banks; i++) {
        const uint8_t *bank = project->rom.data + i * APEX_BANK_SIZE;

        apply_string_content_labels(&project->bank_labels[i], bank,
                                    last_non_ff(bank, APEX_BANK_SIZE), APEX_PAGED_ORG);
    }
    apply_string_content_labels(&project->system_labels, project->rom.data + project->paged_size,
                                last_non_ff(project->rom.data + project->paged_size, APEX_SYSTEM_SIZE),
                                APEX_SYSTEM_ORG);
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
    config_label = upsert_config_label(&project->config_labels, has_bank, bank, addr, name, &old_name);
    for (i = 0; i < labels->count; i++) {
        if (labels->items[i].addr == addr &&
            (old_name ? strcmp(labels->items[i].name, old_name) == 0
                      : !generated_any_label_name(labels->items[i].name))) {
            labels->items[i].name = config_label->name;
            updated = 1;
        }
    }
    if (!updated) {
        Label *label = add_label(labels, addr, config_label->name,
                                 code_label_at(addr, labels->items, labels->count));

        label->name = config_label->name;
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
    removed_name = remove_config_label(&project->config_labels, has_bank, bank, addr);
    if (!removed_name) {
        return 1;
    }
    remove_runtime_label(labels, addr, removed_name);
    free(removed_name);
    apex_project_invalidate(project, APEX_DIRTY_LABELS | APEX_DIRTY_RENDER);
    return 0;
}

int apex_project_set_doc(ApexProject *project, int is_table_doc, int has_bank, uint8_t bank,
                         uint32_t addr, const char *text)
{
    ConfigDocs *docs;

    if (!project || !text || !*text) {
        return 1;
    }
    docs = is_table_doc ? &project->table_docs : &project->routine_docs;
    upsert_config_doc(docs, has_bank, bank, addr, text);
    apex_project_invalidate(project, APEX_DIRTY_DOCS | APEX_DIRTY_RENDER);
    return 0;
}

int apex_project_clear_doc(ApexProject *project, int is_table_doc, int has_bank, uint8_t bank,
                           uint32_t addr)
{
    ConfigDocs *docs;

    if (!project) {
        return 1;
    }
    docs = is_table_doc ? &project->table_docs : &project->routine_docs;
    if (!remove_config_doc(docs, has_bank, bank, addr)) {
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
                                  effective_spec) != 0) {
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
    if (config_set_inline_spec(&project->inline_sigs, has_bank, bank, addr, spec) != 0) {
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
    if (config_set_table_spec(&project->tables, &project->schemas, bank, addr, spec) != 0) {
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

int apex_project_save_overlay(const ApexProject *project, const char *path)
{
    FILE *out;

    if (!project || !path || !*path) {
        return 1;
    }
    out = fopen(path, "w");
    if (!out) {
        return 1;
    }
    fputs("; Apex ImGui overlay\n", out);
    if (project->config_path && *project->config_path) {
        fprintf(out, "include = %s\n", basename_ptr(project->config_path));
    }
    write_labels_section(out, &project->config_labels);
    write_entries_section(out, &project->config_entries);
    write_inline_section(out, &project->inline_sigs);
    write_data_section(out, &project->data_ranges);
    write_tables_section(out, &project->tables);
    write_docs_section(out, "routine_docs", &project->routine_docs);
    write_docs_section(out, "table_docs", &project->table_docs);
    if (fclose(out) != 0) {
        return 1;
    }
    return 0;
}
