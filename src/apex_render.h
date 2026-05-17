#ifndef APEX_RENDER_H
#define APEX_RENDER_H

#include <stddef.h>
#include <stdint.h>

struct ApexProject;

typedef struct {
    const char *text;
    size_t length;
} ApexRenderedTextSlice;

typedef enum {
    APEX_RENDER_LINE_UNKNOWN = 0,
    APEX_RENDER_LINE_BLANK,
    APEX_RENDER_LINE_COMMENT,
    APEX_RENDER_LINE_LOCATION,
    APEX_RENDER_LINE_LABEL,
    APEX_RENDER_LINE_DIRECTIVE,
    APEX_RENDER_LINE_INSTRUCTION
} ApexRenderedLineKind;

typedef enum {
    APEX_RENDER_BLOCK_UNKNOWN = 0,
    APEX_RENDER_BLOCK_CODE,
    APEX_RENDER_BLOCK_DATA,
    APEX_RENDER_BLOCK_TABLE,
    APEX_RENDER_BLOCK_UNCLASSIFIED
} ApexRenderedBlockKind;

typedef enum {
    APEX_RENDER_TRANSITION_NONE = 0,
    APEX_RENDER_TRANSITION_CODE_TO_DATA,
    APEX_RENDER_TRANSITION_DATA_TO_CODE,
    APEX_RENDER_TRANSITION_CODE_TO_TABLE,
    APEX_RENDER_TRANSITION_TABLE_TO_CODE,
    APEX_RENDER_TRANSITION_TABLE_TO_DATA,
    APEX_RENDER_TRANSITION_DATA_TO_TABLE,
    APEX_RENDER_TRANSITION_CODE_TO_UNCLASSIFIED,
    APEX_RENDER_TRANSITION_UNCLASSIFIED_TO_CODE,
    APEX_RENDER_TRANSITION_TABLE_TO_UNCLASSIFIED,
    APEX_RENDER_TRANSITION_UNCLASSIFIED_TO_TABLE,
    APEX_RENDER_TRANSITION_DATA_TO_UNCLASSIFIED,
    APEX_RENDER_TRANSITION_UNCLASSIFIED_TO_DATA
} ApexRenderedTransitionKind;

typedef struct {
    const char *text;
    size_t length;
    ApexRenderedLineKind kind;
    ApexRenderedBlockKind block_kind;
    ApexRenderedTransitionKind transition_kind;
    int has_location;
    int has_conflict;
    uint8_t bank;
    uint32_t cpu_addr;
    size_t rom_addr;
} ApexRenderedLine;

typedef struct ApexRenderedDocument {
    char *text;
    size_t text_len;
    ApexRenderedLine *lines;
    size_t line_count;
} ApexRenderedDocument;

int apex_render_project(const struct ApexProject *project, int emit_xrefs, int emit_explain,
                        ApexRenderedDocument *document);
void apex_render_document_free(ApexRenderedDocument *document);
const ApexRenderedLine *apex_render_find_line_by_address(const ApexRenderedDocument *document,
                                                         uint8_t bank, uint32_t cpu_addr,
                                                         size_t *line_index);
const ApexRenderedLine *apex_render_find_next_transition(const ApexRenderedDocument *document,
                                                         size_t start_index,
                                                         ApexRenderedTransitionKind kind,
                                                         size_t *line_index);
const ApexRenderedLine *apex_render_find_prev_transition(const ApexRenderedDocument *document,
                                                         size_t start_index,
                                                         ApexRenderedTransitionKind kind,
                                                         size_t *line_index);

#endif
