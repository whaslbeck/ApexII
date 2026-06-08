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
    ConfigDocs docs;
    ConfigSymbols symbols;
    DataRanges data_ranges;
    ConfigOptions options;
    ConfigTypes config_types;
    ConfigEntries ref_exclusions;
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
int apex_project_set_doc(ApexProject *project, int has_bank, uint8_t bank,
                         uint32_t addr, const char *text);
int apex_project_clear_doc(ApexProject *project, int has_bank, uint8_t bank,
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
int apex_project_save_overlay(ApexProject *project, const char *path, const char *include_path);
/* Sort all config sections into apexini's deterministic order (in place). */
void apex_project_sort_config(ApexProject *project);
int apex_project_set_type(ApexProject *project, const char *name, int is_word,
                          const char *values_str);
int apex_project_remove_type(ApexProject *project, const char *name);
int apex_project_add_ref_exclusion(ApexProject *project, int has_bank, uint8_t bank,
                                   uint32_t addr);
int apex_project_remove_ref_exclusion(ApexProject *project, int has_bank, uint8_t bank,
                                      uint32_t addr);
int apex_project_set_symbol(ApexProject *project, const char *name, uint32_t value);
int apex_project_clear_symbol(ApexProject *project, const char *name);

/* Code candidate scanner ------------------------------------------------- */
#define APEX_CANDIDATE_PREVIEW 80

typedef struct {
    uint8_t  bank;
    uint32_t addr;
    int      score;       /* 0-100 */
    int      tier;        /* 1=far-ptr, 2=probe */
    int      instr_count;
    char     preview[APEX_CANDIDATE_PREVIEW];
} ApexCodeCandidate;

typedef struct {
    ApexCodeCandidate *items;
    size_t             count;
    size_t             cap;
} ApexCodeCandidates;

void apex_scan_code_candidates(const ApexProject *project, ApexCodeCandidates *out);
void apex_free_code_candidates(ApexCodeCandidates *candidates);

/* Inline-dispatcher candidate scanner ------------------------------------ */
#define APEX_INLINE_SPEC_MAX 128

typedef struct {
    uint8_t  bank;
    uint32_t addr;
    char     spec[APEX_INLINE_SPEC_MAX]; /* e.g. "byte", "far_code", "byte, word" */
    int      score;           /* 0-100 */
    int      callsite_count;
    int      callsite_valid;  /* callsites whose inline bytes validate the spec */
} ApexInlineCandidate;

typedef struct {
    ApexInlineCandidate *items;
    size_t               count;
    size_t               cap;
} ApexInlineCandidates;

void apex_scan_inline_candidates(const ApexProject *project, ApexInlineCandidates *out);
void apex_free_inline_candidates(ApexInlineCandidates *candidates);

#endif
