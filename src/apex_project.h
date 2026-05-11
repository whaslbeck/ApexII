#ifndef APEX_PROJECT_H
#define APEX_PROJECT_H

#include <stdio.h>

#include "apex.h"
#include "apex_analysis.h"
#include "apex_config.h"

struct ApexRenderedDocument;

typedef enum {
    APEX_DIRTY_NONE = 0,
    APEX_DIRTY_LABELS = 1 << 0,
    APEX_DIRTY_DOCS = 1 << 1,
    APEX_DIRTY_RENDER = 1 << 2,
    APEX_DIRTY_ANALYSIS = 1 << 3
} ApexDirtyFlags;

typedef enum {
    APEX_ANALYZE_SCOPE_NONE = 0,
    APEX_ANALYZE_SCOPE_FULL,
    APEX_ANALYZE_SCOPE_SYSTEM_ONLY,
    APEX_ANALYZE_SCOPE_BANK_ONLY
} ApexAnalyzeScope;

typedef struct ApexProject {
    const char *rom_path;
    const char *config_path;
    Buffer rom;
    size_t paged_size;
    size_t banks;
    VectorInfo vectors[7];
    InlineSignatures inline_sigs;
    ConfigLabels config_labels;
    ConfigEntries config_entries;
    TableDefs tables;
    SchemaDefs schemas;
    ConfigDocs routine_docs;
    ConfigDocs table_docs;
    ConfigSymbols symbols;
    DataRanges data_ranges;
    ConfigOptions options;
    ReferenceSet refs;
    LabelSet system_labels;
    LabelSet *bank_labels;
    unsigned dirty_flags;
    int analysis_ready;
    ApexAnalyzeScope analysis_scope;
    uint8_t analysis_bank_id;
    struct ApexRenderedDocument *render_cache;
    int render_cache_emit_xrefs;
    int render_cache_emit_explain;
} ApexProject;

ApexProject *apex_project_open(const char *rom_path, const char *config_path);
void apex_project_free(ApexProject *project);
int apex_project_analyze(ApexProject *project);
void apex_project_invalidate(ApexProject *project, unsigned dirty_flags);
int apex_project_set_label(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr,
                           const char *name);
int apex_project_clear_label(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr);
int apex_project_set_doc(ApexProject *project, int is_table_doc, int has_bank, uint8_t bank,
                         uint32_t addr, const char *text);
int apex_project_clear_doc(ApexProject *project, int is_table_doc, int has_bank, uint8_t bank,
                           uint32_t addr);
int apex_project_set_kind(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr,
                          ApexConfigKind kind, const char *spec);
int apex_project_clear_kind(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr);
int apex_project_set_inline(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr,
                            const char *spec);
int apex_project_clear_inline(ApexProject *project, int has_bank, uint8_t bank, uint32_t addr);
int apex_project_set_table(ApexProject *project, uint8_t bank, uint32_t addr, const char *spec);
int apex_project_clear_table(ApexProject *project, uint8_t bank, uint32_t addr);
const struct ApexRenderedDocument *apex_project_render(ApexProject *project, int emit_xrefs,
                                                       int emit_explain);
int apex_project_write_asm_stream(const ApexProject *project, FILE *out, int emit_xrefs,
                                  int emit_explain);
int apex_project_write_asm(const ApexProject *project, const char *output_path, int emit_xrefs,
                           int emit_explain);
int apex_project_save_overlay(const ApexProject *project, const char *path, const char *include_path);

#endif
