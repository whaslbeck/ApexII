#include "apex_render.h"
#include "apex_project.h"
#include "cpu6809.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ApexRenderedLineKind classify_line(const char *text, size_t length)
{
    size_t i = 0;

    if (length == 0) {
        return APEX_RENDER_LINE_BLANK;
    }
    if (text[0] == ';') {
        if (length > 2 && text[1] == ' ') {
            char *copy = xmalloc(length + 1u);
            ApexRenderedLineKind kind = APEX_RENDER_LINE_COMMENT;

            memcpy(copy, text, length);
            copy[length] = '\0';
            if (strstr(copy, " bank=0x") && strstr(copy, " cpu=0x") && strstr(copy, " rom=0x")) {
                kind = APEX_RENDER_LINE_LOCATION;
            }
            free(copy);
            return kind;
        }
        return APEX_RENDER_LINE_COMMENT;
    }
    while (i < length && (text[i] == ' ' || text[i] == '\t')) {
        i++;
    }
    if (i >= length) {
        return APEX_RENDER_LINE_BLANK;
    }
    if (i == 0) {
        while (i < length) {
            if (text[i] == ':') {
                return APEX_RENDER_LINE_LABEL;
            }
            if (text[i] == ' ' || text[i] == '\t') {
                break;
            }
            i++;
        }
    }
    if (text[0] == ' ' || text[0] == '\t') {
        return APEX_RENDER_LINE_INSTRUCTION;
    }
    return APEX_RENDER_LINE_DIRECTIVE;
}

static ApexRenderedBlockKind parse_block_name(const char *name, size_t length)
{
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

static ApexRenderedBlockKind parse_transition_target_block(const char *text, size_t length)
{
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
        size_t rem    = length - 2;
        size_t klen   = 0;
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

static ApexRenderedTransitionKind parse_transition_kind(const char *text, size_t length)
{
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

static int parse_hex_field(const char *text, const char *name, unsigned long *value)
{
    const char *field = strstr(text, name);
    char *end = NULL;

    if (!field) {
        return 0;
    }
    field += strlen(name);
    *value = strtoul(field, &end, 16);
    return end != field;
}

static int parse_location_comment(const char *text, size_t length, uint8_t *bank,
                                  uint32_t *cpu_addr, size_t *rom_addr)
{
    char *copy = xmalloc(length + 1u);
    unsigned long parsed_bank;
    unsigned long parsed_cpu;
    unsigned long parsed_rom;
    int ok;

    memcpy(copy, text, length);
    copy[length] = '\0';
    ok = parse_hex_field(copy, "bank=0x", &parsed_bank) &&
         parse_hex_field(copy, "cpu=0x", &parsed_cpu) &&
         parse_hex_field(copy, "rom=0x", &parsed_rom);
    free(copy);
    if (!ok) {
        return 0;
    }
    *bank = (uint8_t)parsed_bank;
    *cpu_addr = (uint32_t)parsed_cpu;
    *rom_addr = (size_t)parsed_rom;
    return 1;
}

static const char *skip_space(const char *text, size_t length, size_t *offset)
{
    while (*offset < length && (text[*offset] == ' ' || text[*offset] == '\t')) {
        (*offset)++;
    }
    return text + *offset;
}

static int mnemonic_equals(const char *text, size_t length, const char *name)
{
    size_t offset = 0;
    size_t name_len = strlen(name);
    const char *start = skip_space(text, length, &offset);

    return offset + name_len <= length &&
           memcmp(start, name, name_len) == 0 &&
           (offset + name_len == length || start[name_len] == ' ' || start[name_len] == '\t');
}

static size_t parse_db_count(const char *text, size_t length)
{
    size_t offset = 0;
    size_t count = 0;
    int in_token = 0;
    const char *start = skip_space(text, length, &offset);

    if (offset + 3u > length || memcmp(start, ".DB", 3) != 0) {
        return 0;
    }
    offset += 3u;
    while (offset < length) {
        char ch = text[offset];

        if (ch == ';') {
            break;
        }
        if (ch == ',') {
            if (in_token) {
                count++;
                in_token = 0;
            }
        } else if (!isspace((unsigned char)ch)) {
            in_token = 1;
        }
        offset++;
    }
    if (in_token) {
        count++;
    }
    return count;
}

static size_t parse_string_asm_length(const char *text, size_t length)
{
    size_t offset = 0;
    const char *start = skip_space(text, length, &offset);
    size_t count = 1u;

    if (offset + 6u > length || memcmp(start, "STRING", 6) != 0) {
        return 0;
    }
    offset += 6u;
    while (offset < length && text[offset] != '"') {
        if (text[offset] == ';') {
            return 0;
        }
        offset++;
    }
    if (offset >= length || text[offset] != '"') {
        return 0;
    }
    offset++;
    while (offset < length) {
        if (text[offset] == '\\' && offset + 1u < length) {
            count++;
            offset += 2u;
            continue;
        }
        if (text[offset] == '"') {
            return count;
        }
        count++;
        offset++;
    }
    return 0;
}

static size_t rendered_line_size(const ApexProject *project, const ApexRenderedLine *line)
{
    Cpu6809InstrInfo info;

    if (!line->has_location) {
        return 0;
    }
    if (mnemonic_equals(line->text, line->length, ".DB")) {
        return parse_db_count(line->text, line->length);
    }
    if (mnemonic_equals(line->text, line->length, ".DW")) {
        return 2u;
    }
    if (mnemonic_equals(line->text, line->length, "STRING")) {
        return parse_string_asm_length(line->text, line->length);
    }
    if (mnemonic_equals(line->text, line->length, "STRING_FIXED")) {
        size_t n = parse_string_asm_length(line->text, line->length);
        return n > 0u ? n - 1u : 0u;  /* no null/length byte */
    }
    if (mnemonic_equals(line->text, line->length, "BCD")) {
        /* BCD <2N digits> → N bytes */
        size_t i = 0, digits = 0;
        while (i < line->length && (line->text[i] == ' ' || line->text[i] == '\t')) i++;
        i += 3; /* "BCD" */
        while (i < line->length && (line->text[i] == ' ' || line->text[i] == '\t')) i++;
        while (i < line->length && line->text[i] >= '0' && line->text[i] <= '9') { digits++; i++; }
        return digits / 2u;
    }
    if (mnemonic_equals(line->text, line->length, "INLINE_BYTE")) {
        return 1u;
    }
    if (mnemonic_equals(line->text, line->length, "INLINE_WORD")) {
        return 2u;
    }
    if (mnemonic_equals(line->text, line->length, "INLINE_PTR") ||
        mnemonic_equals(line->text, line->length, "INLINE_STRING_PTR") ||
        mnemonic_equals(line->text, line->length, "INLINE_TABLE_PTR") ||
        mnemonic_equals(line->text, line->length, "INLINE_CODE_PTR") ||
        mnemonic_equals(line->text, line->length, "INLINE_PTR_DMD_FULLFRAME") ||
        mnemonic_equals(line->text, line->length, "INLINE_PTR_SPRITE") ||
        mnemonic_equals(line->text, line->length, "TABLE_PTR") ||
        mnemonic_equals(line->text, line->length, "TABLE_STRING_PTR") ||
        mnemonic_equals(line->text, line->length, "TABLE_CODE_PTR") ||
        mnemonic_equals(line->text, line->length, "TABLE_PTR_DMD_FULLFRAME") ||
        mnemonic_equals(line->text, line->length, "TABLE_PTR_SPRITE")) {
        return 2u;
    }
    if (mnemonic_equals(line->text, line->length, "INLINE_FAR_PTR") ||
        mnemonic_equals(line->text, line->length, "INLINE_FAR_STRING") ||
        mnemonic_equals(line->text, line->length, "INLINE_FAR_TABLE") ||
        mnemonic_equals(line->text, line->length, "INLINE_FAR_CODE") ||
        mnemonic_equals(line->text, line->length, "INLINE_FAR_DMD_FULLFRAME") ||
        mnemonic_equals(line->text, line->length, "INLINE_FAR_SPRITE") ||
        mnemonic_equals(line->text, line->length, "FAR_PTR") ||
        mnemonic_equals(line->text, line->length, "FAR_STRING") ||
        mnemonic_equals(line->text, line->length, "FAR_TABLE") ||
        mnemonic_equals(line->text, line->length, "FAR_CODE") ||
        mnemonic_equals(line->text, line->length, "FAR_DMD_FULLFRAME") ||
        mnemonic_equals(line->text, line->length, "FAR_SPRITE") ||
        mnemonic_equals(line->text, line->length, "TABLE_FAR_PTR") ||
        mnemonic_equals(line->text, line->length, "TABLE_FAR_STRING") ||
        mnemonic_equals(line->text, line->length, "TABLE_FAR_TABLE") ||
        mnemonic_equals(line->text, line->length, "TABLE_FAR_CODE") ||
        mnemonic_equals(line->text, line->length, "TABLE_FAR_DMD_FULLFRAME") ||
        mnemonic_equals(line->text, line->length, "TABLE_FAR_SPRITE")) {
        return 3u;
    }
    if (line->kind == APEX_RENDER_LINE_INSTRUCTION) {
        if (line->rom_addr >= project->rom.size) {
            return 0;
        }
        info = cpu6809_disassemble_info(project->rom.data + line->rom_addr,
                                        project->rom.size - line->rom_addr, line->cpu_addr,
                                        NULL, 0);
        return info.size;
    }
    return 0;
}

static void clear_document(ApexRenderedDocument *document)
{
    if (!document) {
        return;
    }
    free(document->lines);
    free(document->text);
    memset(document, 0, sizeof(*document));
}

static long file_size(FILE *stream)
{
    long size;

    if (fseek(stream, 0L, SEEK_END) != 0) {
        return -1L;
    }
    size = ftell(stream);
    if (size < 0L) {
        return -1L;
    }
    if (fseek(stream, 0L, SEEK_SET) != 0) {
        return -1L;
    }
    return size;
}

static void build_line_index(const ApexProject *project, ApexRenderedDocument *document)
{
    size_t i;
    size_t line_count = 0;
    size_t start = 0;
    int current_has_location = 0;
    int current_has_conflict = 0;
    uint8_t current_bank = 0;
    uint32_t current_cpu_addr = 0;
    size_t current_rom_addr = 0;
    ApexRenderedBlockKind current_block = APEX_RENDER_BLOCK_UNKNOWN;

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
    for (i = 0; i < document->text_len; i++) {
        if (document->text[i] == '\n') {
            document->lines[line_count].text = document->text + start;
            document->lines[line_count].length = i - start;
            document->lines[line_count].kind =
                classify_line(document->lines[line_count].text, document->lines[line_count].length);
            if (document->lines[line_count].kind == APEX_RENDER_LINE_LOCATION &&
                parse_location_comment(document->lines[line_count].text,
                                       document->lines[line_count].length, &current_bank,
                                       &current_cpu_addr, &current_rom_addr)) {
                static const char conflict_prefix[] = "; WARNING classification_conflict ";
                ApexRenderedBlockKind transition_block =
                    parse_transition_target_block(document->lines[line_count].text,
                                                  document->lines[line_count].length);
                ApexRenderedTransitionKind transition_kind =
                    parse_transition_kind(document->lines[line_count].text,
                                          document->lines[line_count].length);

                if (document->lines[line_count].length >= sizeof(conflict_prefix) - 1 &&
                    memcmp(document->lines[line_count].text, conflict_prefix,
                           sizeof(conflict_prefix) - 1) == 0) {
                    current_has_conflict = 1;
                } else {
                    current_has_conflict = 0;
                }
                if (transition_block != APEX_RENDER_BLOCK_UNKNOWN) {
                    current_block = transition_block;
                }
                document->lines[line_count].transition_kind = transition_kind;
                document->lines[line_count].has_location = 1;
                document->lines[line_count].block_kind = current_block;
                document->lines[line_count].bank = current_bank;
                document->lines[line_count].cpu_addr = current_cpu_addr;
                document->lines[line_count].rom_addr = current_rom_addr;
                current_has_location = 1;
            } else if (current_has_location &&
                       (document->lines[line_count].kind == APEX_RENDER_LINE_LABEL ||
                        document->lines[line_count].kind == APEX_RENDER_LINE_DIRECTIVE ||
                        document->lines[line_count].kind == APEX_RENDER_LINE_INSTRUCTION)) {
                size_t size;

                document->lines[line_count].has_location = 1;
                document->lines[line_count].has_conflict = current_has_conflict;
                document->lines[line_count].block_kind = current_block;
                document->lines[line_count].bank = current_bank;
                document->lines[line_count].cpu_addr = current_cpu_addr;
                document->lines[line_count].rom_addr = current_rom_addr;
                size = rendered_line_size(project, &document->lines[line_count]);
                current_cpu_addr += (uint32_t)size;
                current_rom_addr += size;
            }
            line_count++;
            start = i + 1u;
        }
    }
    if (start < document->text_len) {
        document->lines[line_count].text = document->text + start;
        document->lines[line_count].length = document->text_len - start;
        document->lines[line_count].kind =
            classify_line(document->lines[line_count].text, document->lines[line_count].length);
        if (document->lines[line_count].kind == APEX_RENDER_LINE_LOCATION &&
            parse_location_comment(document->lines[line_count].text,
                                   document->lines[line_count].length, &current_bank,
                                   &current_cpu_addr, &current_rom_addr)) {
            static const char conflict_prefix[] = "; WARNING classification_conflict ";
            ApexRenderedBlockKind transition_block =
                parse_transition_target_block(document->lines[line_count].text,
                                              document->lines[line_count].length);
            ApexRenderedTransitionKind transition_kind =
                parse_transition_kind(document->lines[line_count].text,
                                      document->lines[line_count].length);

            if (document->lines[line_count].length >= sizeof(conflict_prefix) - 1 &&
                memcmp(document->lines[line_count].text, conflict_prefix,
                       sizeof(conflict_prefix) - 1) == 0) {
                current_has_conflict = 1;
            } else {
                current_has_conflict = 0;
            }
            if (transition_block != APEX_RENDER_BLOCK_UNKNOWN) {
                current_block = transition_block;
            }
            document->lines[line_count].transition_kind = transition_kind;
            document->lines[line_count].has_location = 1;
            document->lines[line_count].block_kind = current_block;
            document->lines[line_count].bank = current_bank;
            document->lines[line_count].cpu_addr = current_cpu_addr;
            document->lines[line_count].rom_addr = current_rom_addr;
        } else if (current_has_location &&
                   (document->lines[line_count].kind == APEX_RENDER_LINE_LABEL ||
                    document->lines[line_count].kind == APEX_RENDER_LINE_DIRECTIVE ||
                    document->lines[line_count].kind == APEX_RENDER_LINE_INSTRUCTION)) {
            size_t size;

            document->lines[line_count].has_location = 1;
            document->lines[line_count].has_conflict = current_has_conflict;
            document->lines[line_count].block_kind = current_block;
            document->lines[line_count].bank = current_bank;
            document->lines[line_count].cpu_addr = current_cpu_addr;
            document->lines[line_count].rom_addr = current_rom_addr;
            size = rendered_line_size(project, &document->lines[line_count]);
            current_cpu_addr += (uint32_t)size;
            current_rom_addr += size;
        }
    }
}

int apex_render_project(const ApexProject *project, int emit_xrefs, int emit_explain,
                        ApexRenderedDocument *document)
{
    FILE *stream;
    long size;

    if (!document) {
        return 1;
    }
    clear_document(document);
    stream = tmpfile();
    if (!stream) {
        die("failed to create temporary render stream");
    }
    apex_project_write_asm_stream(project, stream, emit_xrefs, emit_explain);
    if (fflush(stream) != 0) {
        fclose(stream);
        die("failed to flush temporary render stream");
    }

    size = file_size(stream);
    if (size < 0L) {
        fclose(stream);
        die("failed to measure temporary render stream");
    }

    document->text = xmalloc((size_t)size + 1u);
    document->text_len = (size_t)size;
    if (size > 0L &&
        fread(document->text, 1u, (size_t)size, stream) != (size_t)size) {
        fclose(stream);
        clear_document(document);
        die("failed to read temporary render stream");
    }
    document->text[document->text_len] = '\0';
    fclose(stream);

    build_line_index(project, document);
    return 0;
}

void apex_render_document_free(ApexRenderedDocument *document)
{
    clear_document(document);
}

const ApexRenderedLine *apex_render_find_line_by_address(const ApexRenderedDocument *document,
                                                         uint8_t bank, uint32_t cpu_addr,
                                                         size_t *line_index)
{
    size_t i;

    if (!document) {
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
                                                         size_t *line_index)
{
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
                                                         size_t *line_index)
{
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
