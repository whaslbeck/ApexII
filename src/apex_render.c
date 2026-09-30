#include "apex_render.h"

#include "apex_project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ApexRenderedBlockKind parse_block_name(const char *name, size_t length) {
    if (length == 4 && memcmp(name, "code", 4) == 0) {
        return APEX_RENDER_BLOCK_CODE;
    }
    if (length == 4 && memcmp(name, "data", 4) == 0) {
        return APEX_RENDER_BLOCK_DATA;
    }
    if (length == 6 && memcmp(name, "sprite", 6) == 0) {
        return APEX_RENDER_BLOCK_SPRITE;
    }
    if (length == 5 && memcmp(name, "table", 5) == 0) {
        return APEX_RENDER_BLOCK_TABLE;
    }
    if (length == 12 && memcmp(name, "unclassified", 12) == 0) {
        return APEX_RENDER_BLOCK_UNCLASSIFIED;
    }
    if (length == 4 && memcmp(name, "free", 4) == 0) {
        return APEX_RENDER_BLOCK_FREE;
    }
    return APEX_RENDER_BLOCK_UNKNOWN;
}

static ApexRenderedBlockKind parse_transition_target_block(const char *text, size_t length) {
    size_t i;
    size_t start;
    size_t name_len = 0;

    if (length <= 2 || text[0] != ';' || text[1] != ' ') {
        return APEX_RENDER_BLOCK_UNKNOWN;
    }
    start = 2;
    for (i = start; i + 4 <= length; i++) {
        if (memcmp(text + i, "_to_", 4) == 0) {
            start = i + 4;
            break;
        }
    }
    if (i + 4 > length) {
        /* No "_to_" found — check if comment starts with a plain block name
           (e.g. "; code bank=..." emitted for the first block in a bank). */
        const char *kw = text + 2;
        size_t rem = length - 2;
        size_t klen = 0;
        while (klen < rem && kw[klen] != ' ' && kw[klen] != '\t') {
            klen++;
        }
        return parse_block_name(kw, klen);
    }
    while (start + name_len < length && text[start + name_len] != ' ' &&
           text[start + name_len] != '\t') {
        name_len++;
    }
    return parse_block_name(text + start, name_len);
}

static ApexRenderedTransitionKind parse_transition_kind(const char *text, size_t length) {
    if (length < 4 || text[0] != ';' || text[1] != ' ') {
        return APEX_RENDER_TRANSITION_NONE;
    }
    text += 2;
    length -= 2;
    if (length >= 12 && memcmp(text, "code_to_data", 12) == 0) {
        return APEX_RENDER_TRANSITION_CODE_TO_DATA;
    }
    if (length >= 12 && memcmp(text, "data_to_code", 12) == 0) {
        return APEX_RENDER_TRANSITION_DATA_TO_CODE;
    }
    if (length >= 13 && memcmp(text, "code_to_table", 13) == 0) {
        return APEX_RENDER_TRANSITION_CODE_TO_TABLE;
    }
    if (length >= 13 && memcmp(text, "table_to_code", 13) == 0) {
        return APEX_RENDER_TRANSITION_TABLE_TO_CODE;
    }
    if (length >= 13 && memcmp(text, "table_to_data", 13) == 0) {
        return APEX_RENDER_TRANSITION_TABLE_TO_DATA;
    }
    if (length >= 13 && memcmp(text, "data_to_table", 13) == 0) {
        return APEX_RENDER_TRANSITION_DATA_TO_TABLE;
    }
    if (length >= 20 && memcmp(text, "code_to_unclassified", 20) == 0) {
        return APEX_RENDER_TRANSITION_CODE_TO_UNCLASSIFIED;
    }
    if (length >= 20 && memcmp(text, "unclassified_to_code", 20) == 0) {
        return APEX_RENDER_TRANSITION_UNCLASSIFIED_TO_CODE;
    }
    if (length >= 21 && memcmp(text, "table_to_unclassified", 21) == 0) {
        return APEX_RENDER_TRANSITION_TABLE_TO_UNCLASSIFIED;
    }
    if (length >= 21 && memcmp(text, "unclassified_to_table", 21) == 0) {
        return APEX_RENDER_TRANSITION_UNCLASSIFIED_TO_TABLE;
    }
    if (length >= 20 && memcmp(text, "data_to_unclassified", 20) == 0) {
        return APEX_RENDER_TRANSITION_DATA_TO_UNCLASSIFIED;
    }
    if (length >= 20 && memcmp(text, "unclassified_to_data", 20) == 0) {
        return APEX_RENDER_TRANSITION_UNCLASSIFIED_TO_DATA;
    }
    return APEX_RENDER_TRANSITION_NONE;
}

static void clear_document(ApexRenderedDocument *document) {
    unsigned long gen;

    if (!document) {
        return;
    }
    gen = document->generation;
    free(document->lines);
    free(document->text);
    free(document->addr_index);
    memset(document, 0, sizeof(*document));
    document->generation = gen; /* persist across rebuilds for frontend caches */
}

static int addr_index_cmp(const void *pa, const void *pb) {
    const ApexRenderedAddrIndex *a = (const ApexRenderedAddrIndex *)pa;
    const ApexRenderedAddrIndex *b = (const ApexRenderedAddrIndex *)pb;

    if (a->bank != b->bank)
        return (int)a->bank - (int)b->bank;
    if (a->cpu_addr != b->cpu_addr)
        return a->cpu_addr < b->cpu_addr ? -1 : 1;
    /* stable by line_index so a lookup returns the first line at the address,
       matching the historical linear-scan semantics. */
    if (a->line_index != b->line_index)
        return a->line_index < b->line_index ? -1 : 1;
    return 0;
}

/* Build the (bank, cpu_addr) → line_index lookup index over has_location lines. */
static void build_addr_index(ApexRenderedDocument *document) {
    size_t i, n = 0;

    for (i = 0; i < document->line_count; i++) {
        if (document->lines[i].has_location) {
            n++;
        }
    }
    document->addr_index_count = n;
    if (n == 0) {
        document->addr_index = NULL;
        return;
    }
    document->addr_index = xmalloc(n * sizeof(document->addr_index[0]));
    n = 0;
    for (i = 0; i < document->line_count; i++) {
        if (document->lines[i].has_location) {
            document->addr_index[n].bank = document->lines[i].bank;
            document->addr_index[n].cpu_addr = document->lines[i].cpu_addr;
            document->addr_index[n].line_index = i;
            n++;
        }
    }
    qsort(document->addr_index, n, sizeof(document->addr_index[0]), addr_index_cmp);
}

/* Line kind from the line's syntax alone; location comments are identified by
   the writer's marks, not by their text. */
static ApexRenderedLineKind classify_line_syntax(const char *text, size_t length) {
    size_t i = 0;

    if (length == 0) {
        return APEX_RENDER_LINE_BLANK;
    }
    if (text[0] == ';') {
        return APEX_RENDER_LINE_COMMENT;
    }
    while (i < length && (text[i] == ' ' || text[i] == '\t')) {
        i++;
    }
    if (i >= length) {
        return APEX_RENDER_LINE_BLANK;
    }
    if (i == 0) {
        while (i < length && text[i] != ' ' && text[i] != '\t') {
            if (text[i] == ':') {
                return APEX_RENDER_LINE_LABEL;
            }
            i++;
        }
        return APEX_RENDER_LINE_DIRECTIVE;
    }
    if (text[i] == ';') {
        return APEX_RENDER_LINE_COMMENT; /* indented comment, e.g. "    ; addr ..." */
    }
    return APEX_RENDER_LINE_INSTRUCTION;
}

/* Build the line table from the writer's marks: every location comment and
   every line that assembles to bytes was reported by the writer with its exact
   bank / address / size, so nothing is inferred from the text. */
static void build_line_index_from_marks(ApexRenderedDocument *document, const ApexAsmMark *marks,
                                        size_t mark_count) {
    size_t i, line_count = 0, start = 0, mi = 0;
    int cur_has_location = 0, cur_conflict = 0;
    uint8_t cur_bank = 0;
    uint32_t cur_cpu = 0;
    size_t cur_rom = 0;
    ApexRenderedBlockKind cur_block = APEX_RENDER_BLOCK_UNKNOWN;

    if (document->text_len == 0) {
        return;
    }
    for (i = 0; i < document->text_len; i++) {
        if (document->text[i] == '\n') {
            line_count++;
        }
    }
    if (document->text[document->text_len - 1] != '\n') {
        line_count++;
    }
    document->lines = xmalloc(line_count * sizeof(document->lines[0]));
    memset(document->lines, 0, line_count * sizeof(document->lines[0]));
    document->line_count = line_count;

    line_count = 0;
    while (start < document->text_len) {
        const char *nl = memchr(document->text + start, '\n', document->text_len - start);
        size_t end = nl ? (size_t)(nl - document->text) : document->text_len;
        ApexRenderedLine *l = &document->lines[line_count++];

        l->text = document->text + start;
        l->length = end - start;
        l->kind = classify_line_syntax(l->text, l->length);
        while (mi < mark_count && marks[mi].text_off < start) {
            mi++; /* a mark on a line that was not a line start (should not happen) */
        }
        if (mi < mark_count && marks[mi].text_off == start &&
            marks[mi].kind == APEX_ASM_MARK_LOCATION) {
            const ApexAsmMark *m = &marks[mi++];
            char kw[48];
            size_t kl = (size_t)snprintf(kw, sizeof(kw), "; %s ", m->loc_kind);
            ApexRenderedBlockKind tb = parse_transition_target_block(kw, kl);

            cur_bank = m->bank;
            cur_cpu = m->cpu_addr;
            cur_rom = m->rom_addr;
            cur_conflict = m->conflict;
            if (tb != APEX_RENDER_BLOCK_UNKNOWN) {
                cur_block = tb;
            }
            l->kind = APEX_RENDER_LINE_LOCATION;
            l->transition_kind = parse_transition_kind(kw, kl);
            l->has_location = 1;
            l->block_kind = cur_block;
            l->bank = cur_bank;
            l->cpu_addr = cur_cpu;
            l->rom_addr = cur_rom;
            cur_has_location = 1;
        } else if (cur_has_location &&
                   (l->kind == APEX_RENDER_LINE_LABEL || l->kind == APEX_RENDER_LINE_DIRECTIVE ||
                    l->kind == APEX_RENDER_LINE_INSTRUCTION)) {
            if (mi < mark_count && marks[mi].text_off == start &&
                marks[mi].kind == APEX_ASM_MARK_ITEM) {
                /* the writer's exact byte count; the address continues from
                   the last location comment */
                const ApexAsmMark *m = &marks[mi++];
                l->has_location = 1;
                l->has_conflict = cur_conflict;
                l->block_kind = cur_block;
                l->bank = cur_bank;
                l->cpu_addr = cur_cpu;
                l->rom_addr = cur_rom;
                cur_cpu += (uint32_t)m->size;
                cur_rom += m->size;
            } else {
                l->has_location = 1;
                l->has_conflict = cur_conflict;
                l->block_kind = cur_block;
                l->bank = cur_bank;
                l->cpu_addr = cur_cpu;
                l->rom_addr = cur_rom;
            }
        }
        start = end + 1u;
    }
    document->line_count = line_count;
    build_addr_index(document);
    document->generation++;
}

int apex_render_project(const ApexProject *project, int emit_xrefs, int emit_explain,
                        ApexRenderedDocument *document) {
    ApexAsmText t;

    if (!document) {
        return 1;
    }
    clear_document(document);
    memset(&t, 0, sizeof(t));
    apex_project_write_asm_text(project, emit_xrefs, emit_explain, &t);
    document->text = t.text; /* the document takes ownership of the text */
    document->text_len = t.text_len;
    build_line_index_from_marks(document, t.marks, t.mark_count);
    free(t.marks);
    return 0;
}

void apex_render_document_free(ApexRenderedDocument *document) {
    clear_document(document);
}

const ApexRenderedLine *apex_render_find_line_by_address(const ApexRenderedDocument *document,
                                                         uint8_t bank, uint32_t cpu_addr,
                                                         size_t *line_index) {
    size_t i;

    if (!document) {
        return NULL;
    }
    if (document->addr_index) {
        /* Binary-search the sorted (bank, cpu_addr, line_index) index for the
           first entry at (bank, cpu_addr) — matches the old first-match scan. */
        size_t lo = 0, hi = document->addr_index_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            const ApexRenderedAddrIndex *e = &document->addr_index[mid];
            if (e->bank < bank || (e->bank == bank && e->cpu_addr < cpu_addr)) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < document->addr_index_count && document->addr_index[lo].bank == bank &&
            document->addr_index[lo].cpu_addr == cpu_addr) {
            i = document->addr_index[lo].line_index;
            if (line_index) {
                *line_index = i;
            }
            return &document->lines[i];
        }
        return NULL;
    }
    for (i = 0; i < document->line_count; i++) {
        if (document->lines[i].has_location && document->lines[i].bank == bank &&
            document->lines[i].cpu_addr == cpu_addr) {
            if (line_index) {
                *line_index = i;
            }
            return &document->lines[i];
        }
    }
    return NULL;
}

const ApexRenderedLine *apex_render_find_next_transition(const ApexRenderedDocument *document,
                                                         size_t start_index,
                                                         ApexRenderedTransitionKind kind,
                                                         size_t *line_index) {
    size_t i;

    if (!document) {
        return NULL;
    }
    for (i = start_index + 1u; i < document->line_count; i++) {
        if (document->lines[i].transition_kind == kind) {
            if (line_index) {
                *line_index = i;
            }
            return &document->lines[i];
        }
    }
    return NULL;
}

const ApexRenderedLine *apex_render_find_prev_transition(const ApexRenderedDocument *document,
                                                         size_t start_index,
                                                         ApexRenderedTransitionKind kind,
                                                         size_t *line_index) {
    size_t i;

    if (!document || document->line_count == 0) {
        return NULL;
    }
    if (start_index >= document->line_count) {
        start_index = document->line_count - 1u;
    }
    for (i = start_index + 1u; i > 0; i--) {
        if (document->lines[i - 1u].transition_kind == kind) {
            if (line_index) {
                *line_index = i - 1u;
            }
            return &document->lines[i - 1u];
        }
    }
    return NULL;
}
