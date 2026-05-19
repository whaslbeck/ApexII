#ifndef APEX_ANALYSIS_H
#define APEX_ANALYSIS_H

#include "apex_config.h"
#include "cpu6809.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t addr;
    const char *name;
    int is_code;
    int is_data;
    int is_string;
    int is_conflict;
    int scanned;
    int is_explicit_entry;  /* from [entries] config section */
    int reached_by_flow;    /* flow from another entry reaches this address */
    const char *explain;
    const char *kind_explain;
} Label;

typedef struct {
    Label *items;
    size_t count;
    size_t cap;
    int sorted;
} LabelSet;

typedef struct {
    const char *vector_name;
    const char *entry_name;
    uint32_t vector_addr;
    uint32_t target_addr;
} VectorInfo;

typedef struct {
    const Label *labels;
    size_t label_count;
    const Label *extra_labels;
    size_t extra_label_count;
    const ConfigSymbols *symbols;
    int sorted;
} LabelLookup;

typedef struct {
    uint8_t bank;
    uint32_t addr;
    uint8_t source_bank;
    uint32_t source_addr;
    const char *kind;
    const char *source;
    int row_index;      /* >= 0 for table row refs, -1 otherwise */
    uint32_t row_cpu_addr; /* CPU address of the row entry within the table */
} Reference;

typedef struct {
    Reference *items;
    size_t count;
    size_t cap;
    int sorted;
} ReferenceSet;

size_t last_non_ff(const uint8_t *data, size_t len);
uint16_t read_be16(const uint8_t *p);
int generated_label_name(const char *name);
int generated_string_label_name(const char *name);
int generated_any_label_name(const char *name);
void collect_vectors(const uint8_t *system, VectorInfo *vectors, size_t count);
Label *add_label(LabelSet *set, uint32_t addr, const char *name, int is_code);
void explain_label(Label *label, const char *source);
void explain_label_kind(Label *label, const char *source);
void mark_label_data(Label *label);
void add_reference(ReferenceSet *refs, uint8_t bank, uint32_t addr, uint8_t source_bank,
                   uint32_t source_addr, const char *kind, const char *source);
void add_table_row_reference(ReferenceSet *refs, uint8_t bank, uint32_t addr,
                              uint8_t source_bank, uint32_t source_addr, const char *source,
                              int row_index, uint32_t row_cpu_addr);
void sort_data_ranges(DataRanges *ranges);
void sort_table_defs(TableDefs *tables);
void sort_inline_signatures(InlineSignatures *sigs);
void sort_and_dedup_refs(ReferenceSet *refs);
size_t remove_references_from_source_range(ReferenceSet *refs, uint8_t source_bank,
                                           uint32_t source_start, uint32_t source_end);
size_t prune_unreferenced_generated_labels(LabelSet *labels, uint8_t bank,
                                           const ReferenceSet *refs);
const char *make_generated_label(uint32_t addr);
const InlineSignature *inline_signature_for(const InlineSignatures *sigs, uint8_t bank,
                                            uint32_t addr);
const char *make_bank_label(uint8_t bank, uint16_t addr);
const char *make_string_label(const char *base, const uint8_t *data, size_t len);
void apply_string_content_labels(LabelSet *labels, const uint8_t *data, size_t used,
                                 uint32_t base_addr);
int bank_index_for_id(const uint8_t *paged_rom, size_t banks, uint8_t bank_id);
int bank_index_for_far_ref(const uint8_t *paged_rom, size_t banks, uint8_t bank);
uint8_t bank_id_for_index(const uint8_t *paged_rom, int bank_index);
void validate_config_classification(const ConfigEntries *entries, const TableDefs *tables,
                                    const DataRanges *data_ranges);
size_t labels_at(uint32_t addr, const Label *labels, size_t label_count, int sorted);
int code_label_at(uint32_t addr, const Label *labels, size_t label_count, int sorted);
const DataRange *data_range_at(uint8_t bank, uint32_t addr, const DataRanges *ranges);
const char *config_doc_at(const ConfigDocs *docs, uint8_t bank, uint32_t addr);
int string_label_at(uint32_t addr, const Label *labels, size_t label_count, int sorted);
const char *label_name_at(uint32_t addr, const Label *labels, size_t label_count, int sorted);
const char *symbol_name_at(uint32_t addr, const ConfigSymbols *symbols);
int label_between(uint32_t start, uint32_t end, const Label *labels, size_t label_count,
                  int sorted);
void sort_label_set(LabelSet *set);
size_t label_lower_bound(const Label *labels, size_t count, uint32_t addr);
size_t refs_lower_bound(const ReferenceSet *refs, uint8_t bank, uint32_t addr);
const char *lookup_label_for_cpu(void *ctx, uint32_t addr);
const TableDef *table_def_at(uint8_t bank, uint32_t addr, const TableDefs *tables);
int in_system_addr(uint32_t addr);
int scannable_code_addr(uint32_t addr, size_t used);
uint32_t detect_inline_dispatcher(const uint8_t *data, size_t used, const VectorInfo *vectors,
                                  size_t vector_count);
unsigned inline_bytes_consumed(const Cpu6809InstrInfo *info, const InlineSignatures *sigs,
                               uint8_t current_bank, size_t pos, size_t used);
int valid_far_code_target(uint16_t addr, uint8_t bank, const uint8_t *paged_rom, size_t banks);
void apply_inline_far_label(TableFieldKind kind, const uint8_t *paged_rom, size_t banks,
                            LabelSet *bank_labels, LabelSet *system_labels, uint16_t addr,
                            uint8_t bank);
void apply_inline_ptr16_label(TableFieldKind kind, LabelSet *labels, uint8_t bank, uint16_t ptr);
void apply_system_ptr16_label(TableFieldKind kind, LabelSet *system_labels, uint16_t ptr,
                              const char *source_prefix);
void collect_inline_refs(const InlineSignature *sig, const uint8_t *data, size_t used,
                         size_t *pos, uint8_t current_bank, const uint8_t *paged_rom,
                         size_t banks, LabelSet *bank_labels, LabelSet *system_labels,
                         const char *source, uint32_t source_addr, ReferenceSet *refs);
void collect_code_targets(const uint8_t *data, size_t used, uint32_t base_addr, LabelSet *labels,
                          const InlineSignatures *inline_sigs, const uint8_t *paged_rom,
                          size_t banks, LabelSet *bank_labels, LabelSet *system_labels,
                          const DataRanges *data_ranges, uint8_t current_bank,
                          ReferenceSet *refs, const ConfigEntries *ref_exclusions);
const char *vector_entry_at(uint32_t addr, const VectorInfo *vectors, size_t vector_count);
size_t valid_string_len(const uint8_t *data, size_t len);

#endif
