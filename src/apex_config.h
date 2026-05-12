#ifndef APEX_CONFIG_H
#define APEX_CONFIG_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TABLE_PTR16_STRING,
    TABLE_PTR16_DATA,
    TABLE_PTR16_CODE,
    TABLE_PTR16_TABLE,
    TABLE_PTR16_DMD_FULLFRAME,
    TABLE_FAR_STRING,
    TABLE_FAR_DATA,
    TABLE_FAR_TABLE,
    TABLE_FAR_CODE,
    TABLE_FAR_DMD_FULLFRAME,
    TABLE_BYTE,
    TABLE_WORD
} TableFieldKind;

typedef struct {
    const char *name;   /* owned string */
    uint32_t value;
} ConfigTypeValue;

typedef struct {
    char *name;             /* owned */
    TableFieldKind kind;    /* TABLE_BYTE or TABLE_WORD only */
    ConfigTypeValue *values;
    size_t value_count;
    size_t value_cap;
} ConfigType;

typedef struct {
    ConfigType *items;
    size_t count;
    size_t cap;
} ConfigTypes;

typedef struct {
    TableFieldKind kind;
    size_t count;
    const char *type_name;  /* borrowed ptr into ConfigType.name, NULL if no type */
} TableField;

typedef struct {
    TableField *items;
    size_t count;
    size_t cap;
} TableSchema;

typedef struct {
    int has_bank;
    uint8_t bank;
    uint32_t addr;
    unsigned length;
    TableSchema schema;
} InlineSignature;

typedef struct {
    InlineSignature *items;
    size_t count;
    size_t cap;
} InlineSignatures;

typedef struct {
    int has_bank;
    uint8_t bank;
    uint32_t addr;
    char *text;
} ConfigDoc;

typedef struct {
    ConfigDoc *items;
    size_t count;
    size_t cap;
} ConfigDocs;

typedef struct {
    const char *name;
    uint32_t value;
} ConfigSymbol;

typedef struct ConfigSymbols {
    ConfigSymbol *items;
    size_t count;
    size_t cap;
} ConfigSymbols;

typedef struct {
    int has_bank;
    uint8_t bank;
    uint32_t addr;
    char *name;
} ConfigLabel;

typedef struct {
    ConfigLabel *items;
    size_t count;
    size_t cap;
} ConfigLabels;

typedef struct {
    int has_bank;
    uint8_t bank;
    uint32_t addr;
} ConfigEntry;

typedef struct {
    ConfigEntry *items;
    size_t count;
    size_t cap;
} ConfigEntries;

typedef struct {
    int labels_are_entries;
} ConfigOptions;

typedef struct {
    uint8_t bank;
    uint32_t addr;
    TableSchema schema;
    int has_header;
    size_t rows;
} TableDef;

typedef struct {
    TableDef *items;
    size_t count;
    size_t cap;
} TableDefs;

typedef struct {
    const char *name;
    TableSchema schema;
} SchemaDef;

typedef struct {
    SchemaDef *items;
    size_t count;
    size_t cap;
} SchemaDefs;

typedef enum {
    DATA_BYTES,
    DATA_STRING,
    DATA_DMD_FULLFRAME,
    DATA_FAR_STRING,
    DATA_FAR_DATA,
    DATA_FAR_TABLE,
    DATA_FAR_CODE,
    DATA_FAR_DMD_FULLFRAME
} DataKind;

typedef struct {
    uint8_t bank;
    uint32_t addr;
    DataKind kind;
    size_t length;
} DataRange;

typedef struct {
    DataRange *items;
    size_t count;
    size_t cap;
} DataRanges;

typedef enum {
    APEX_KIND_CODE,
    APEX_KIND_DATA,
    APEX_KIND_STRING,
    APEX_KIND_TABLE
} ApexConfigKind;

void add_table_field(TableSchema *schema, TableFieldKind kind, size_t count);
size_t table_schema_width(const TableSchema *schema);
int table_kind_is_far(TableFieldKind kind);

void add_inline_signature_schema(InlineSignatures *sigs, int has_bank, uint8_t bank,
                                 uint32_t addr, const TableSchema *schema);
void add_inline_signature_ex(InlineSignatures *sigs, int has_bank, uint8_t bank, uint32_t addr,
                             unsigned length, TableFieldKind kind);

void load_config(const char *path, InlineSignatures *sigs, ConfigLabels *labels,
                 ConfigEntries *entries, TableDefs *tables, SchemaDefs *schemas,
                 ConfigDocs *routine_docs, ConfigDocs *table_docs, ConfigSymbols *symbols,
                 DataRanges *data_ranges, ConfigOptions *options, ConfigTypes *types);
void free_config_types(ConfigTypes *types);
const ConfigType *find_config_type(const ConfigTypes *types, const char *name);
const char *config_type_enum_name(const ConfigTypes *types, const char *type_name, uint32_t value);
void config_set_type(ConfigTypes *types, const char *name, TableFieldKind kind,
                     const char *values_str);
int config_remove_type(ConfigTypes *types, const char *name);
int config_set_entry(ConfigEntries *entries, int has_bank, uint8_t bank, uint32_t addr);
int config_clear_entry(ConfigEntries *entries, int has_bank, uint8_t bank, uint32_t addr);
int config_set_inline_spec(InlineSignatures *sigs, int has_bank, uint8_t bank, uint32_t addr,
                           const char *spec, const ConfigTypes *types);
int config_clear_inline(InlineSignatures *sigs, int has_bank, uint8_t bank, uint32_t addr);
int config_set_data_spec(DataRanges *ranges, uint8_t bank, uint32_t addr, const char *spec);
int config_clear_data(DataRanges *ranges, uint8_t bank, uint32_t addr);
int config_set_table_spec(TableDefs *tables, const SchemaDefs *schemas, uint8_t bank, uint32_t addr,
                          const char *spec, const ConfigTypes *types);
int config_clear_table(TableDefs *tables, uint8_t bank, uint32_t addr);

#endif
