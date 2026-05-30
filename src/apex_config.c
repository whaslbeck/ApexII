#include "apex.h"
#include "apex_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int parse_table_field(char *value, TableField *field, const ConfigTypes *types);

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

static char *dup_string(const char *s)
{
    size_t len = strlen(s) + 1u;
    char *copy = xmalloc(len);

    memcpy(copy, s, len);
    return copy;
}

static void strip_config_comment(char *line)
{
    int in_quote = 0;
    int escaped = 0;
    char *p;

    for (p = line; *p; p++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (*p == '\\') {
            escaped = 1;
            continue;
        }
        if (*p == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && (*p == ';' || *p == '#')) {
            *p = '\0';
            return;
        }
    }
}

static char *dup_config_value(const char *s)
{
    char *raw = dup_string(s);
    char *trimmed = trim(raw);
    size_t len = strlen(trimmed);
    char *copy;
    size_t in;
    size_t out = 0;

    if (len >= 2u && trimmed[0] == '"' && trimmed[len - 1u] == '"') {
        trimmed[len - 1u] = '\0';
        trimmed++;
        len -= 2u;
    }
    copy = xmalloc(len + 1u);
    for (in = 0; in < len; in++) {
        if (trimmed[in] == '\\' && in + 1u < len) {
            in++;
            switch (trimmed[in]) {
            case 'n':
                copy[out++] = '\n';
                break;
            case ';':
            case '#':
            case '\\':
            case '"':
                copy[out++] = trimmed[in];
                break;
            default:
                copy[out++] = trimmed[in];
                break;
            }
        } else {
            copy[out++] = trimmed[in];
        }
    }
    copy[out] = '\0';
    free(raw);
    return copy;
}

static int parse_config_bool(const char *value, int *out)
{
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
        strcmp(value, "on") == 0) {
        *out = 1;
        return 1;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0 ||
        strcmp(value, "off") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static void copy_inline_schema(TableSchema *dst, const TableSchema *src)
{
    size_t i;

    memset(dst, 0, sizeof(*dst));
    for (i = 0; i < src->count; i++) {
        add_table_field(dst, src->items[i].kind, src->items[i].count);
        dst->items[dst->count - 1u].type_name = src->items[i].type_name;
    }
}

static void add_config_doc(ConfigDocs *docs, int has_bank, uint8_t bank, uint32_t addr,
                           const char *text)
{
    size_t i;

    if (!text || !*text) {
        return;
    }
    for (i = 0; i < docs->count; i++) {
        if (docs->items[i].has_bank == has_bank && docs->items[i].bank == bank &&
            docs->items[i].addr == addr) {
            free(docs->items[i].text);
            docs->items[i].text = dup_string(text);
            return;
        }
    }
    if (docs->count == docs->cap) {
        size_t new_cap = docs->cap == 0 ? 16 : docs->cap * 2;
        ConfigDoc *new_items = realloc(docs->items, new_cap * sizeof(docs->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        docs->items = new_items;
        docs->cap = new_cap;
    }
    docs->items[docs->count].has_bank = has_bank;
    docs->items[docs->count].bank = bank;
    docs->items[docs->count].addr = addr;
    docs->items[docs->count].text = dup_string(text);
    docs->count++;
}

static int valid_symbol_name(const char *s)
{
    if (!isalpha((unsigned char)*s) && *s != '_') {
        return 0;
    }
    for (s++; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_') {
            return 0;
        }
    }
    return 1;
}

static int reserved_symbol_name(const char *s)
{
    static const char *reserved[] = {
        "BANK_ID", "FILL_TO_BANK_END", "INLINE_BYTE", "INLINE_WORD", "INLINE_PTR",
        "INLINE_STRING_PTR", "INLINE_CODE_PTR", "INLINE_TABLE_PTR", "INLINE_FAR_CODE",
        "INLINE_FAR_STRING", "INLINE_FAR_PTR", "INLINE_FAR_TABLE",
        "INLINE_PTR_DMD_FULLFRAME", "INLINE_FAR_DMD_FULLFRAME",
        "TABLE_PTR", "TABLE_FAR_CODE", "TABLE_FAR_STRING", "TABLE_FAR_PTR",
        "TABLE_FAR_TABLE", "TABLE_PTR_DMD_FULLFRAME", "TABLE_FAR_DMD_FULLFRAME",
        "FAR_CODE", "FAR_STRING", "FAR_PTR", "FAR_TABLE", "FAR_DMD_FULLFRAME",
        "STRING", "LDA", "LDB", "LDD", "LDX", "LDY", "LDU", "LDS", "STA", "STB",
        "STD", "STX", "STY", "STU", "STS", "JSR", "JMP", "RTS", "RTI", "BRA",
        "BSR", "PULS", "PULU", "PSHS", "PSHU", NULL
    };
    size_t i;

    for (i = 0; reserved[i]; i++) {
        if (strcmp(s, reserved[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_config_symbol(ConfigSymbols *symbols, const char *name, uint32_t value)
{
    size_t i;

    if (!name || !*name) {
        return;
    }
    if (!valid_symbol_name(name)) {
        die("invalid symbol name '%s'", name);
    }
    if (reserved_symbol_name(name)) {
        die("symbol name '%s' collides with assembler syntax", name);
    }
    if (value > 0xffffu) {
        die("symbol '%s' value out of range", name);
    }
    for (i = 0; i < symbols->count; i++) {
        if (strcmp(symbols->items[i].name, name) == 0) {
            if (symbols->items[i].value != value) {
                die("symbol '%s' is defined more than once", name);
            }
            return;
        }
    }
    if (symbols->count == symbols->cap) {
        size_t new_cap = symbols->cap == 0 ? 128 : symbols->cap * 2;
        ConfigSymbol *new_items = realloc(symbols->items, new_cap * sizeof(symbols->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        symbols->items = new_items;
        symbols->cap = new_cap;
    }
    symbols->items[symbols->count].name = dup_string(name);
    symbols->items[symbols->count].value = value;
    symbols->count++;
}

int config_valid_symbol_name(const char *name)
{
    return name && *name && valid_symbol_name(name) && !reserved_symbol_name(name);
}

int config_set_symbol(ConfigSymbols *symbols, const char *name, uint32_t value)
{
    size_t i;
    ConfigSymbol *new_items;
    size_t new_cap;

    if (!symbols || !config_valid_symbol_name(name) || value > 0xffffu) {
        return 1;
    }
    for (i = 0; i < symbols->count; i++) {
        if (strcmp(symbols->items[i].name, name) == 0) {
            symbols->items[i].value = value;
            return 0;
        }
    }
    if (symbols->count == symbols->cap) {
        new_cap  = symbols->cap == 0 ? 8 : symbols->cap * 2;
        new_items = (ConfigSymbol *)realloc(symbols->items,
                                            new_cap * sizeof(symbols->items[0]));
        if (!new_items) return 1;
        symbols->items = new_items;
        symbols->cap   = new_cap;
    }
    symbols->items[symbols->count].name  = dup_string(name);
    symbols->items[symbols->count].value = value;
    symbols->count++;
    return 0;
}

int config_clear_symbol(ConfigSymbols *symbols, const char *name)
{
    size_t i;
    if (!symbols || !name) return 1;
    for (i = 0; i < symbols->count; i++) {
        if (strcmp(symbols->items[i].name, name) == 0) {
            free((void *)symbols->items[i].name);
            memmove(&symbols->items[i], &symbols->items[i + 1],
                    (symbols->count - i - 1) * sizeof(symbols->items[0]));
            symbols->count--;
            return 0;
        }
    }
    return 1;
}

static void add_config_label(ConfigLabels *labels, int has_bank, uint8_t bank, uint32_t addr,
                             const char *name)
{
    size_t i;

    if (!name || !*name) {
        return;
    }
    if (!valid_symbol_name(name)) {
        die("invalid label name '%s'", name);
    }
    if (reserved_symbol_name(name)) {
        die("label name '%s' collides with assembler syntax", name);
    }
    for (i = 0; i < labels->count; i++) {
        if (strcmp(labels->items[i].name, name) == 0 &&
            (labels->items[i].has_bank != has_bank || labels->items[i].bank != bank ||
             labels->items[i].addr != addr)) {
            die("label '%s' is defined at more than one address", name);
        }
        if (labels->items[i].has_bank == has_bank && labels->items[i].bank == bank &&
            labels->items[i].addr == addr) {
            free(labels->items[i].name);
            labels->items[i].name = dup_string(name);
            return;
        }
    }
    if (labels->count == labels->cap) {
        size_t new_cap = labels->cap == 0 ? 16 : labels->cap * 2;
        ConfigLabel *new_items = realloc(labels->items, new_cap * sizeof(labels->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        labels->items = new_items;
        labels->cap = new_cap;
    }
    labels->items[labels->count].has_bank = has_bank;
    labels->items[labels->count].bank = bank;
    labels->items[labels->count].addr = addr;
    labels->items[labels->count].name = dup_string(name);
    labels->count++;
}

static void add_config_entry(ConfigEntries *entries, int has_bank, uint8_t bank, uint32_t addr)
{
    size_t i;

    for (i = 0; i < entries->count; i++) {
        if (config_addr_matches(entries->items[i].has_bank, entries->items[i].bank,
                                entries->items[i].addr, has_bank, bank, addr)) {
            return;
        }
    }
    if (entries->count == entries->cap) {
        size_t new_cap = entries->cap == 0 ? 16 : entries->cap * 2;
        ConfigEntry *new_items = realloc(entries->items, new_cap * sizeof(entries->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        entries->items = new_items;
        entries->cap = new_cap;
    }
    entries->items[entries->count].has_bank = has_bank;
    entries->items[entries->count].bank = bank;
    entries->items[entries->count].addr = addr;
    entries->count++;
}

static int remove_config_entry(ConfigEntries *entries, int has_bank, uint8_t bank, uint32_t addr)
{
    size_t i;

    for (i = 0; i < entries->count; i++) {
        if (config_addr_matches(entries->items[i].has_bank, entries->items[i].bank,
                                entries->items[i].addr, has_bank, bank, addr)) {
            memmove(&entries->items[i], &entries->items[i + 1],
                    (entries->count - i - 1u) * sizeof(entries->items[0]));
            entries->count--;
            return 1;
        }
    }
    return 0;
}

void add_table_field(TableSchema *schema, TableFieldKind kind, size_t count)
{
    if (count == 0) {
        return;
    }
    if (schema->count == schema->cap) {
        size_t new_cap = schema->cap == 0 ? 4 : schema->cap * 2;
        TableField *new_items = realloc(schema->items, new_cap * sizeof(schema->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        schema->items = new_items;
        schema->cap = new_cap;
    }
    schema->items[schema->count].kind = kind;
    schema->items[schema->count].count = count;
    schema->items[schema->count].type_name = NULL;
    schema->count++;
}

static TableSchema copy_table_schema(const TableSchema *src)
{
    TableSchema copy;
    size_t i;

    memset(&copy, 0, sizeof(copy));
    for (i = 0; i < src->count; i++) {
        add_table_field(&copy, src->items[i].kind, src->items[i].count);
        copy.items[copy.count - 1u].type_name = src->items[i].type_name;
    }
    return copy;
}

static void add_schema_def(SchemaDefs *schemas, const char *name, TableSchema schema)
{
    size_t i;

    if (!valid_symbol_name(name)) {
        die("invalid schema name '%s'", name);
    }
    for (i = 0; i < schemas->count; i++) {
        if (strcmp(schemas->items[i].name, name) == 0) {
            free(schemas->items[i].schema.items);
            schemas->items[i].schema = schema;
            return;
        }
    }
    if (schemas->count == schemas->cap) {
        size_t new_cap = schemas->cap == 0 ? 16 : schemas->cap * 2;
        SchemaDef *new_items = realloc(schemas->items, new_cap * sizeof(schemas->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        schemas->items = new_items;
        schemas->cap = new_cap;
    }
    schemas->items[schemas->count].name = dup_string(name);
    schemas->items[schemas->count].schema = schema;
    schemas->count++;
}

static int schema_def_named(const SchemaDefs *schemas, const char *name, TableSchema *schema)
{
    size_t i;

    for (i = 0; i < schemas->count; i++) {
        if (strcmp(schemas->items[i].name, name) == 0) {
            *schema = copy_table_schema(&schemas->items[i].schema);
            return 1;
        }
    }
    return 0;
}

static void add_table_def(TableDefs *tables, uint8_t bank, uint32_t addr, TableSchema schema,
                          int has_header, size_t rows)
{
    size_t i;

    for (i = 0; i < tables->count; i++) {
        if (tables->items[i].bank == bank && tables->items[i].addr == addr) {
            free(tables->items[i].schema.items);
            tables->items[i].schema = schema;
            tables->items[i].has_header = has_header;
            tables->items[i].rows = rows;
            return;
        }
    }
    if (tables->count == tables->cap) {
        size_t new_cap = tables->cap == 0 ? 16 : tables->cap * 2;
        TableDef *new_items = realloc(tables->items, new_cap * sizeof(tables->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        tables->items = new_items;
        tables->cap = new_cap;
    }
    tables->items[tables->count].bank = bank;
    tables->items[tables->count].addr = addr;
    tables->items[tables->count].schema = schema;
    tables->items[tables->count].has_header = has_header;
    tables->items[tables->count].rows = rows;
    tables->count++;
}

static int remove_table_def(TableDefs *tables, uint8_t bank, uint32_t addr)
{
    size_t i;

    for (i = 0; i < tables->count; i++) {
        if (tables->items[i].bank == bank && tables->items[i].addr == addr) {
            free(tables->items[i].schema.items);
            memmove(&tables->items[i], &tables->items[i + 1],
                    (tables->count - i - 1u) * sizeof(tables->items[0]));
            tables->count--;
            return 1;
        }
    }
    return 0;
}

void add_data_range(DataRanges *ranges, uint8_t bank, uint32_t addr, DataKind kind,
                    size_t length)
{
    size_t i;

    if (kind == DATA_BYTES && length == 0) {
        return;
    }
    for (i = 0; i < ranges->count; i++) {
        if (ranges->items[i].bank == bank && ranges->items[i].addr == addr) {
            ranges->items[i].kind = kind;
            ranges->items[i].length = length;
            return;
        }
    }
    if (ranges->count == ranges->cap) {
        size_t new_cap = ranges->cap == 0 ? 16 : ranges->cap * 2;
        DataRange *new_items = realloc(ranges->items, new_cap * sizeof(ranges->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        ranges->items = new_items;
        ranges->cap = new_cap;
    }
    ranges->items[ranges->count].bank = bank;
    ranges->items[ranges->count].addr = addr;
    ranges->items[ranges->count].kind = kind;
    ranges->items[ranges->count].length = length;
    ranges->count++;
}

static int remove_data_range(DataRanges *ranges, uint8_t bank, uint32_t addr)
{
    size_t i;

    for (i = 0; i < ranges->count; i++) {
        if (ranges->items[i].bank == bank && ranges->items[i].addr == addr) {
            memmove(&ranges->items[i], &ranges->items[i + 1],
                    (ranges->count - i - 1u) * sizeof(ranges->items[0]));
            ranges->count--;
            return 1;
        }
    }
    return 0;
}

static int parse_bank_label_ref(const char *text, uint8_t *bank, uint32_t *addr)
{
    char bank_text[5];
    char addr_text[7];
    uint32_t bank_value;

    if (strlen(text) != 9 || text[0] != 'B' || text[3] != '_' || text[4] != 'A') {
        return 0;
    }
    bank_text[0] = '0';
    bank_text[1] = 'x';
    bank_text[2] = text[1];
    bank_text[3] = text[2];
    bank_text[4] = '\0';
    addr_text[0] = '0';
    addr_text[1] = 'x';
    memcpy(addr_text + 2, text + 5, 4);
    addr_text[6] = '\0';
    if (!parse_u32(bank_text, &bank_value) || bank_value > 0xffu ||
        !parse_u32(addr_text, addr)) {
        return 0;
    }
    *bank = (uint8_t)bank_value;
    return 1;
}

void add_inline_signature_schema(InlineSignatures *sigs, int has_bank, uint8_t bank,
                                 uint32_t addr, const TableSchema *schema)
{
    size_t i;
    unsigned length = (unsigned)table_schema_width(schema);

    if (length == 0 || length > 255u) {
        return;
    }
    for (i = 0; i < sigs->count; i++) {
        if (config_addr_matches(sigs->items[i].has_bank, sigs->items[i].bank,
                                sigs->items[i].addr, has_bank, bank, addr)) {
            sigs->items[i].length = length;
            free(sigs->items[i].schema.items);
            copy_inline_schema(&sigs->items[i].schema, schema);
            return;
        }
    }
    if (sigs->count == sigs->cap) {
        size_t new_cap = sigs->cap == 0 ? 8 : sigs->cap * 2;
        InlineSignature *new_items = realloc(sigs->items, new_cap * sizeof(sigs->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        sigs->items = new_items;
        sigs->cap = new_cap;
    }
    sigs->items[sigs->count].has_bank = has_bank;
    sigs->items[sigs->count].bank = bank;
    sigs->items[sigs->count].addr = addr;
    sigs->items[sigs->count].length = length;
    copy_inline_schema(&sigs->items[sigs->count].schema, schema);
    sigs->count++;
}

void add_inline_signature_ex(InlineSignatures *sigs, int has_bank, uint8_t bank, uint32_t addr,
                             unsigned length, TableFieldKind kind)
{
    TableSchema schema;

    memset(&schema, 0, sizeof(schema));
    add_table_field(&schema, kind, length);
    add_inline_signature_schema(sigs, has_bank, bank, addr, &schema);
    free(schema.items);
}

static int remove_inline_signature(InlineSignatures *sigs, int has_bank, uint8_t bank, uint32_t addr)
{
    size_t i;

    for (i = 0; i < sigs->count; i++) {
        if (config_addr_matches(sigs->items[i].has_bank, sigs->items[i].bank,
                                sigs->items[i].addr, has_bank, bank, addr)) {
            free(sigs->items[i].schema.items);
            memmove(&sigs->items[i], &sigs->items[i + 1],
                    (sigs->count - i - 1u) * sizeof(sigs->items[0]));
            sigs->count--;
            return 1;
        }
    }
    return 0;
}

static int parse_table_kind(char *value, TableFieldKind *kind)
{
    value = trim(value);
    if (strcmp(value, "byte") == 0) {
        *kind = TABLE_BYTE;
    } else if (strcmp(value, "word") == 0) {
        *kind = TABLE_WORD;
    } else if (strcmp(value, "ptr16_string") == 0) {
        *kind = TABLE_PTR16_STRING;
    } else if (strcmp(value, "ptr16_data") == 0) {
        *kind = TABLE_PTR16_DATA;
    } else if (strcmp(value, "ptr16_code") == 0) {
        *kind = TABLE_PTR16_CODE;
    } else if (strcmp(value, "ptr16_table") == 0) {
        *kind = TABLE_PTR16_TABLE;
    } else if (strcmp(value, "ptr16_dmd_fullframe") == 0) {
        *kind = TABLE_PTR16_DMD_FULLFRAME;
    } else if (strcmp(value, "ptr16_sprite") == 0) {
        *kind = TABLE_PTR16_SPRITE;
    } else if (strcmp(value, "far_string") == 0) {
        *kind = TABLE_FAR_STRING;
    } else if (strcmp(value, "far_data") == 0) {
        *kind = TABLE_FAR_DATA;
    } else if (strcmp(value, "far_table") == 0) {
        *kind = TABLE_FAR_TABLE;
    } else if (strcmp(value, "far_code") == 0) {
        *kind = TABLE_FAR_CODE;
    } else if (strcmp(value, "far_dmd_fullframe") == 0) {
        *kind = TABLE_FAR_DMD_FULLFRAME;
    } else if (strcmp(value, "far_sprite") == 0) {
        *kind = TABLE_FAR_SPRITE;
    } else {
        return 0;
    }
    return 1;
}

static int parse_table_field(char *value, TableField *field, const ConfigTypes *types)
{
    char *open;
    char *close;
    uint32_t count;
    size_t i;

    value = trim(value);
    open = strchr(value, '[');
    if (open) {
        close = strchr(open, ']');
        if (!close || *trim(close + 1) != '\0') {
            die("invalid table field '%s'", value);
        }
        *open = '\0';
        *close = '\0';
        if (!parse_u32(open + 1, &count) || count == 0) {
            die("invalid table field count '%s'", open + 1);
        }
    } else {
        count = 1;
    }
    field->type_name = NULL;
    if (parse_table_kind(value, &field->kind)) {
        field->count = count;
        return 1;
    }
    if (types) {
        for (i = 0; i < types->count; i++) {
            if (strcmp(types->items[i].name, value) == 0) {
                field->kind = types->items[i].kind;
                field->count = count;
                field->type_name = types->items[i].name;
                return 1;
            }
        }
    }
    return 0;
}

static int parse_table_schema(char *value, TableSchema *schema, const ConfigTypes *types)
{
    char *p = value;

    free(schema->items);
    memset(schema, 0, sizeof(*schema));
    while (p && *p) {
        char *comma = strchr(p, ',');
        TableField field;

        if (comma) {
            *comma = '\0';
        }
        p = trim(p);
        if (*p == '\0' || !parse_table_field(p, &field, types)) {
            return 0;
        }
        add_table_field(schema, field.kind, field.count);
        schema->items[schema->count - 1u].type_name = field.type_name;
        p = comma ? comma + 1 : NULL;
    }
    return schema->count > 0;
}

static int parse_wrapped_table_format(char *value, const char *prefix, char **inner)
{
    size_t prefix_len = strlen(prefix);
    char *close;

    value = trim(value);
    if (strncmp(value, prefix, prefix_len) != 0 || value[prefix_len] != '(') {
        return 0;
    }
    close = strrchr(value + prefix_len + 1u, ')');
    if (!close || *trim(close + 1) != '\0') {
        die("invalid table format '%s'", value);
    }
    *close = '\0';
    *inner = trim(value + prefix_len + 1u);
    return 1;
}

static int parse_far_code_table_format(char *value, size_t *rows)
{
    char *open;
    char *close;
    uint32_t parsed_rows;

    value = trim(value);
    if (strncmp(value, "far_code[", 9) != 0) {
        return 0;
    }
    open = value + 8;
    close = strchr(open, ']');
    if (!close || *trim(close + 1) != '\0') {
        die("invalid far_code table format '%s'", value);
    }
    *close = '\0';
    if (!parse_u32(open + 1, &parsed_rows) || parsed_rows == 0) {
        die("invalid far_code table row count '%s'", open + 1);
    }
    *rows = parsed_rows;
    return 1;
}

static int parse_rows_table_format(char *value, size_t *rows, TableSchema *schema,
                                   const SchemaDefs *schemas, const ConfigTypes *types)
{
    char *open;
    char *close;
    char *inner;
    uint32_t parsed_rows;

    value = trim(value);
    if (strncmp(value, "rows[", 5) != 0) {
        return 0;
    }
    open = value + 4;
    close = strchr(open, ']');
    if (!close || close[1] != '(') {
        die("invalid rows table format '%s'", value);
    }
    *close = '\0';
    if (!parse_u32(open + 1, &parsed_rows) || parsed_rows == 0) {
        die("invalid rows table row count '%s'", open + 1);
    }
    inner = close + 2;
    close = strrchr(inner, ')');
    if (!close || *trim(close + 1) != '\0') {
        die("invalid rows table format '%s'", value);
    }
    *close = '\0';
    if (!parse_table_schema(inner, schema, types) && !schema_def_named(schemas, inner, schema)) {
        die("invalid rows table row format '%s'", inner);
    }
    *rows = parsed_rows;
    return 1;
}

static int parse_count_format(char *value, const char *prefix, size_t *count)
{
    size_t prefix_len = strlen(prefix);
    char *open;
    char *close;
    uint32_t parsed_count;

    value = trim(value);
    if (strncmp(value, prefix, prefix_len) != 0 || value[prefix_len] != '[') {
        return 0;
    }
    open = value + prefix_len;
    close = strchr(open, ']');
    if (!close || *trim(close + 1) != '\0') {
        die("invalid %s count format '%s'", prefix, value);
    }
    *close = '\0';
    if (!parse_u32(open + 1, &parsed_count) || parsed_count == 0) {
        die("invalid %s count '%s'", prefix, open + 1);
    }
    *count = parsed_count;
    return 1;
}

static char *resolve_include_path(const char *from_path, const char *include_path)
{
    const char *slash;
    size_t dir_len;
    char *resolved;

    if (include_path[0] == '/') {
        return dup_string(include_path);
    }
    slash = strrchr(from_path, '/');
    if (!slash) {
        return dup_string(include_path);
    }
    dir_len = (size_t)(slash - from_path + 1);
    resolved = xmalloc(dir_len + strlen(include_path) + 1u);
    memcpy(resolved, from_path, dir_len);
    strcpy(resolved + dir_len, include_path);
    return resolved;
}

int table_kind_is_far(TableFieldKind kind)
{
    return kind == TABLE_FAR_STRING || kind == TABLE_FAR_DATA || kind == TABLE_FAR_TABLE ||
           kind == TABLE_FAR_CODE || kind == TABLE_FAR_DMD_FULLFRAME || kind == TABLE_FAR_SPRITE;
}

static size_t table_field_width(TableFieldKind kind)
{
    return kind == TABLE_BYTE ? 1u : table_kind_is_far(kind) ? 3u : 2u;
}

size_t table_schema_width(const TableSchema *schema)
{
    size_t i;
    size_t width = 0;

    for (i = 0; i < schema->count; i++) {
        width += table_field_width(schema->items[i].kind) * schema->items[i].count;
    }
    return width;
}

void free_config_types(ConfigTypes *types)
{
    size_t i, j;

    if (!types) {
        return;
    }
    for (i = 0; i < types->count; i++) {
        free(types->items[i].name);
        for (j = 0; j < types->items[i].value_count; j++) {
            free((char *)types->items[i].values[j].name);
        }
        free(types->items[i].values);
    }
    free(types->items);
    memset(types, 0, sizeof(*types));
}

const ConfigType *find_config_type(const ConfigTypes *types, const char *name)
{
    size_t i;

    if (!types || !name) {
        return NULL;
    }
    for (i = 0; i < types->count; i++) {
        if (strcmp(types->items[i].name, name) == 0) {
            return &types->items[i];
        }
    }
    return NULL;
}

const char *config_type_enum_name(const ConfigTypes *types, const char *type_name, uint32_t value)
{
    const ConfigType *type = find_config_type(types, type_name);
    size_t i;

    if (!type) {
        return NULL;
    }
    for (i = 0; i < type->value_count; i++) {
        if (type->values[i].value == value) {
            return type->values[i].name;
        }
    }
    return NULL;
}

void config_set_type(ConfigTypes *types, const char *name, TableFieldKind kind,
                     const char *values_str)
{
    ConfigType *type;
    char *values_copy;
    char *p;
    size_t i;

    if (!types) {
        return;
    }
    for (i = 0; i < types->count; i++) {
        if (strcmp(types->items[i].name, name) == 0) {
            size_t j;

            for (j = 0; j < types->items[i].value_count; j++) {
                free((char *)types->items[i].values[j].name);
            }
            types->items[i].value_count = 0;
            type = &types->items[i];
            goto parse_values;
        }
    }
    if (types->count == types->cap) {
        size_t new_cap = types->cap == 0 ? 8 : types->cap * 2;
        ConfigType *new_items = realloc(types->items, new_cap * sizeof(types->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        types->items = new_items;
        types->cap = new_cap;
    }
    types->items[types->count].name = dup_string(name);
    types->items[types->count].kind = kind;
    types->items[types->count].values = NULL;
    types->items[types->count].value_count = 0;
    types->items[types->count].value_cap = 0;
    type = &types->items[types->count];
    types->count++;

parse_values:
    values_copy = dup_string(values_str);
    p = values_copy;
    while (p && *p) {
        char *comma = strchr(p, ',');
        char *colon;
        char *name_part;
        uint32_t val;

        if (comma) {
            *comma = '\0';
        }
        p = trim(p);
        colon = strchr(p, ':');
        if (!colon) {
            die("invalid type value '%s': expected numeric:name", p);
        }
        *colon = '\0';
        name_part = trim(colon + 1);
        if (!parse_u32(trim(p), &val)) {
            die("invalid type value number '%s'", p);
        }
        if (!valid_symbol_name(name_part)) {
            die("invalid type value name '%s'", name_part);
        }
        if (type->value_count == type->value_cap) {
            size_t new_cap = type->value_cap == 0 ? 4 : type->value_cap * 2;
            ConfigTypeValue *new_vals = realloc(type->values, new_cap * sizeof(type->values[0]));

            if (!new_vals) {
                die("out of memory");
            }
            type->values = new_vals;
            type->value_cap = new_cap;
        }
        type->values[type->value_count].name = dup_string(name_part);
        type->values[type->value_count].value = val;
        type->value_count++;
        p = comma ? comma + 1 : NULL;
    }
    free(values_copy);
}

int config_remove_type(ConfigTypes *types, const char *name)
{
    size_t i, j;

    if (!types || !name) {
        return 0;
    }
    for (i = 0; i < types->count; i++) {
        if (strcmp(types->items[i].name, name) != 0) {
            continue;
        }
        free(types->items[i].name);
        for (j = 0; j < types->items[i].value_count; j++) {
            free((char *)types->items[i].values[j].name);
        }
        free(types->items[i].values);
        for (j = i + 1; j < types->count; j++) {
            types->items[j - 1] = types->items[j];
        }
        types->count--;
        return 1;
    }
    return 0;
}

void load_config(const char *path, InlineSignatures *sigs, ConfigLabels *labels,
                 ConfigEntries *entries, TableDefs *tables, SchemaDefs *schemas,
                 ConfigDocs *routine_docs, ConfigDocs *table_docs, ConfigSymbols *symbols,
                 DataRanges *data_ranges, ConfigOptions *options, ConfigTypes *types,
                 ConfigEntries *ref_exclusions)
{
    FILE *f;
    char line[8192];
    int in_options = 0;
    int in_inline = 0;
    int in_labels = 0;
    int in_entries = 0;
    int in_schemas = 0;
    int in_tables = 0;
    int in_routine_docs = 0;
    int in_table_docs = 0;
    int in_symbols = 0;
    int in_data = 0;
    int in_types = 0;
    int in_exclude_refs = 0;
    /* pending multi-line [types] entry */
    char pt_name[256];
    TableFieldKind pt_kind;
    char pt_vals[8192];
    pt_name[0] = '\0';
    pt_kind = TABLE_BYTE;
    pt_vals[0] = '\0';

    if (!path) {
        return;
    }
    f = fopen(path, "r");
    if (!f) {
        die("failed to open config %s", path);
    }
    while (fgets(line, sizeof(line), f)) {
        char *s;
        char *eq;

        strip_config_comment(line);
        /* continuation line: leading whitespace in [types] appends to pending entry */
        if (in_types && pt_name[0] &&
            ((unsigned char)line[0] == '\t' || (unsigned char)line[0] == ' ')) {
            char *cv = trim(line);
            if (*cv) {
                size_t vl = strlen(pt_vals);
                if (vl) { strncat(pt_vals, ", ", sizeof(pt_vals) - vl - 1); vl += 2; }
                strncat(pt_vals, cv, sizeof(pt_vals) - strlen(pt_vals) - 1);
            }
            continue;
        }
        s = trim(line);
        if (*s == '\0') {
            continue;
        }
        if (*s == '[') {
            char *end = strchr(s, ']');

            if (!end) {
                die("invalid config section '%s'", s);
            }
            /* flush any pending multi-line type before leaving [types] */
            if (pt_name[0]) {
                config_set_type(types, pt_name, pt_kind, pt_vals);
                pt_name[0] = '\0'; pt_vals[0] = '\0';
            }
            *end = '\0';
            in_options = strcmp(s + 1, "options") == 0;
            in_inline = strcmp(s + 1, "inline") == 0;
            in_labels = strcmp(s + 1, "labels") == 0;
            in_entries = strcmp(s + 1, "entries") == 0;
            in_schemas = strcmp(s + 1, "schemas") == 0;
            in_tables = strcmp(s + 1, "tables") == 0;
            in_routine_docs = strcmp(s + 1, "routine_docs") == 0;
            in_table_docs = strcmp(s + 1, "table_docs") == 0;
            in_symbols = strcmp(s + 1, "symbols") == 0;
            in_data = strcmp(s + 1, "data") == 0;
            in_types = strcmp(s + 1, "types") == 0;
            in_exclude_refs = strcmp(s + 1, "exclude_refs") == 0;
            continue;
        }
        eq = strchr(s, '=');
        if (!eq) {
            die("invalid config line '%s'", s);
        }
        *eq = '\0';
        s = trim(s);
        if (strcmp(s, "include") == 0) {
            char *value = dup_config_value(eq + 1);
            char *include_path = resolve_include_path(path, value);

            load_config(include_path, sigs, labels, entries, tables, schemas, routine_docs,
                        table_docs, symbols, data_ranges, options, types, ref_exclusions);
            free(value);
            free(include_path);
            continue;
        }
        if (in_options) {
            char *key = s;
            char *value = dup_config_value(eq + 1);

            if (strcmp(key, "labels_are_entries") == 0) {
                if (!parse_config_bool(value, &options->labels_are_entries)) {
                    die("invalid option '%s = %s'", key, value);
                }
            } else {
                die("unknown option '%s'", key);
            }
            free(value);
        } else if (in_inline) {
            uint32_t addr;
            uint8_t bank = 0;
            int has_bank = 0;
            char *key = s;
            char *value = dup_config_value(eq + 1);
            TableSchema schema;

            if (parse_bank_label_ref(key, &bank, &addr)) {
                has_bank = 1;
            } else if (!parse_u32(key, &addr)) {
                die("invalid inline signature '%s = %s'", key, value);
            }
            memset(&schema, 0, sizeof(schema));
            if (parse_table_schema(value, &schema, types)) {
                add_inline_signature_schema(sigs, has_bank, bank, addr, &schema);
                free(schema.items);
            } else {
                die("invalid inline signature '%s = %s'", key, value);
            }
            free(value);
        } else if (in_labels) {
            uint32_t addr;
            uint8_t bank = 0;
            int has_bank = 0;
            char *key = s;
            char *value = dup_config_value(eq + 1);

            if (parse_bank_label_ref(key, &bank, &addr)) {
                has_bank = 1;
            } else if (!parse_u32(key, &addr)) {
                die("invalid label config '%s = %s'", key, value);
            }
            add_config_label(labels, has_bank, bank, addr, value);
            free(value);
        } else if (in_entries) {
            uint32_t addr;
            uint8_t bank = 0;
            int has_bank = 0;
            char *key = s;
            char *value = dup_config_value(eq + 1);

            if (parse_bank_label_ref(key, &bank, &addr)) {
                has_bank = 1;
            } else if (!parse_u32(key, &addr)) {
                die("invalid entry config '%s = %s'", key, value);
            }
            if (*value != '\0' && strcmp(value, "code") != 0 && strcmp(value, "entry") != 0) {
                die("invalid entry config '%s = %s'", key, value);
            }
            add_config_entry(entries, has_bank, bank, addr);
            free(value);
        } else if (in_schemas) {
            TableSchema schema = {0};
            char *key = s;
            char *value = dup_config_value(eq + 1);

            if (!parse_table_schema(value, &schema, types)) {
                die("invalid schema config '%s = %s'", key, value);
            }
            add_schema_def(schemas, key, schema);
            free(value);
        } else if (in_routine_docs || in_table_docs) {
            uint32_t addr;
            uint8_t bank = 0;
            int has_bank = 0;
            char *key = s;
            char *value = dup_config_value(eq + 1);

            if (parse_bank_label_ref(key, &bank, &addr)) {
                has_bank = 1;
            } else if (!parse_u32(key, &addr)) {
                die("invalid doc config '%s = %s'", key, value);
            }
            if (in_routine_docs) {
                add_config_doc(routine_docs, has_bank, bank, addr, value);
            } else {
                add_config_doc(table_docs, has_bank, bank, addr, value);
            }
            free(value);
        } else if (in_symbols) {
            uint32_t value;
            char *key = s;
            char *value_text = dup_config_value(eq + 1);

            if (!parse_u32(value_text, &value)) {
                die("invalid symbol config '%s = %s'", key, value_text);
            }
            add_config_symbol(symbols, key, value);
            free(value_text);
        } else if (in_data) {
            uint32_t addr;
            uint8_t bank;
            char *key = s;
            char *value = dup_config_value(eq + 1);
            size_t length;

            if (!parse_bank_label_ref(key, &bank, &addr)) {
                die("invalid data config key '%s'", key);
            }
            if (parse_count_format(value, "bytes", &length)) {
                add_data_range(data_ranges, bank, addr, DATA_BYTES, length);
            } else if (strcmp(value, "string") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_STRING, 0);
            } else if (strcmp(value, "string_lp") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_STRING_LP, 0);
            } else if (parse_count_format(value, "string", &length)) {
                add_data_range(data_ranges, bank, addr, DATA_STRING_FIXED, length);
            } else if (strcmp(value, "dmd_fullframe") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_DMD_FULLFRAME, 0);
            } else if (strcmp(value, "ptr16_string") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_PTR16_STRING, 2);
            } else if (strcmp(value, "ptr16_data") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_PTR16_DATA, 2);
            } else if (strcmp(value, "ptr16_code") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_PTR16_CODE, 2);
            } else if (strcmp(value, "ptr16_table") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_PTR16_TABLE, 2);
            } else if (strcmp(value, "far_string") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_FAR_STRING, 3);
            } else if (strcmp(value, "far_data") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_FAR_DATA, 3);
            } else if (strcmp(value, "far_table") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_FAR_TABLE, 3);
            } else if (strcmp(value, "far_code") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_FAR_CODE, 3);
            } else if (strcmp(value, "far_dmd_fullframe") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_FAR_DMD_FULLFRAME, 3);
            } else if (strcmp(value, "sprite") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_SPRITE, 0);
            } else if (parse_count_format(value, "sprite_noheader", &length)) {
                add_data_range(data_ranges, bank, addr, DATA_SPRITE_NOHEADER, length);
            } else if (strcmp(value, "ptr16_sprite") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_PTR16_SPRITE, 2);
            } else if (strcmp(value, "far_sprite") == 0) {
                add_data_range(data_ranges, bank, addr, DATA_FAR_SPRITE, 3);
            } else {
                die("invalid data format '%s = %s'", key, value);
            }
            free(value);
        } else if (in_tables) {
            uint32_t addr;
            uint8_t bank;
            char *key = s;
            char *value = dup_config_value(eq + 1);
            TableSchema schema;
            int has_header = 1;
            size_t rows = 0;
            char *inner;

            memset(&schema, 0, sizeof(schema));
            add_table_field(&schema, TABLE_PTR16_DATA, 1);
            if (!parse_bank_label_ref(key, &bank, &addr)) {
                die("invalid table config key '%s'", key);
            }
            if (strcmp(value, "counted_ptr16_string") == 0) {
                free(schema.items);
                memset(&schema, 0, sizeof(schema));
                add_table_field(&schema, TABLE_PTR16_STRING, 1);
            } else if (strcmp(value, "counted_ptr16_data") == 0) {
                free(schema.items);
                memset(&schema, 0, sizeof(schema));
                add_table_field(&schema, TABLE_PTR16_DATA, 1);
            } else if (parse_wrapped_table_format(value, "counted", &inner)) {
                free(schema.items);
                memset(&schema, 0, sizeof(schema));
                if (!parse_table_schema(inner, &schema, types) &&
                    !schema_def_named(schemas, inner, &schema)) {
                    die("invalid counted table row format '%s'", inner);
                }
            } else if (parse_rows_table_format(value, &rows, &schema, schemas, types)) {
                has_header = 0;
            } else if (parse_far_code_table_format(value, &rows)) {
                free(schema.items);
                memset(&schema, 0, sizeof(schema));
                add_table_field(&schema, TABLE_FAR_CODE, 1);
                has_header = 0;
            } else {
                die("invalid table format '%s = %s'", key, value);
            }
            add_table_def(tables, bank, addr, schema, has_header, rows);
            free(value);
        } else if (in_exclude_refs) {
            uint32_t addr;
            uint8_t bank = 0;
            int has_bank = 0;
            char *key = s;
            char *value = dup_config_value(eq + 1);

            if (parse_bank_label_ref(key, &bank, &addr)) {
                has_bank = 1;
            } else if (!parse_u32(key, &addr)) {
                die("invalid exclude_refs config '%s = %s'", key, value);
            }
            if (ref_exclusions) {
                add_config_entry(ref_exclusions, has_bank, bank, addr);
            }
            free(value);
        } else if (in_types) {
            char *key = s;
            char *colon = strchr(key, ':');
            char *type_name_str;
            char *kind_str;
            TableFieldKind kind;
            char *value = dup_config_value(eq + 1);
            char *trimmed_value;

            /* flush previous pending multi-line type */
            if (pt_name[0]) {
                config_set_type(types, pt_name, pt_kind, pt_vals);
                pt_name[0] = '\0'; pt_vals[0] = '\0';
            }
            if (!colon) {
                die("invalid type '%s': expected name:byte or name:word", key);
            }
            *colon = '\0';
            type_name_str = trim(key);
            kind_str = trim(colon + 1);
            if (!parse_table_kind(kind_str, &kind) || (kind != TABLE_BYTE && kind != TABLE_WORD)) {
                die("invalid type kind '%s': must be byte or word", kind_str);
            }
            if (!valid_symbol_name(type_name_str)) {
                die("invalid type name '%s'", type_name_str);
            }
            trimmed_value = trim(value);
            if (*trimmed_value == '\0') {
                /* empty value: accumulate continuation lines */
                snprintf(pt_name, sizeof(pt_name), "%s", type_name_str);
                pt_kind = kind;
                pt_vals[0] = '\0';
            } else {
                config_set_type(types, type_name_str, kind, trimmed_value);
            }
            free(value);
        }
    }
    /* flush any pending multi-line type at end of file */
    if (pt_name[0]) {
        config_set_type(types, pt_name, pt_kind, pt_vals);
    }
    if (ferror(f)) {
        die("failed to read config %s", path);
    }
    fclose(f);
}

int config_set_entry(ConfigEntries *entries, int has_bank, uint8_t bank, uint32_t addr)
{
    add_config_entry(entries, has_bank, bank, addr);
    return 0;
}

int config_clear_entry(ConfigEntries *entries, int has_bank, uint8_t bank, uint32_t addr)
{
    return remove_config_entry(entries, has_bank, bank, addr) ? 0 : 1;
}

int config_set_inline_spec(InlineSignatures *sigs, int has_bank, uint8_t bank, uint32_t addr,
                           const char *spec, const ConfigTypes *types)
{
    char *value;
    TableSchema schema;

    if (!spec || !*spec) {
        return 1;
    }
    value = dup_string(spec);
    memset(&schema, 0, sizeof(schema));
    if (!parse_table_schema(value, &schema, types)) {
        free(schema.items);
        free(value);
        return 1;
    }
    add_inline_signature_schema(sigs, has_bank, bank, addr, &schema);
    free(schema.items);
    free(value);
    return 0;
}

int config_clear_inline(InlineSignatures *sigs, int has_bank, uint8_t bank, uint32_t addr)
{
    return remove_inline_signature(sigs, has_bank, bank, addr) ? 0 : 1;
}

int config_set_data_spec(DataRanges *ranges, uint8_t bank, uint32_t addr, const char *spec)
{
    char *value;
    size_t length;

    if (!spec || !*spec) {
        return 1;
    }
    value = dup_string(spec);
    if (parse_count_format(value, "bytes", &length)) {
        add_data_range(ranges, bank, addr, DATA_BYTES, length);
    } else if (parse_count_format(value, "string", &length)) {
        add_data_range(ranges, bank, addr, DATA_STRING_FIXED, length);
    } else if (strcmp(value, "string") == 0) {
        add_data_range(ranges, bank, addr, DATA_STRING, 0);
    } else if (strcmp(value, "string_lp") == 0) {
        add_data_range(ranges, bank, addr, DATA_STRING_LP, 0);
    } else if (strcmp(value, "dmd_fullframe") == 0) {
        add_data_range(ranges, bank, addr, DATA_DMD_FULLFRAME, 0);
    } else if (strcmp(value, "ptr16_string") == 0) {
        add_data_range(ranges, bank, addr, DATA_PTR16_STRING, 2);
    } else if (strcmp(value, "ptr16_data") == 0) {
        add_data_range(ranges, bank, addr, DATA_PTR16_DATA, 2);
    } else if (strcmp(value, "ptr16_code") == 0) {
        add_data_range(ranges, bank, addr, DATA_PTR16_CODE, 2);
    } else if (strcmp(value, "ptr16_table") == 0) {
        add_data_range(ranges, bank, addr, DATA_PTR16_TABLE, 2);
    } else if (strcmp(value, "far_string") == 0) {
        add_data_range(ranges, bank, addr, DATA_FAR_STRING, 3);
    } else if (strcmp(value, "far_data") == 0) {
        add_data_range(ranges, bank, addr, DATA_FAR_DATA, 3);
    } else if (strcmp(value, "far_table") == 0) {
        add_data_range(ranges, bank, addr, DATA_FAR_TABLE, 3);
    } else if (strcmp(value, "far_code") == 0) {
        add_data_range(ranges, bank, addr, DATA_FAR_CODE, 3);
    } else if (strcmp(value, "far_dmd_fullframe") == 0) {
        add_data_range(ranges, bank, addr, DATA_FAR_DMD_FULLFRAME, 3);
    } else if (strcmp(value, "sprite") == 0) {
        add_data_range(ranges, bank, addr, DATA_SPRITE, 0);
    } else if (parse_count_format(value, "sprite_noheader", &length)) {
        add_data_range(ranges, bank, addr, DATA_SPRITE_NOHEADER, length);
    } else if (strcmp(value, "ptr16_sprite") == 0) {
        add_data_range(ranges, bank, addr, DATA_PTR16_SPRITE, 2);
    } else if (strcmp(value, "far_sprite") == 0) {
        add_data_range(ranges, bank, addr, DATA_FAR_SPRITE, 3);
    } else {
        free(value);
        return 1;
    }
    free(value);
    return 0;
}

int config_clear_data(DataRanges *ranges, uint8_t bank, uint32_t addr)
{
    return remove_data_range(ranges, bank, addr) ? 0 : 1;
}

int config_set_table_spec(TableDefs *tables, const SchemaDefs *schemas, uint8_t bank, uint32_t addr,
                          const char *spec, const ConfigTypes *types)
{
    char *value;
    TableSchema schema;
    int has_header = 1;
    size_t rows = 0;
    char *inner;

    if (!spec || !*spec) {
        return 1;
    }
    value = dup_string(spec);
    memset(&schema, 0, sizeof(schema));
    add_table_field(&schema, TABLE_PTR16_DATA, 1);
    if (strcmp(value, "counted_ptr16_string") == 0) {
        free(schema.items);
        memset(&schema, 0, sizeof(schema));
        add_table_field(&schema, TABLE_PTR16_STRING, 1);
    } else if (strcmp(value, "counted_ptr16_data") == 0) {
        free(schema.items);
        memset(&schema, 0, sizeof(schema));
        add_table_field(&schema, TABLE_PTR16_DATA, 1);
    } else if (parse_wrapped_table_format(value, "counted", &inner)) {
        free(schema.items);
        memset(&schema, 0, sizeof(schema));
        if (!parse_table_schema(inner, &schema, types) && !schema_def_named(schemas, inner, &schema)) {
            free(value);
            return 1;
        }
    } else if (parse_rows_table_format(value, &rows, &schema, schemas, types)) {
        has_header = 0;
    } else if (parse_far_code_table_format(value, &rows)) {
        free(schema.items);
        memset(&schema, 0, sizeof(schema));
        add_table_field(&schema, TABLE_FAR_CODE, 1);
        has_header = 0;
    } else {
        free(schema.items);
        free(value);
        return 1;
    }
    add_table_def(tables, bank, addr, schema, has_header, rows);
    free(value);
    return 0;
}

int config_clear_table(TableDefs *tables, uint8_t bank, uint32_t addr)
{
    return remove_table_def(tables, bank, addr) ? 0 : 1;
}
