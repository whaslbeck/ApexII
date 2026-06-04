#include "apex.h"
#include "apex_analysis.h"
#include "apex_config.h"
#include "apexdmd.h"
#include "apexsprite.h"
#include "apex_project.h"
#include "cpu6809.h"
#include "apexdis_api.h"
#include "apex_rominfo.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum {
    BLOCK_UNKNOWN,
    BLOCK_CODE,
    BLOCK_DATA,
    BLOCK_TABLE,
    BLOCK_UNCLASSIFIED,
    BLOCK_SPRITE
} BlockKind;

static void emit_vector_equates(FILE *out, const VectorInfo *vectors, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        fprintf(out, "%s_ADDR = 0x%04x\n", vectors[i].vector_name, vectors[i].vector_addr);
        fprintf(out, "%s_ADDR = 0x%04x\n", vectors[i].entry_name, vectors[i].target_addr);
    }
    fputc('\n', out);
}


static void add_inline_signature(InlineSignatures *sigs, uint32_t addr, unsigned length,
                                 TableFieldKind kind)
{
    add_inline_signature_ex(sigs, 0, 0, addr, length, kind);
}


static void emit_location_comment(FILE *out, const char *kind, uint8_t bank, uint32_t cpu_addr,
                                  size_t rom_addr)
{
    fprintf(out, "; %s bank=0x%02x cpu=0x%04x rom=0x%06lx\n", kind, bank,
            (unsigned)cpu_addr & 0xffffu, (unsigned long)rom_addr);
}

static const char *block_kind_name(BlockKind kind)
{
    if (kind == BLOCK_CODE) {
        return "code";
    }
    if (kind == BLOCK_TABLE) {
        return "table";
    }
    if (kind == BLOCK_DATA) {
        return "data";
    }
    if (kind == BLOCK_UNCLASSIFIED) {
        return "unclassified";
    }
    if (kind == BLOCK_SPRITE) {
        return "sprite";
    }
    return "unknown";
}

static void emit_transition_comment(FILE *out, BlockKind from, BlockKind to, uint8_t bank,
                                    uint32_t cpu_addr, size_t rom_addr)
{
    char kind[32];

    if (to == BLOCK_UNKNOWN || from == to) {
        return;
    }
    if (from == BLOCK_UNKNOWN) {
        emit_location_comment(out, block_kind_name(to), bank, cpu_addr, rom_addr);
        return;
    }
    snprintf(kind, sizeof(kind), "%s_to_%s", block_kind_name(from), block_kind_name(to));
    emit_location_comment(out, kind, bank, cpu_addr, rom_addr);
}

static void emit_doc_comment(FILE *out, const char *doc)
{
    const char *line = doc;

    while (line && *line) {
        const char *end = strchr(line, '\n');

        if (end) {
            fprintf(out, "; doc %.*s\n", (int)(end - line), line);
            line = end + 1;
        } else {
            fprintf(out, "; doc %s\n", line);
            break;
        }
    }
}

static void emit_explain_comment(FILE *out, BlockKind kind, const Label *label,
                                 const InlineSignature *inline_sig)
{
    int is_dmd = 0;
    int is_sprite = 0;

    if (label && label->explain) {
        fprintf(out, "; explain label source=%s\n", label->explain);
    }
    if (label && ((label->kind_explain && strstr(label->kind_explain, "dmd_fullframe")) ||
                  (label->explain && strstr(label->explain, "dmd_fullframe")))) {
        is_dmd = 1;
    }
    if (label && ((label->kind_explain && strstr(label->kind_explain, "sprite")) ||
                  (label->explain && strstr(label->explain, "sprite")))) {
        is_sprite = 1;
    }
    if (kind != BLOCK_UNKNOWN) {
        const char *source = label && label->kind_explain ? label->kind_explain :
                             label && label->explain      ? label->explain :
                                                            "scan";
        const char *kind_name = block_kind_name(kind);

        if (kind == BLOCK_TABLE) {
            source = label && label->kind_explain ? label->kind_explain : "config_table";
        } else if (kind == BLOCK_DATA && label && label->is_string) {
            source = label->kind_explain ? label->kind_explain :
                     label->explain      ? label->explain :
                                           "string_label";
        } else if (kind == BLOCK_DATA && label && label->is_data) {
            source = label->kind_explain ? label->kind_explain :
                     label->explain      ? label->explain :
                                           "data_label";
        }
        if (kind == BLOCK_DATA && is_dmd) {
            kind_name = "dmd_fullframe";
        }
        if (kind == BLOCK_DATA && is_sprite) {
            kind_name = "sprite";
        }
        fprintf(out, "; explain kind=%s source=%s\n", kind_name, source);
    }
    if (inline_sig) {
        fprintf(out, "; explain inline source=%s\n",
                inline_sig->has_bank ? "config_inline_banked" : "config_inline");
    }
}

static void emit_reference_comment(FILE *out, const ReferenceSet *refs, uint8_t bank, uint32_t addr)
{
    size_t i;
    int emitted = 0;

    if (!refs) {
        return;
    }
    /* refs are sorted+deduped by sort_and_dedup_refs; use binary search when sorted */
    i = refs->sorted ? refs_lower_bound(refs, bank, addr) : 0;
    for (; i < refs->count; i++) {
        if (refs->items[i].bank != bank || refs->items[i].addr != addr) {
            if (refs->sorted) break;
            continue;
        }
        if (refs->items[i].row_index >= 0) {
            uint32_t rc = refs->items[i].row_cpu_addr;
            uint8_t rb = refs->items[i].source_bank;
            fprintf(out, "%stable:%s line:%d[B%02x_A%04x]",
                    emitted ? ", " : "; referenced_by ",
                    refs->items[i].source, refs->items[i].row_index,
                    (unsigned)rb, (unsigned)rc & 0xffffu);
        } else {
            fprintf(out, "%s%s:%s", emitted ? ", " : "; referenced_by ",
                    refs->items[i].kind, refs->items[i].source);
        }
        emitted = 1;
    }
    if (emitted) {
        fputc('\n', out);
    }
}

static const char *table_field_name(TableFieldKind kind)
{
    if (kind == TABLE_BYTE) {
        return "byte";
    }
    if (kind == TABLE_WORD) {
        return "word";
    }
    if (kind == TABLE_PTR16_STRING) {
        return "ptr16_string";
    }
    if (kind == TABLE_PTR16_DATA) {
        return "ptr16_data";
    }
    if (kind == TABLE_PTR16_CODE) {
        return "ptr16_code";
    }
    if (kind == TABLE_PTR16_TABLE) {
        return "ptr16_table";
    }
    if (kind == TABLE_PTR16_DMD_FULLFRAME) {
        return "ptr16_dmd_fullframe";
    }
    if (kind == TABLE_PTR16_SPRITE) {
        return "ptr16_sprite";
    }
    if (kind == TABLE_FAR_STRING) {
        return "far_string";
    }
    if (kind == TABLE_FAR_DATA) {
        return "far_data";
    }
    if (kind == TABLE_FAR_TABLE) {
        return "far_table";
    }
    if (kind == TABLE_FAR_DMD_FULLFRAME) {
        return "far_dmd_fullframe";
    }
    if (kind == TABLE_FAR_SPRITE) {
        return "far_sprite";
    }
    return "far_code";
}

static void format_table_schema(const TableSchema *schema, char *out, size_t out_size)
{
    size_t i;
    size_t used = 0;

    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    for (i = 0; i < schema->count; i++) {
        int written;
        const char *sep = i == 0 ? "" : ", ";

        {
            const char *fname = schema->items[i].type_name
                                ? schema->items[i].type_name
                                : table_field_name(schema->items[i].kind);
            if (schema->items[i].count == 1) {
                written = snprintf(out + used, out_size - used, "%s%s", sep, fname);
            } else {
                written = snprintf(out + used, out_size - used, "%s%s[%lu]", sep,
                                   fname, (unsigned long)schema->items[i].count);
            }
        }
        if (written < 0 || (size_t)written >= out_size - used) {
            out[out_size - 1] = '\0';
            return;
        }
        used += (size_t)written;
    }
}

static const char *data_kind_name(DataKind kind)
{
    if (kind == DATA_STRING) {
        return "string";
    }
    if (kind == DATA_STRING_LP) {
        return "string_lp";
    }
    if (kind == DATA_STRING_FIXED) {
        return "string_fixed";
    }
    if (kind == DATA_DMD_FULLFRAME) {
        return "dmd_fullframe";
    }
    if (kind == DATA_PTR16_STRING) {
        return "ptr16_string";
    }
    if (kind == DATA_PTR16_DATA) {
        return "ptr16_data";
    }
    if (kind == DATA_PTR16_CODE) {
        return "ptr16_code";
    }
    if (kind == DATA_PTR16_TABLE) {
        return "ptr16_table";
    }
    if (kind == DATA_FAR_STRING) {
        return "far_string";
    }
    if (kind == DATA_FAR_DATA) {
        return "far_data";
    }
    if (kind == DATA_FAR_TABLE) {
        return "far_table";
    }
    if (kind == DATA_FAR_CODE) {
        return "far_code";
    }
    if (kind == DATA_FAR_DMD_FULLFRAME) {
        return "far_dmd_fullframe";
    }
    if (kind == DATA_SPRITE) {
        return "sprite";
    }
    if (kind == DATA_PTR16_SPRITE) {
        return "ptr16_sprite";
    }
    if (kind == DATA_FAR_SPRITE) {
        return "far_sprite";
    }
    if (kind == DATA_SPRITE_NOHEADER) {
        return "sprite_noheader";
    }
    return "bytes";
}

static int data_kind_is_far(DataKind kind)
{
    return kind == DATA_FAR_STRING || kind == DATA_FAR_DATA || kind == DATA_FAR_TABLE ||
           kind == DATA_FAR_CODE || kind == DATA_FAR_DMD_FULLFRAME || kind == DATA_FAR_SPRITE;
}

static int data_kind_is_ptr16(DataKind kind)
{
    return kind == DATA_PTR16_STRING || kind == DATA_PTR16_DATA ||
           kind == DATA_PTR16_CODE  || kind == DATA_PTR16_TABLE || kind == DATA_PTR16_SPRITE;
}

static TableFieldKind data_ptr16_to_table_kind(DataKind kind)
{
    if (kind == DATA_PTR16_CODE)   return TABLE_PTR16_CODE;
    if (kind == DATA_PTR16_STRING) return TABLE_PTR16_STRING;
    if (kind == DATA_PTR16_TABLE)  return TABLE_PTR16_TABLE;
    if (kind == DATA_PTR16_SPRITE) return TABLE_PTR16_SPRITE;
    return TABLE_PTR16_DATA;
}

static int locate_bank_bytes(const uint8_t *paged_rom, size_t banks, uint8_t bank, uint16_t addr,
                             const uint8_t **src, size_t *len)
{
    size_t offset;
    int bank_index;

    if (bank == 0xffu) {
        if (!in_system_addr(addr)) {
            return 0;
        }
        offset = banks * APEX_BANK_SIZE + (size_t)(addr - APEX_SYSTEM_ORG);
    } else {
        if (addr < APEX_PAGED_ORG || addr >= 0x8000u) {
            return 0;
        }
        bank_index = bank_index_for_far_ref(paged_rom, banks, bank);
        if (bank_index < 0) {
            return 0;
        }
        offset = (size_t)bank_index * APEX_BANK_SIZE + (size_t)(addr - APEX_PAGED_ORG);
    }
    *src = paged_rom + offset;
    *len = banks * APEX_BANK_SIZE + APEX_SYSTEM_SIZE - offset;
    return 1;
}

static int decode_dmd_fullframe_metadata(const uint8_t *src, size_t len, uint8_t *decoder_type,
                                         size_t *consumed)
{
    uint8_t plane[APEX_DMD_PAGE_BYTES];
    size_t used = 0;
    uint8_t type = 0;

    if (!apexdmd_decode_fullframe(src, len, plane, &used, &type)) {
        return 0;
    }
    if (decoder_type) {
        *decoder_type = type;
    }
    if (consumed) {
        *consumed = used;
    }
    return 1;
}

static void emit_dmd_fullframe_comment(FILE *out, const uint8_t *src, size_t len)
{
    uint8_t type = 0;
    size_t consumed = 0;

    if (decode_dmd_fullframe_metadata(src, len, &type, &consumed)) {
        fprintf(out, " ; dmd type=fullframe decoder=0x%02x consumed=%lu width=%u height=%u",
                (unsigned)type, (unsigned long)consumed, (unsigned)APEX_DMD_WIDTH,
                (unsigned)APEX_DMD_HEIGHT);
    } else {
        fputs(" ; dmd type=fullframe invalid=1", out);
    }
}

static void emit_dmd_fullframe_target_comment(FILE *out, const uint8_t *paged_rom, size_t banks,
                                              uint8_t bank, uint16_t addr)
{
    const uint8_t *src;
    size_t len;

    if (locate_bank_bytes(paged_rom, banks, bank, addr, &src, &len)) {
        emit_dmd_fullframe_comment(out, src, len);
    } else {
        fputs(" ; dmd type=fullframe unmapped=1", out);
    }
}

static void emit_sprite_comment(FILE *out, const uint8_t *src, size_t len)
{
    uint8_t hdr = 0, vert = 0, horiz = 0, width = 0, height = 0, enc_type = 0;
    size_t consumed = 0;
    uint8_t tmp[APEX_SPRITE_MAX_BYTES];

    if (apexsprite_decode(src, len, tmp, &hdr, &vert, &horiz, &width, &height, &enc_type, &consumed)) {
        fprintf(out, " ; sprite hdr=0x%02x vert=%u horiz=%u width=%u height=%u consumed=%lu",
                (unsigned)hdr, (unsigned)vert, (unsigned)horiz,
                (unsigned)width, (unsigned)height, (unsigned long)consumed);
    } else {
        fputs(" ; sprite invalid=1", out);
    }
}

static void emit_sprite_noheader_comment(FILE *out, const uint8_t *src, size_t len,
                                         uint8_t table_height)
{
    uint8_t width = 0;
    size_t consumed = 0;
    uint8_t tmp[APEX_SPRITE_MAX_BYTES];

    if (apexsprite_decode_noheader(src, len, tmp, table_height, &width, &consumed)) {
        fprintf(out, " ; sprite_noheader width=%u height=%u consumed=%lu",
                (unsigned)width, (unsigned)table_height, (unsigned long)consumed);
    } else {
        fputs(" ; sprite_noheader invalid=1", out);
    }
}

static void emit_sprite_target_comment(FILE *out, const uint8_t *paged_rom, size_t banks,
                                       uint8_t bank, uint16_t addr)
{
    const uint8_t *src;
    size_t len;

    if (!locate_bank_bytes(paged_rom, banks, bank, addr, &src, &len)) {
        fputs(" ; sprite unmapped=1", out);
        return;
    }
    /* No-header VSI: byte 0 is width (1..128); header format uses 0x00/0xFD/0xFE/0xFF */
    if (apexsprite_is_noheader(src, len)) {
        fprintf(out, " ; sprite_noheader width=%u", (unsigned)src[0]);
    } else {
        emit_sprite_comment(out, src, len);
    }
}

static void emit_routine_comment_block(FILE *out, uint8_t bank, uint32_t addr, uint32_t base_addr,
                                       size_t rom_base, const InlineSignature *inline_sig,
                                       const char *doc, const ReferenceSet *refs)
{
    fputc('\n', out);
    emit_location_comment(out, "label", bank, addr, rom_base + (size_t)(addr - base_addr));
    emit_reference_comment(out, refs, bank, addr);
    fprintf(out, "; kind routine\n");
    if (inline_sig) {
        char schema_text[128];

        format_table_schema(&inline_sig->schema, schema_text, sizeof(schema_text));
        fprintf(out, "; inline length=%u\n", inline_sig->length);
        fprintf(out, "; inline params=%s\n", schema_text);
    }
    emit_doc_comment(out, doc);
}

static void emit_data_comment_block(FILE *out, uint8_t bank, uint32_t addr, uint32_t base_addr,
                                    size_t rom_base, const DataRange *range,
                                    const ReferenceSet *refs, const uint8_t *paged_rom,
                                    size_t banks)
{
    fputc('\n', out);
    emit_location_comment(out, "label", bank, addr, rom_base + (size_t)(addr - base_addr));
    emit_reference_comment(out, refs, bank, addr);
    if (range->kind == DATA_SPRITE || range->kind == DATA_SPRITE_NOHEADER ||
        range->kind == DATA_FAR_SPRITE) {
        fprintf(out, "; kind sprite\n");
    } else {
        fprintf(out, "; kind data\n");
    }
    if (range->kind == DATA_BYTES) {
        fprintf(out, "; data type=%s length=%lu\n", data_kind_name(range->kind),
                (unsigned long)range->length);
    } else if (range->kind == DATA_SPRITE_NOHEADER) {
        fprintf(out, "; data type=sprite_noheader[%lu]\n", (unsigned long)range->length);
    } else {
        fprintf(out, "; data type=%s\n", data_kind_name(range->kind));
    }
    if (range->kind == DATA_DMD_FULLFRAME) {
        const uint8_t *src;
        size_t len;

        if (locate_bank_bytes(paged_rom, banks, bank, (uint16_t)addr, &src, &len)) {
            emit_dmd_fullframe_comment(out, src, len);
            fputc('\n', out);
        }
    }
    if (range->kind == DATA_SPRITE) {
        const uint8_t *src;
        size_t len;

        if (locate_bank_bytes(paged_rom, banks, bank, (uint16_t)addr, &src, &len)) {
            emit_sprite_comment(out, src, len);
            fputc('\n', out);
        }
    }
    if (range->kind == DATA_SPRITE_NOHEADER) {
        const uint8_t *src;
        size_t len;

        if (locate_bank_bytes(paged_rom, banks, bank, (uint16_t)addr, &src, &len)) {
            emit_sprite_noheader_comment(out, src, len, (uint8_t)range->length);
            fputc('\n', out);
        }
    }
}

static int label_is_dmd_fullframe(const Label *label)
{
    if (!label) {
        return 0;
    }
    return (label->kind_explain && strstr(label->kind_explain, "dmd_fullframe")) ||
           (label->explain && strstr(label->explain, "dmd_fullframe"));
}

static int label_is_sprite(const Label *label)
{
    if (!label) {
        return 0;
    }
    return (label->kind_explain && strstr(label->kind_explain, "sprite")) ||
           (label->explain && strstr(label->explain, "sprite"));
}

static void emit_table_comment_block(FILE *out, uint8_t bank, uint32_t addr, uint32_t base_addr,
                                     size_t rom_base, const TableDef *table, const uint8_t *data,
                                     size_t len, size_t pos, const char *doc,
                                     const ReferenceSet *refs)
{
    size_t rows = table->rows;
    unsigned row_width = (unsigned)table_schema_width(&table->schema);
    char row_format[256];

    if (table->has_header) {
        if (pos + 3u <= len) {
            rows = read_be16(data + pos);
            row_width = data[pos + 2u];
        } else {
            rows = 0;
            row_width = 0;
        }
    }
    format_table_schema(&table->schema, row_format, sizeof(row_format));
    fputc('\n', out);
    emit_location_comment(out, "label", bank, addr, rom_base + (size_t)(addr - base_addr));
    emit_reference_comment(out, refs, bank, addr);
    fprintf(out, "; kind table\n");
    fprintf(out, "; table rows=%lu row_width=%u row_format=%s\n", (unsigned long)rows, row_width,
            row_format);
    emit_doc_comment(out, doc);
}

static void emit_conflict_warning(FILE *out, uint8_t bank, uint32_t cpu_addr, size_t rom_addr,
                                   const char *code_from, const char *data_from)
{
    const char *cf = code_from ? code_from : "unknown";
    const char *df = data_from ? data_from : "unknown";

    fprintf(stderr,
            "warning: classification conflict at bank=0x%02x cpu=0x%04x rom=0x%06lx: "
            "code_from=%s data_from=%s\n",
            bank, (unsigned)cpu_addr & 0xffffu, (unsigned long)rom_addr, cf, df);
    fprintf(out,
            "; WARNING classification_conflict bank=0x%02x cpu=0x%04x rom=0x%06lx "
            "code_from=%s data_from=%s\n",
            bank, (unsigned)cpu_addr & 0xffffu, (unsigned long)rom_addr, cf, df);
}

static void emit_labels_at(FILE *out, uint32_t addr, const Label *labels, size_t label_count,
                           int labels_sorted,
                           uint8_t bank, uint32_t base_addr, size_t rom_base,
                           const InlineSignatures *inline_sigs, const TableDef *table,
                           const DataRange *data_range,
                           const uint8_t *data, size_t len, size_t pos,
                           const ConfigDocs *docs,
                           const ReferenceSet *refs, int emit_explain,
                           const uint8_t *paged_rom, size_t banks)
{
    size_t i;
    int emitted_block = 0;
    int emitted_label = 0;
    int emitted_conflict = 0;
    const InlineSignature *inline_sig = inline_signature_for(inline_sigs, bank, addr);

    i = labels_sorted ? label_lower_bound(labels, label_count, addr) : 0;
    for (; i < label_count; i++) {
        if (labels[i].addr != addr) {
            if (labels_sorted) break;
            continue;
        }
        if (!emitted_block && table) {
                emit_table_comment_block(out, bank, addr, base_addr, rom_base, table, data, len, pos,
                                         config_doc_at(docs, bank, addr), refs);
                if (emit_explain) {
                    emit_explain_comment(out, BLOCK_TABLE, &labels[i], NULL);
                }
                emitted_block = 1;
            } else if (!emitted_block && data_range) {
                emit_data_comment_block(out, bank, addr, base_addr, rom_base, data_range, refs,
                                        paged_rom, banks);
                if (emit_explain) {
                    emit_explain_comment(out, BLOCK_DATA, &labels[i], NULL);
                }
                emitted_block = 1;
            } else if (!emitted_block && labels[i].is_code) {
                emit_routine_comment_block(out, bank, addr, base_addr, rom_base, inline_sig,
                                           config_doc_at(docs, bank, addr), refs);
                if (emit_explain) {
                    emit_explain_comment(out, BLOCK_CODE, &labels[i], inline_sig);
                }
                emitted_block = 1;
            } else if (!emitted_block) {
                emit_location_comment(out, "label", bank, addr,
                                      rom_base + (size_t)(addr - base_addr));
                emit_reference_comment(out, refs, bank, addr);
                if (label_is_dmd_fullframe(&labels[i])) {
                    const uint8_t *src;
                    size_t meta_len;

                    fprintf(out, "; kind data\n");
                    fprintf(out, "; data type=dmd_fullframe\n");
                    if (locate_bank_bytes(paged_rom, banks, bank, (uint16_t)addr, &src, &meta_len)) {
                        emit_dmd_fullframe_comment(out, src, meta_len);
                        fputc('\n', out);
                    }
                }
                if (label_is_sprite(&labels[i])) {
                    const uint8_t *src;
                    size_t meta_len;

                    fprintf(out, "; kind data\n");
                    fprintf(out, "; data type=sprite\n");
                    if (locate_bank_bytes(paged_rom, banks, bank, (uint16_t)addr, &src, &meta_len)) {
                        emit_sprite_comment(out, src, meta_len);
                        fputc('\n', out);
                    }
                }
                if (emit_explain) {
                    emit_explain_comment(out, labels[i].is_data || labels[i].is_string ? BLOCK_DATA :
                                                                              BLOCK_UNKNOWN,
                                         &labels[i], inline_sig);
                }
                emitted_block = 1;
            }
            if (labels[i].is_conflict && !emitted_conflict) {
                emit_conflict_warning(out, bank, addr,
                                      rom_base + (size_t)(addr - base_addr),
                                      labels[i].explain, labels[i].kind_explain);
                emitted_conflict = 1;
            }
            fprintf(out, "%s:\n", labels[i].name);
            emitted_label = 1;
    }
    if (!emitted_label && refs) {
        size_t ri = refs->sorted ? refs_lower_bound(refs, bank, addr) : 0;

        for (; ri < refs->count; ri++) {
            if (refs->items[ri].bank == bank && refs->items[ri].addr == addr) {
                if (!emitted_block) {
                    emit_location_comment(out, "label", bank, addr,
                                          rom_base + (size_t)(addr - base_addr));
                    emit_reference_comment(out, refs, bank, addr);
                }
                fprintf(out, "%s:\n",
                        bank == 0xffu ? make_generated_label(addr) :
                                        make_bank_label(bank, (uint16_t)addr));
                break;
            }
            if (refs->sorted && (refs->items[ri].bank > bank ||
                                  (refs->items[ri].bank == bank && refs->items[ri].addr > addr))) {
                break;
            }
        }
    }
}

static void emit_config_symbols(FILE *out, const ConfigSymbols *symbols)
{
    size_t i;

    if (!symbols || symbols->count == 0) {
        return;
    }
    for (i = 0; i < symbols->count; i++) {
        fprintf(out, "%s = 0x%04x\n", symbols->items[i].name,
                (unsigned)symbols->items[i].value & 0xffffu);
    }
    fputc('\n', out);
}

static void emit_type_equates(FILE *out, const ConfigTypes *types)
{
    size_t i, j;
    int emitted = 0;

    if (!types || types->count == 0) {
        return;
    }
    for (i = 0; i < types->count; i++) {
        const ConfigType *type = &types->items[i];
        char upper_type[128];
        size_t k;

        for (k = 0; type->name[k] && k < sizeof(upper_type) - 1u; k++) {
            upper_type[k] = (char)toupper((unsigned char)type->name[k]);
        }
        upper_type[k] = '\0';
        for (j = 0; j < type->value_count; j++) {
            char upper_val[128];

            for (k = 0; type->values[j].name[k] && k < sizeof(upper_val) - 1u; k++) {
                upper_val[k] = (char)toupper((unsigned char)type->values[j].name[k]);
            }
            upper_val[k] = '\0';
            fprintf(out, "%s_%s = 0x%02x\n", upper_type, upper_val,
                    (unsigned)type->values[j].value & 0xffu);
            emitted = 1;
        }
    }
    if (emitted) {
        fputc('\n', out);
    }
}

static void emit_inline_truncated_warning(FILE *out, uint8_t bank, uint32_t cpu_addr,
                                          size_t rom_addr, const char *inst, unsigned expected,
                                          size_t available)
{
    fprintf(stderr,
            "warning: inline data truncated after %s at bank=0x%02x cpu=0x%04x rom=0x%06lx: "
            "expected %u byte(s), available %lu\n",
            inst, bank, (unsigned)cpu_addr & 0xffffu, (unsigned long)rom_addr, expected,
            (unsigned long)available);
    fprintf(out,
            "; WARNING inline_truncated bank=0x%02x cpu=0x%04x rom=0x%06lx expected=%u "
            "available=%lu for %s\n",
            bank, (unsigned)cpu_addr & 0xffffu, (unsigned long)rom_addr, expected,
            (unsigned long)available, inst);
}

static void emit_inline_far_warning(FILE *out, uint8_t current_bank, uint32_t cpu_addr,
                                    size_t rom_addr, const char *inst, uint16_t target,
                                    uint8_t target_bank)
{
    fprintf(stderr,
            "warning: invalid inline far-code target after %s at bank=0x%02x cpu=0x%04x "
            "rom=0x%06lx: target=0x%04x bank=0x%02x\n",
            inst, current_bank, (unsigned)cpu_addr & 0xffffu, (unsigned long)rom_addr, target,
            target_bank);
    fprintf(out,
            "; WARNING inline_far_code_invalid bank=0x%02x cpu=0x%04x rom=0x%06lx "
            "target=0x%04x target_bank=0x%02x for %s\n",
            current_bank, (unsigned)cpu_addr & 0xffffu, (unsigned long)rom_addr, target,
            target_bank, inst);
}

static void emit_string(FILE *out, const uint8_t *data, size_t len)
{
    size_t i;

    fprintf(out, "    STRING \"");
    for (i = 0; i + 1u < len; i++) {
        if (data[i] == '"' || data[i] == '\\') {
            fputc('\\', out);
        }
        fputc(data[i], out);
    }
    fprintf(out, "\"\n");
}

/* Returns 1 + N (length byte + N chars) if data[0..N] is a valid LP string, else 0. */
static size_t valid_string_lp_len(const uint8_t *data, size_t len)
{
    size_t n, i;
    if (len == 0) return 0;
    n = data[0];
    if (n == 0 || 1u + n > len) return 0;
    for (i = 1u; i <= n; i++) {
        if (data[i] < 0x20u || data[i] > 0x7fu) return 0;
    }
    return 1u + n;
}

/* Emits STRING_LP "…" (data[0] is the length byte, data[1..len-1] are the chars). */
static void emit_string_lp(FILE *out, const uint8_t *data, size_t len)
{
    size_t i;
    fprintf(out, "    STRING_LP \"");
    for (i = 1u; i < len; i++) {
        if (data[i] == '"' || data[i] == '\\') fputc('\\', out);
        fputc(data[i], out);
    }
    fprintf(out, "\"\n");
}

/* Emits STRING_FIXED "…" (data[0..len-1] are the chars, no extra byte). */
static void emit_string_fixed(FILE *out, const uint8_t *data, size_t len)
{
    size_t i;
    fprintf(out, "    STRING_FIXED \"");
    for (i = 0; i < len; i++) {
        if (data[i] == '"' || data[i] == '\\') fputc('\\', out);
        fputc(data[i], out);
    }
    fprintf(out, "\"\n");
}

static void emit_inner_string_label_equates(FILE *out, uint32_t start, size_t len,
                                            const Label *labels, size_t label_count)
{
    const char *start_name = label_name_at(start, labels, label_count, 0);
    size_t i;

    if (!start_name) {
        return;
    }
    for (i = 0; i < label_count; i++) {
        if (labels[i].is_string && labels[i].addr > start && labels[i].addr < start + len) {
            fprintf(out, "%s = %s + 0x%04x\n", labels[i].name, start_name,
                    (unsigned)(labels[i].addr - start));
        }
    }
}

static const char *table_ptr_label(uint16_t ptr, uint8_t bank,
                                   const Label *labels,
                                   size_t label_count, const Label *extra_labels,
                                   size_t extra_label_count)
{
    const char *label = label_name_at(ptr, labels, label_count, 0);

    if (!label) {
        label = label_name_at(ptr, extra_labels, extra_label_count, 0);
    }
    if (!label && ptr >= APEX_PAGED_ORG && ptr < 0x8000u) {
        label = make_bank_label(bank, ptr);
    }
    if (!label && in_system_addr(ptr)) {
        label = make_generated_label(ptr);
    }
    return label;
}

static void emit_table_ptr_field(FILE *out, const uint8_t *data, size_t len,
                                 size_t *pos, uint8_t bank, TableFieldKind kind, const Label *labels,
                                 size_t label_count, const Label *extra_labels,
                                 size_t extra_label_count, const uint8_t *paged_rom, size_t banks)
{
    uint16_t ptr;
    const char *label;

    if (*pos + 2u > len) {
        return;
    }
    ptr = read_be16(data + *pos);
    label = table_ptr_label(ptr, bank, labels, label_count, extra_labels, extra_label_count);
    {
        const char *pseudo = kind == TABLE_PTR16_DMD_FULLFRAME ? "TABLE_PTR_DMD_FULLFRAME" :
                             kind == TABLE_PTR16_SPRITE         ? "TABLE_PTR_SPRITE"         :
                                                                  "TABLE_PTR";
        if (label) {
            fprintf(out, "    %s %s", pseudo, label);
        } else {
            fprintf(out, "    %s 0x%04x", pseudo, ptr);
        }
    }
    if (kind == TABLE_PTR16_DMD_FULLFRAME) {
        emit_dmd_fullframe_target_comment(out, paged_rom, banks, bank, ptr);
    }
    if (kind == TABLE_PTR16_SPRITE) {
        emit_sprite_target_comment(out, paged_rom, banks, bank, ptr);
    }
    fputc('\n', out);
    *pos += 2u;
}

static void emit_db_line(FILE *out, uint32_t addr, const uint8_t *data, size_t count);

static void emit_table_rows(FILE *out, const TableDef *table, const uint8_t *data, size_t len,
                            uint32_t base_addr, size_t *pos, uint8_t current_bank, size_t rows,
                            size_t row_width, const Label *labels, size_t label_count,
                            const Label *extra_labels, size_t extra_label_count,
                            const uint8_t *paged_rom, size_t banks, const LabelSet *bank_labels,
                            const ConfigTypes *types, const ConfigDocs *docs);

static void emit_counted_table(FILE *out, const TableDef *table, const uint8_t *data, size_t len,
                               uint32_t base_addr, size_t *pos, uint8_t bank, const Label *labels,
                               size_t label_count, const Label *extra_labels,
                               size_t extra_label_count, const uint8_t *paged_rom, size_t banks,
                               const LabelSet *bank_labels, const ConfigTypes *types,
                               const ConfigDocs *docs)
{
    uint16_t count;
    uint8_t row_width;

    if (*pos + 3u > len) {
        return;
    }
    count = read_be16(data + *pos);
    fprintf(out, "    .DW 0x%04x\n", count);
    row_width = data[*pos + 2u];
    emit_db_line(out, base_addr + (uint32_t)(*pos + 2u), data + *pos + 2u, 1);
    *pos += 3u;
    emit_table_rows(out, table, data, len, base_addr, pos, bank, count, row_width, labels,
                    label_count, extra_labels, extra_label_count, paged_rom, banks, bank_labels,
                    types, docs);
}

static void emit_far_code_ref(FILE *out, const char *pseudo, uint16_t addr, uint8_t bank,
                              const Label *labels, size_t label_count,
                              const Label *extra_labels, size_t extra_label_count,
                              const uint8_t *paged_rom, size_t banks,
                              const LabelSet *bank_labels)
{
    const char *label = NULL;
    int generated = 0;
    uint8_t label_bank = bank;

    if (bank == 0xffu && in_system_addr(addr)) {
        label = label_name_at(addr, labels, label_count, 0);
        if (!label) {
            label = label_name_at(addr, extra_labels, extra_label_count, 0);
        }
        if (!label) {
            label = make_generated_label(addr);
            generated = 1;
        }
    } else if (paged_rom && bank_labels) {
        int bank_index = bank_index_for_far_ref(paged_rom, banks, bank);

        if (bank_index >= 0) {
            label_bank = bank_id_for_index(paged_rom, bank_index);
            label = label_name_at(addr, bank_labels[bank_index].items, bank_labels[bank_index].count, 0);
        }
        if (!label) {
            label = make_bank_label(label_bank, addr);
            generated = 1;
        }
    } else {
        label = make_bank_label(bank, addr);
        generated = 1;
    }

    if (label_bank == bank && (generated || generated_label_name(label) || !generated)) {
        fprintf(out, "    %s %s", pseudo, label);
    } else {
        fprintf(out, "    %s %s, 0x%02x", pseudo, label, bank);
    }
}

static const char *table_far_pseudo(TableFieldKind kind)
{
    if (kind == TABLE_FAR_STRING) {
        return "TABLE_FAR_STRING";
    }
    if (kind == TABLE_FAR_TABLE) {
        return "TABLE_FAR_TABLE";
    }
    if (kind == TABLE_FAR_CODE) {
        return "TABLE_FAR_CODE";
    }
    if (kind == TABLE_FAR_DMD_FULLFRAME) {
        return "TABLE_FAR_DMD_FULLFRAME";
    }
    if (kind == TABLE_FAR_SPRITE) {
        return "TABLE_FAR_SPRITE";
    }
    return "TABLE_FAR_PTR";
}

static const char *inline_far_pseudo(TableFieldKind kind)
{
    if (kind == TABLE_FAR_STRING) {
        return "INLINE_FAR_STRING";
    }
    if (kind == TABLE_FAR_TABLE) {
        return "INLINE_FAR_TABLE";
    }
    if (kind == TABLE_FAR_CODE) {
        return "INLINE_FAR_CODE";
    }
    if (kind == TABLE_FAR_DMD_FULLFRAME) {
        return "INLINE_FAR_DMD_FULLFRAME";
    }
    if (kind == TABLE_FAR_SPRITE) {
        return "INLINE_FAR_SPRITE";
    }
    return "INLINE_FAR_PTR";
}

static const char *inline_ptr_pseudo(TableFieldKind kind)
{
    if (kind == TABLE_PTR16_STRING) {
        return "INLINE_STRING_PTR";
    }
    if (kind == TABLE_PTR16_TABLE) {
        return "INLINE_TABLE_PTR";
    }
    if (kind == TABLE_PTR16_CODE) {
        return "INLINE_CODE_PTR";
    }
    if (kind == TABLE_PTR16_DMD_FULLFRAME) {
        return "INLINE_PTR_DMD_FULLFRAME";
    }
    if (kind == TABLE_PTR16_SPRITE) {
        return "INLINE_PTR_SPRITE";
    }
    return "INLINE_PTR";
}

static const char *data_far_pseudo(DataKind kind)
{
    if (kind == DATA_FAR_STRING) {
        return "FAR_STRING";
    }
    if (kind == DATA_FAR_TABLE) {
        return "FAR_TABLE";
    }
    if (kind == DATA_FAR_CODE) {
        return "FAR_CODE";
    }
    if (kind == DATA_FAR_DMD_FULLFRAME) {
        return "FAR_DMD_FULLFRAME";
    }
    if (kind == DATA_FAR_SPRITE) {
        return "FAR_SPRITE";
    }
    return "FAR_PTR";
}

static char db_ascii_char(uint8_t value)
{
    return value >= 0x20u && value <= 0x7eu ? (char)value : '.';
}

static void emit_db_ascii_comment(FILE *out, uint32_t addr, const uint8_t *data, size_t count)
{
    size_t i;

    fprintf(out, " ; 0x%04x |", (unsigned)addr & 0xffffu);
    for (i = 0; i < count; i++) {
        fputc(db_ascii_char(data[i]), out);
    }
    fputs("|\n", out);
}

static void emit_db_line(FILE *out, uint32_t addr, const uint8_t *data, size_t count)
{
    size_t i;

    fprintf(out, "    .DB ");
    for (i = 0; i < count; i++) {
        fprintf(out, "%s0x%02x", i == 0 ? "" : ", ", data[i]);
    }
    emit_db_ascii_comment(out, addr, data, count);
}

static void emit_data_bytes(FILE *out, const uint8_t *data, size_t len, uint32_t base_addr,
                            size_t *pos, size_t count);


static void emit_table_rows(FILE *out, const TableDef *table, const uint8_t *data, size_t len,
                            uint32_t base_addr, size_t *pos, uint8_t current_bank, size_t rows,
                            size_t row_width, const Label *labels, size_t label_count,
                            const Label *extra_labels, size_t extra_label_count,
                            const uint8_t *paged_rom, size_t banks, const LabelSet *bank_labels,
                            const ConfigTypes *types, const ConfigDocs *docs)
{
    size_t row;

    for (row = 0; row < rows && *pos + row_width <= len; row++) {
        size_t row_start = *pos;
        size_t i;

        /* Labels at row boundaries after the first: the outer emit_db_with_labels
           loop already called emit_labels_at for the table's own start address
           (row 0).  Rows 1..N are consumed entirely within this function, so
           their addresses are never visited by the outer loop.  Emit any named
           label definitions here so the assembler can resolve references to them. */
        if (row > 0) {
            uint32_t row_addr = base_addr + (uint32_t)row_start;
            size_t li;

            for (li = 0; li < label_count; li++) {
                if (labels[li].addr == row_addr) {
                    fprintf(out, "%s:\n", labels[li].name);
                }
            }
            for (li = 0; li < extra_label_count; li++) {
                if (extra_labels[li].addr == row_addr) {
                    fprintf(out, "%s:\n", extra_labels[li].name);
                }
            }
        }

        fprintf(out, "; [row %lu]\n", (unsigned long)row);
        emit_doc_comment(out, config_doc_at(docs, current_bank, base_addr + (uint32_t)row_start));

        for (i = 0; i < table->schema.count; i++) {
            size_t n;
            const TableField *field = &table->schema.items[i];

            /* Untyped bytes: group all field->count bytes into one .DB line. */
            if (field->kind == TABLE_BYTE && !field->type_name) {
                if (*pos + field->count <= len) {
                    emit_db_line(out, base_addr + (uint32_t)*pos, data + *pos, field->count);
                    *pos += field->count;
                }
                continue;
            }

            for (n = 0; n < field->count; n++) {
                TableFieldKind kind = field->kind;

                if (kind == TABLE_BYTE) {
                    uint8_t byte_val = data[*pos];

                    if (field->type_name && types) {
                        const char *ename = config_type_enum_name(types, field->type_name, byte_val);

                        fprintf(out, "    .DB 0x%02x ; 0x%04x |%c|", byte_val,
                                (unsigned)(base_addr + (uint32_t)*pos) & 0xffffu,
                                db_ascii_char(byte_val));
                        if (ename) {
                            fprintf(out, " %s=%s", field->type_name, ename);
                        } else {
                            fprintf(out, " %s=0x%02x", field->type_name, byte_val);
                        }
                        fputc('\n', out);
                    }
                    (*pos)++;
                } else if (kind == TABLE_WORD) {
                    uint16_t word_val = read_be16(data + *pos);

                    if (field->type_name && types) {
                        const char *ename = config_type_enum_name(types, field->type_name, word_val);

                        fprintf(out, "    .DW 0x%04x ;", word_val);
                        if (ename) {
                            fprintf(out, " %s=%s", field->type_name, ename);
                        } else {
                            fprintf(out, " %s=0x%04x", field->type_name, word_val);
                        }
                        fputc('\n', out);
                    } else {
                        fprintf(out, "    .DW 0x%04x\n", word_val);
                    }
                    *pos += 2u;
                } else if (table_kind_is_far(kind)) {
                    uint16_t addr = read_be16(data + *pos);
                    uint8_t bank = data[*pos + 2u];

                    emit_far_code_ref(out, table_far_pseudo(kind), addr, bank, labels, label_count,
                                      extra_labels, extra_label_count, paged_rom, banks,
                                      bank_labels);
                    if (kind == TABLE_FAR_DMD_FULLFRAME) {
                        emit_dmd_fullframe_target_comment(out, paged_rom, banks, bank, addr);
                    }
                    if (kind == TABLE_FAR_SPRITE) {
                        emit_sprite_target_comment(out, paged_rom, banks, bank, addr);
                    }
                    fputc('\n', out);
                    *pos += 3u;
                } else {
                    emit_table_ptr_field(out, data, len, pos, current_bank, kind, labels,
                                         label_count, extra_labels, extra_label_count, paged_rom,
                                         banks);
                }
            }
        }
        if (*pos < row_start + row_width) {
            emit_data_bytes(out, data, len, base_addr, pos, row_start + row_width - *pos);
        }
    }
}

static void emit_data_bytes(FILE *out, const uint8_t *data, size_t len, uint32_t base_addr,
                            size_t *pos, size_t count)
{
    size_t end = *pos + count;

    if (end > len) {
        end = len;
    }
    while (*pos < end) {
        size_t col = 0;
        size_t line_start = *pos;

        while (*pos < end && col < 16) {
            (*pos)++;
            col++;
        }
        emit_db_line(out, base_addr + (uint32_t)line_start, data + line_start, col);
    }
}

static int emit_data_range(FILE *out, const DataRange *range, const uint8_t *data, size_t len,
                           uint32_t base_addr, size_t *pos, const Label *labels, size_t label_count,
                           const Label *extra_labels, size_t extra_label_count,
                           const uint8_t *paged_rom, size_t banks,
                           const LabelSet *bank_labels)
{
    if (range->kind == DATA_STRING) {
        size_t string_len = valid_string_len(data + *pos, len - *pos);

        if (string_len == 0) {
            return 0;
        }
        emit_string(out, data + *pos, string_len);
        *pos += string_len;
        return 1;
    }
    if (range->kind == DATA_STRING_LP) {
        size_t string_len = valid_string_lp_len(data + *pos, len - *pos);

        if (string_len == 0) {
            return 0;
        }
        emit_string_lp(out, data + *pos, string_len);
        *pos += string_len;
        return 1;
    }
    if (range->kind == DATA_STRING_FIXED) {
        size_t n = range->length;
        size_t i;

        if (n == 0 || *pos + n > len) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            if (data[*pos + i] < 0x20u || data[*pos + i] > 0x7fu) {
                return 0;
            }
        }
        emit_string_fixed(out, data + *pos, n);
        *pos += n;
        return 1;
    }
    if (range->kind == DATA_PTR16_STRING || range->kind == DATA_PTR16_DATA ||
        range->kind == DATA_PTR16_CODE  || range->kind == DATA_PTR16_TABLE) {
        uint16_t ptr;
        const char *lbl;

        if (*pos + 2u > len) {
            return 0;
        }
        ptr = read_be16(data + *pos);
        lbl = table_ptr_label(ptr, range->bank, labels, label_count,
                              extra_labels, extra_label_count);
        if (lbl) {
            fprintf(out, "    .DW %s\n", lbl);
        } else {
            fprintf(out, "    .DW 0x%04x\n", ptr);
        }
        *pos += 2u;
        return 1;
    }
    if (data_kind_is_far(range->kind)) {
        if (*pos + 3u > len) {
            return 0;
        }
        emit_far_code_ref(out, data_far_pseudo(range->kind), read_be16(data + *pos),
                          data[*pos + 2u], labels, label_count, extra_labels, extra_label_count,
                          paged_rom, banks, bank_labels);
        if (range->kind == DATA_FAR_DMD_FULLFRAME) {
            emit_dmd_fullframe_target_comment(out, paged_rom, banks, data[*pos + 2u],
                                              read_be16(data + *pos));
        }
        if (range->kind == DATA_FAR_SPRITE) {
            emit_sprite_target_comment(out, paged_rom, banks, data[*pos + 2u],
                                       read_be16(data + *pos));
        }
        fputc('\n', out);
        *pos += 3u;
        return 1;
    }
    if (range->kind == DATA_DMD_FULLFRAME) {
        size_t consumed = 0;
        uint8_t type = 0;

        if (!decode_dmd_fullframe_metadata(data + *pos, len - *pos, &type, &consumed) ||
            consumed == 0u) {
            consumed = 1u;
        }
        emit_data_bytes(out, data, len, base_addr, pos, consumed);
        return 1;
    }
    if (range->kind == DATA_SPRITE) {
        uint8_t tmp[APEX_SPRITE_MAX_BYTES];
        size_t consumed = 0;

        if (!apexsprite_decode(data + *pos, len - *pos, tmp,
                               NULL, NULL, NULL, NULL, NULL, NULL, &consumed) ||
            consumed == 0u) {
            consumed = 1u;
        }
        emit_data_bytes(out, data, len, base_addr, pos, consumed);
        return 1;
    }
    if (range->kind == DATA_SPRITE_NOHEADER) {
        uint8_t tmp[APEX_SPRITE_MAX_BYTES];
        size_t consumed = 0;

        if (!apexsprite_decode_noheader(data + *pos, len - *pos, tmp,
                                        (uint8_t)range->length, NULL, &consumed) ||
            consumed == 0u) {
            consumed = 1u;
        }
        emit_data_bytes(out, data, len, base_addr, pos, consumed);
        return 1;
    }
    emit_data_bytes(out, data, len, base_addr, pos, range->length);
    return 1;
}

static void emit_inline_fields(FILE *out, const InlineSignature *sig, const uint8_t *data,
                               size_t len, size_t *pos, uint8_t current_bank, const char *inst,
                               const Label *labels, size_t label_count,
                               const Label *extra_labels, size_t extra_label_count,
                               const uint8_t *paged_rom, size_t banks,
                               const LabelSet *bank_labels, uint32_t base_addr, size_t rom_base,
                               const ConfigTypes *types)
{
    size_t i;

    for (i = 0; i < sig->schema.count; i++) {
        size_t n;
        const TableField *field = &sig->schema.items[i];

        for (n = 0; n < field->count; n++) {
            TableFieldKind kind = field->kind;

            if (kind == TABLE_BYTE) {
                uint8_t byte_val = data[*pos];

                fprintf(out, "        INLINE_BYTE 0x%02x ; for %s", byte_val, inst);
                if (field->type_name && types) {
                    const char *ename = config_type_enum_name(types, field->type_name, byte_val);

                    if (ename) {
                        fprintf(out, " %s=%s", field->type_name, ename);
                    } else {
                        fprintf(out, " %s=0x%02x", field->type_name, byte_val);
                    }
                }
                fputc('\n', out);
                (*pos)++;
            } else if (kind == TABLE_WORD) {
                uint16_t word_val = read_be16(data + *pos);

                fprintf(out, "        INLINE_WORD 0x%04x ; for %s", word_val, inst);
                if (field->type_name && types) {
                    const char *ename = config_type_enum_name(types, field->type_name, word_val);

                    if (ename) {
                        fprintf(out, " %s=%s", field->type_name, ename);
                    } else {
                        fprintf(out, " %s=0x%04x", field->type_name, word_val);
                    }
                }
                fputc('\n', out);
                *pos += 2u;
            } else if (table_kind_is_far(kind)) {
                uint16_t target = read_be16(data + *pos);
                uint8_t bank = data[*pos + 2u];

                if (kind == TABLE_FAR_CODE && !valid_far_code_target(target, bank, paged_rom,
                                                                     banks)) {
                    emit_inline_far_warning(out, current_bank, base_addr + (uint32_t)*pos,
                                            rom_base + *pos, inst, target, bank);
                }
                fputs("    ", out);
                emit_far_code_ref(out, inline_far_pseudo(kind), target, bank, labels, label_count,
                                  extra_labels, extra_label_count, paged_rom, banks, bank_labels);
                fprintf(out, " ; for %s", inst);
                if (kind == TABLE_FAR_DMD_FULLFRAME) {
                    emit_dmd_fullframe_target_comment(out, paged_rom, banks, bank, target);
                }
                if (kind == TABLE_FAR_SPRITE) {
                    emit_sprite_target_comment(out, paged_rom, banks, bank, target);
                }
                fputc('\n', out);
                *pos += 3u;
            } else {
                uint16_t ptr = read_be16(data + *pos);
                const char *label = table_ptr_label(ptr, current_bank, labels, label_count,
                                                    extra_labels, extra_label_count);

                if (label) {
                    fprintf(out, "        %s %s ; for %s", inline_ptr_pseudo(kind), label, inst);
                } else {
                    fprintf(out, "        %s 0x%04x ; for %s", inline_ptr_pseudo(kind), ptr, inst);
                }
                if (kind == TABLE_PTR16_DMD_FULLFRAME) {
                    emit_dmd_fullframe_target_comment(out, paged_rom, banks, current_bank, ptr);
                }
                if (kind == TABLE_PTR16_SPRITE) {
                    emit_sprite_target_comment(out, paged_rom, banks, current_bank, ptr);
                }
                fputc('\n', out);
                *pos += 2u;
            }
            if (*pos > len) {
                return;
            }
        }
    }
}

static void emit_db_with_labels(FILE *out, const uint8_t *data, size_t len, uint32_t base_addr,
                                const Label *labels, size_t label_count, int sorted,
                                const Label *extra_labels, size_t extra_label_count,
                                const VectorInfo *vectors, size_t vector_count,
                                const InlineSignatures *inline_sigs, const uint8_t *paged_rom,
                                size_t banks, const LabelSet *bank_labels, const TableDefs *tables,
                                const ConfigDocs *docs,
                                const ConfigSymbols *symbols, const DataRanges *data_ranges,
                                const ReferenceSet *refs,
                                uint8_t current_bank,
                                size_t rom_base, int emit_explain, const ConfigTypes *types)
{
    size_t pos = 0;
    int decoding_code = 0;
    BlockKind previous_kind = BLOCK_UNKNOWN;
    LabelLookup lookup;

    lookup.labels = labels;
    lookup.label_count = label_count;
    lookup.extra_labels = extra_labels;
    lookup.extra_label_count = extra_label_count;
    lookup.symbols = symbols;
    lookup.sorted = sorted;

    while (pos < len) {
        size_t col = 0;
        const char *entry_name;
        const TableDef *table = NULL;
        const DataRange *data_range = NULL;
        int has_string_label;
        int has_code_label;
        BlockKind current_kind = BLOCK_UNCLASSIFIED;

        if (tables) {
            table = table_def_at(current_bank, base_addr + (uint32_t)pos, tables);
        }
        data_range = data_range_at(current_bank, base_addr + (uint32_t)pos, data_ranges);
        has_string_label = string_label_at(base_addr + (uint32_t)pos, labels, label_count, sorted);
        has_code_label = code_label_at(base_addr + (uint32_t)pos, labels, label_count, sorted);
        if (table) {
            current_kind = BLOCK_TABLE;
        } else if (data_range) {
            if (data_range->kind == DATA_SPRITE || data_range->kind == DATA_SPRITE_NOHEADER ||
                data_range->kind == DATA_FAR_SPRITE) {
                current_kind = BLOCK_SPRITE;
            } else {
                current_kind = BLOCK_DATA;
            }
        } else if (has_string_label) {
            current_kind = BLOCK_DATA;
        } else if (has_code_label || decoding_code) {
            current_kind = BLOCK_CODE;
        }
        emit_transition_comment(out, previous_kind, current_kind, current_bank,
                                base_addr + (uint32_t)pos, rom_base + pos);
        emit_labels_at(out, base_addr + (uint32_t)pos, labels, label_count, sorted,
                       current_bank, base_addr, rom_base, inline_sigs, table, data_range,
                       data, len, pos, docs, refs, emit_explain,
                       paged_rom, banks);
        if (data_range && emit_data_range(out, data_range, data, len, base_addr, &pos, labels, label_count,
                                          extra_labels, extra_label_count, paged_rom, banks,
                                          bank_labels)) {
            decoding_code = 0;
            previous_kind = (current_kind == BLOCK_SPRITE) ? BLOCK_SPRITE : BLOCK_DATA;
            continue;
        }
        if (has_string_label) {
            size_t string_len = valid_string_len(data + pos, len - pos);

            if (string_len > 0) {
                emit_inner_string_label_equates(out, base_addr + (uint32_t)pos, string_len, labels,
                                                label_count);
                emit_string(out, data + pos, string_len);
                pos += string_len;
                decoding_code = 0;
                previous_kind = BLOCK_DATA;
                continue;
            }
        }
        if (table) {
            if (table->has_header) {
                emit_counted_table(out, table, data, len, base_addr, &pos, current_bank, labels,
                                   label_count, extra_labels, extra_label_count, paged_rom, banks,
                                   bank_labels, types, docs);
            } else {
                emit_table_rows(out, table, data, len, base_addr, &pos, current_bank, table->rows,
                                table_schema_width(&table->schema), labels, label_count,
                                extra_labels, extra_label_count, paged_rom, banks, bank_labels,
                                types, docs);
            }
            decoding_code = 0;
            previous_kind = BLOCK_TABLE;
            continue;
        }
        if (has_code_label) {
            decoding_code = 1;
        }
        entry_name = vector_entry_at(base_addr + (uint32_t)pos, vectors, vector_count);
        if (entry_name && pos + 1 < len) {
            fprintf(out, "    .DW %s\n", entry_name);
            pos += 2;
            decoding_code = 0;
            previous_kind = current_kind == BLOCK_CODE ? BLOCK_UNCLASSIFIED : current_kind;
            continue;
        }
        if (decoding_code) {
            char inst[256];
            Cpu6809InstrInfo info =
                cpu6809_disassemble_info_ex(data + pos, len - pos, base_addr + (uint32_t)pos, inst,
                                            sizeof(inst), lookup_label_for_cpu, &lookup);
            if (info.size > 0) {
                const InlineSignature *inline_sig = NULL;

                if (label_between(base_addr + (uint32_t)pos,
                                  base_addr + (uint32_t)(pos + info.size), labels,
                                  label_count, sorted)) {
                    decoding_code = 0;
                    emit_transition_comment(out, BLOCK_CODE, BLOCK_UNCLASSIFIED, current_bank,
                                            base_addr + (uint32_t)pos, rom_base + pos);
                    current_kind = BLOCK_UNCLASSIFIED;
                } else {
                    /* Append doc as end-of-line comment when no code label at this
                       address (label-address docs are already in the label header). */
                    const char *idoc = has_code_label ? NULL
                        : config_doc_at(docs, current_bank, base_addr + (uint32_t)pos);
                    if (idoc) {
                        const char *nl = strchr(idoc, '\n');
                        if (nl)
                            fprintf(out, "    %-40s ; %.*s\n", inst, (int)(nl - idoc), idoc);
                        else
                            fprintf(out, "    %-40s ; %s\n", inst, idoc);
                    } else {
                        fprintf(out, "    %s\n", inst);
                    }
                    pos += info.size;
                    if (info.has_target) {
                        inline_sig = inline_signature_for(inline_sigs, current_bank, info.target);
                    }
                    if (inline_sig) {
                        if (pos + inline_sig->length > len) {
                            emit_inline_truncated_warning(
                                out, current_bank, base_addr + (uint32_t)pos, rom_base + pos, inst,
                                inline_sig->length, len - pos);
                        } else {
                            emit_inline_fields(out, inline_sig, data, len, &pos, current_bank, inst,
                                               labels, label_count, extra_labels, extra_label_count,
                                               paged_rom, banks, bank_labels, base_addr, rom_base,
                                               types);
                        }
                    }
                    if (info.flags & CPU6809_FLOW_STOP) {
                        decoding_code = 0;
                    }
                    previous_kind = BLOCK_CODE;
                    continue;
                }
            }
            emit_transition_comment(out, BLOCK_CODE, BLOCK_UNCLASSIFIED, current_bank,
                                    base_addr + (uint32_t)pos, rom_base + pos);
            current_kind = BLOCK_UNCLASSIFIED;
            decoding_code = 0;
        }
        {
            size_t line_start = pos;

        while (pos < len && col < 16) {
            if (col > 0 && labels_at(base_addr + (uint32_t)pos, labels, label_count, sorted)) {
                break;
            }
            if (col > 0 && vector_entry_at(base_addr + (uint32_t)pos, vectors, vector_count)) {
                break;
            }
            pos++;
            col++;
        }
            emit_db_line(out, base_addr + (uint32_t)line_start, data + line_start, col);
        }
        previous_kind = current_kind == BLOCK_CODE ? BLOCK_UNCLASSIFIED : current_kind;
    }
}

static void emit_paged_region(FILE *out, const uint8_t *data, const LabelSet *labels,
                               const LabelSet *system_labels, const InlineSignatures *inline_sigs,
                               const uint8_t *paged_rom, size_t banks, const LabelSet *bank_labels,
                               const TableDefs *tables, const ConfigDocs *docs,
                               const ConfigSymbols *symbols,
                               const DataRanges *data_ranges, const ReferenceSet *refs,
                               size_t rom_bank_base, int emit_explain, const ConfigTypes *types)
{
    size_t used = last_non_ff(data, APEX_BANK_SIZE);
    uint8_t bank_id = data[0];

    emit_location_comment(out, "bank_start", bank_id, APEX_PAGED_ORG, rom_bank_base);
    if (!labels_at(APEX_PAGED_ORG, labels->items, labels->count, labels->sorted)) {
        fprintf(out, "B%02x_A%04x:\n", bank_id, (unsigned)APEX_PAGED_ORG);
    }

    if (used == 0) {
        used = 1;
    }
    fprintf(out, "    BANK_ID 0x%02x\n", bank_id);    if (used > 1) {
        emit_db_with_labels(out, data + 1, used - 1, APEX_PAGED_ORG + 1, labels->items,
                            labels->count, labels->sorted, system_labels->items,
                            system_labels->count, NULL, 0, inline_sigs, paged_rom, banks,
                            bank_labels, tables, docs, symbols, data_ranges,
                            refs, bank_id, rom_bank_base + 1u, emit_explain, types);
    }
    if (used < APEX_BANK_SIZE) {
        emit_location_comment(out, "free", bank_id,
                              (uint32_t)(APEX_PAGED_ORG + used),
                              rom_bank_base + used);
        fprintf(out, "    FILL_TO_BANK_END\n");
    }
}

static size_t total_code_bank_labels(const LabelSet *bank_labels, size_t banks)
{
    size_t total = 0, i, j;

    for (i = 0; i < banks; i++) {
        for (j = 0; j < bank_labels[i].count; j++) {
            if (bank_labels[i].items[j].is_code) total++;
        }
    }
    return total;
}

void build_system_labels(const uint8_t *data, const VectorInfo *vectors, size_t vector_count,
                         InlineSignatures *inline_sigs, const uint8_t *paged_rom,
                         size_t banks, LabelSet *bank_labels, LabelSet *labels,
                         const ConfigLabels *config_labels,
                         const ConfigEntries *config_entries,
                         const DataRanges *data_ranges, const ConfigOptions *options,
                         ReferenceSet *refs)
{
    size_t used = last_non_ff(data, APEX_SYSTEM_SIZE);
    size_t i;
    uint32_t inline_dispatcher;

    for (i = 0; i < vector_count; i++) {
        Label *entry = add_label(labels, vectors[i].target_addr, vectors[i].entry_name, 1);
        Label *vector = add_label(labels, vectors[i].vector_addr, vectors[i].vector_name, 0);

        explain_label(entry, "vector_entry");
        explain_label_kind(entry, "vector_entry");
        explain_label(vector, "vector_table");
        explain_label_kind(vector, "vector_table");
    }
    for (i = 0; i < config_labels->count; i++) {
        if (((!config_labels->items[i].has_bank) ||
             (config_labels->items[i].has_bank && config_labels->items[i].bank == 0xffu)) &&
            in_system_addr(config_labels->items[i].addr)) {
            Label *label = add_label(labels, config_labels->items[i].addr,
                                     config_labels->items[i].name, options->labels_are_entries);

            explain_label(label, "config_label");
            if (options->labels_are_entries) {
                label->is_explicit_entry = 1;
                explain_label_kind(label, "config_label_entry");
            }
        }
    }
    for (i = 0; i < config_entries->count; i++) {
        if (((!config_entries->items[i].has_bank) ||
             (config_entries->items[i].has_bank && config_entries->items[i].bank == 0xffu)) &&
            in_system_addr(config_entries->items[i].addr)) {
            Label *label = add_label(labels, config_entries->items[i].addr,
                                     make_generated_label(config_entries->items[i].addr), 1);

            label->is_explicit_entry = 1;
            explain_label(label, "config_entry");
            explain_label_kind(label, "config_entry");
        }
    }
    inline_dispatcher = detect_inline_dispatcher(data, used, vectors, vector_count);
    if (inline_dispatcher != 0 && !inline_signature_for(inline_sigs, 0xffu, inline_dispatcher)) {
        add_inline_signature(inline_sigs, inline_dispatcher, 1, TABLE_BYTE);
    }
    {
        size_t prev_count, prev_code;
        do {
            prev_count = labels->count;
            prev_code  = total_code_bank_labels(labels, 1);
            collect_code_targets(data, used, APEX_SYSTEM_ORG, labels, inline_sigs, paged_rom,
                                 banks, bank_labels, labels, data_ranges, 0xff, refs, NULL);
        } while (labels->count != prev_count || total_code_bank_labels(labels, 1) != prev_code);
    }
}

static void emit_system_region(FILE *out, const uint8_t *data, const VectorInfo *vectors,
                                size_t vector_count, const InlineSignatures *inline_sigs,
                                const LabelSet *labels, const uint8_t *paged_rom, size_t banks,
                                const LabelSet *bank_labels, const TableDefs *tables,
                                const ConfigDocs *docs,
                                const ConfigSymbols *symbols, const DataRanges *data_ranges,
                                const ReferenceSet *refs,
                                size_t rom_system_base, int emit_explain, const ConfigTypes *types)
{
    size_t used = last_non_ff(data, APEX_SYSTEM_SIZE);

    emit_location_comment(out, "bank_start", 0xffu, APEX_SYSTEM_ORG, rom_system_base);
    if (!labels_at(APEX_SYSTEM_ORG, labels->items, labels->count, labels->sorted)) {
        fprintf(out, "Bff_A%04x:\n", (unsigned)APEX_SYSTEM_ORG);
    }

    if (used > 0) {
        emit_db_with_labels(out, data, used, APEX_SYSTEM_ORG, labels->items, labels->count,
                            labels->sorted, NULL, 0, vectors, vector_count, inline_sigs,
                            paged_rom, banks, bank_labels, tables, docs,
                            symbols, data_ranges, refs, 0xff, rom_system_base, emit_explain,
                            types);
    }
    if (used < APEX_SYSTEM_SIZE) {
        emit_location_comment(out, "free", 0xffu,
                              (uint32_t)(APEX_SYSTEM_ORG + used),
                              rom_system_base + used);
        fprintf(out, "    FILL_TO_BANK_END\n");
    }
}

static void emit_xref_for_label(FILE *out, uint8_t bank, const Label *label,
                                const ReferenceSet *refs)
{
    size_t i;
    size_t j;
    int header = 0;

    for (i = 0; i < refs->count; i++) {
        if (refs->items[i].bank == bank && refs->items[i].addr == label->addr) {
            for (j = 0; j < i; j++) {
                if (refs->items[j].bank == bank && refs->items[j].addr == label->addr &&
                    strcmp(refs->items[j].kind, refs->items[i].kind) == 0 &&
                    strcmp(refs->items[j].source, refs->items[i].source) == 0) {
                    break;
                }
            }
            if (j != i) {
                continue;
            }
            if (!header) {
                fprintf(out, "; XREF %s bank=0x%02x cpu=0x%04x\n", label->name, bank,
                        (unsigned)label->addr & 0xffffu);
                header = 1;
            }
            fprintf(out, ";   %s:%s\n", refs->items[i].kind, refs->items[i].source);
        }
    }
}

static void emit_xref_index(FILE *out, const uint8_t *paged_rom, const LabelSet *bank_labels,
                            size_t banks,
                            const LabelSet *system_labels, const ReferenceSet *refs)
{
    size_t i;
    size_t j;

    if (!refs || refs->count == 0) {
        return;
    }
    fprintf(out, "\n; XREF INDEX\n");
    for (i = 0; i < banks; i++) {
        uint8_t bank_id = paged_rom[i * APEX_BANK_SIZE];

        for (j = 0; j < bank_labels[i].count; j++) {
            emit_xref_for_label(out, bank_id, &bank_labels[i].items[j], refs);
        }
    }
    for (j = 0; j < system_labels->count; j++) {
        emit_xref_for_label(out, 0xffu, &system_labels->items[j], refs);
    }
}

static size_t total_bank_labels(const LabelSet *bank_labels, size_t banks)
{
    size_t total = 0;
    size_t i;

    for (i = 0; i < banks; i++) {
        total += bank_labels[i].count;
    }
    return total;
}

void collect_bank_code_targets(const uint8_t *paged_rom, size_t banks,
                               const InlineSignatures *inline_sigs,
                               LabelSet *bank_labels, LabelSet *system_labels,
                               const DataRanges *data_ranges, ReferenceSet *refs,
                               const ConfigEntries *ref_exclusions)
{
    size_t prev_count, prev_code;

    do {
        size_t i;
        prev_count = total_bank_labels(bank_labels, banks);
        prev_code  = total_code_bank_labels(bank_labels, banks);
        for (i = 0; i < banks; i++) {
            const uint8_t *bank = paged_rom + i * APEX_BANK_SIZE;
            size_t used = last_non_ff(bank, APEX_BANK_SIZE);

            if (used > 1) {
                collect_code_targets(bank, used, APEX_PAGED_ORG, &bank_labels[i], inline_sigs,
                                     paged_rom, banks, bank_labels, system_labels, data_ranges,
                                     bank[0], refs, ref_exclusions);
            }
        }
    } while (total_bank_labels(bank_labels, banks) != prev_count ||
             total_code_bank_labels(bank_labels, banks) != prev_code);
}

void apply_config_bank_labels(const ConfigLabels *config_labels, const uint8_t *paged_rom,
                              size_t banks, LabelSet *bank_labels,
                              const ConfigOptions *options)
{
    size_t i;

    for (i = 0; i < config_labels->count; i++) {
        const ConfigLabel *label = &config_labels->items[i];
        int bank_index;

        if (!label->has_bank) {
            continue;
        }
        bank_index = bank_index_for_id(paged_rom, banks, label->bank);
        if (bank_index >= 0 && label->addr >= APEX_PAGED_ORG && label->addr < 0x8000u) {
            Label *target = add_label(&bank_labels[bank_index], label->addr, label->name,
                                      options->labels_are_entries);

            explain_label(target, "config_label");
            if (options->labels_are_entries) {
                target->is_explicit_entry = 1;
                explain_label_kind(target, "config_label_entry");
            }
        }
    }
}

void apply_config_bank_entries(const ConfigEntries *config_entries, const uint8_t *paged_rom,
                               size_t banks, LabelSet *bank_labels)
{
    size_t i;

    for (i = 0; i < config_entries->count; i++) {
        const ConfigEntry *entry = &config_entries->items[i];
        int bank_index;

        if (!entry->has_bank) {
            continue;
        }
        bank_index = bank_index_for_id(paged_rom, banks, entry->bank);
        if (bank_index >= 0 && entry->addr >= APEX_PAGED_ORG && entry->addr < 0x8000u) {
            Label *label = add_label(&bank_labels[bank_index], entry->addr,
                                     make_bank_label(entry->bank, entry->addr), 1);

            label->is_explicit_entry = 1;
            explain_label(label, "config_entry");
            explain_label_kind(label, "config_entry");
        }
    }
}

static void apply_data_far_label(DataKind kind, const uint8_t *paged_rom, size_t banks,
                                 LabelSet *bank_labels, LabelSet *system_labels, uint16_t addr,
                                 uint8_t bank);
static void apply_table_ptr16_label(TableFieldKind kind, LabelSet *labels, uint8_t bank,
                                    uint16_t ptr);

static const char *table_far_ref_source(TableFieldKind kind)
{
    if (kind == TABLE_FAR_CODE) {
        return "table_far_code_ref";
    }
    if (kind == TABLE_FAR_STRING) {
        return "table_far_string_ref";
    }
    if (kind == TABLE_FAR_DMD_FULLFRAME) {
        return "table_far_dmd_fullframe_ref";
    }
    if (kind == TABLE_FAR_SPRITE) {
        return "table_far_sprite_ref";
    }
    return "table_far_data_ref";
}

static const char *table_ptr_ref_source(TableFieldKind kind)
{
    if (kind == TABLE_PTR16_CODE) {
        return "table_ptr16_code_ref";
    }
    if (kind == TABLE_PTR16_STRING) {
        return "table_ptr16_string_ref";
    }
    if (kind == TABLE_PTR16_DMD_FULLFRAME) {
        return "table_ptr16_dmd_fullframe_ref";
    }
    if (kind == TABLE_PTR16_SPRITE) {
        return "table_ptr16_sprite_ref";
    }
    return "table_ptr16_data_ref";
}

static const char *config_data_source(DataKind kind)
{
    if (kind == DATA_DMD_FULLFRAME) {
        return "config_dmd_fullframe";
    }
    if (kind == DATA_SPRITE) {
        return "config_sprite";
    }
    return "config_data";
}

void apply_data_range_labels(const DataRanges *data_ranges, const uint8_t *paged_rom,
                             size_t banks, const uint8_t *system_rom,
                             LabelSet *bank_labels, LabelSet *system_labels,
                             ReferenceSet *refs)
{
    size_t i;

    for (i = 0; i < data_ranges->count; i++) {
        const DataRange *range = &data_ranges->items[i];
        Label *label;

        if (range->bank == 0xffu) {
            if (!in_system_addr(range->addr)) {
                continue;
            }
            label = add_label(system_labels, range->addr, make_generated_label(range->addr), 0);
        } else {
            int bank_index = bank_index_for_id(paged_rom, banks, range->bank);

            if (bank_index < 0 || range->addr < APEX_PAGED_ORG || range->addr >= 0x8000u) {
                continue;
            }
            label = add_label(&bank_labels[bank_index], range->addr,
                              make_bank_label(range->bank, range->addr), 0);
        }
        explain_label(label, config_data_source(range->kind));
        explain_label_kind(label, config_data_source(range->kind));
        mark_label_data(label);
        if (range->kind == DATA_STRING || range->kind == DATA_STRING_LP ||
            range->kind == DATA_STRING_FIXED) {
            label->is_string = 1;
        } else if (data_kind_is_far(range->kind) && range->addr + 2u <= 0xffffu) {
            const uint8_t *data;
            uint16_t target;
            uint8_t bank;
            const char *source = NULL;

            if (range->bank == 0xffu) {
                if (!in_system_addr(range->addr) || range->addr + 2u > 0xffffu) {
                    continue;
                }
                data = system_rom + (size_t)(range->addr - APEX_SYSTEM_ORG);
                source = label_name_at(range->addr, system_labels->items, system_labels->count, 0);
            } else {
                int bank_index = bank_index_for_id(paged_rom, banks, range->bank);

                if (bank_index < 0 || range->addr < APEX_PAGED_ORG ||
                    range->addr + 2u >= 0x8000u) {
                    continue;
                }
                data = paged_rom + (size_t)bank_index * APEX_BANK_SIZE +
                       (size_t)(range->addr - APEX_PAGED_ORG);
                source = label_name_at(range->addr, bank_labels[bank_index].items,
                                       bank_labels[bank_index].count, 0);
            }
            target = read_be16(data);
            bank = data[2];
            apply_data_far_label(range->kind, paged_rom, banks, bank_labels, system_labels, target,
                                 bank);
            if (bank == 0xffu && in_system_addr(target)) {
                add_reference(refs, 0xff, target, range->bank, range->addr, "data", source);
            } else {
                int bank_index = bank_index_for_far_ref(paged_rom, banks, bank);

                if (bank_index >= 0 && target >= APEX_PAGED_ORG && target < 0x8000u) {
                    add_reference(refs, bank_id_for_index(paged_rom, bank_index), target,
                                  range->bank, range->addr, "data", source);
                }
            }
        } else if (data_kind_is_ptr16(range->kind)) {
            /* Follow each 2-byte pointer in the range to discover code/data targets. */
            TableFieldKind tkind = data_ptr16_to_table_kind(range->kind);
            const uint8_t *data = NULL;
            const char *source = NULL;
            size_t j;

            if (range->bank == 0xffu) {
                if (!in_system_addr(range->addr)) {
                    continue;
                }
                data = system_rom + (size_t)(range->addr - APEX_SYSTEM_ORG);
                source = label_name_at(range->addr, system_labels->items, system_labels->count, 0);
            } else {
                int bank_index = bank_index_for_id(paged_rom, banks, range->bank);

                if (bank_index < 0 || range->addr < APEX_PAGED_ORG ||
                    range->addr >= 0x8000u) {
                    continue;
                }
                data = paged_rom + (size_t)bank_index * APEX_BANK_SIZE +
                       (size_t)(range->addr - APEX_PAGED_ORG);
                source = label_name_at(range->addr, bank_labels[bank_index].items,
                                       bank_labels[bank_index].count, 0);
            }
            for (j = 0; j + 2u <= range->length; j += 2u) {
                uint16_t ptr = read_be16(data + j);

                if (ptr >= APEX_PAGED_ORG && ptr < 0x8000u) {
                    int bank_index = bank_index_for_id(paged_rom, banks, range->bank);

                    if (bank_index >= 0) {
                        apply_table_ptr16_label(tkind, &bank_labels[bank_index], range->bank, ptr);
                        add_reference(refs, range->bank, ptr, range->bank,
                                      range->addr + (uint32_t)j, "data", source);
                    }
                } else if (in_system_addr(ptr)) {
                    apply_system_ptr16_label(tkind, system_labels, ptr, "data");
                    add_reference(refs, 0xff, ptr, range->bank,
                                  range->addr + (uint32_t)j, "data", source);
                }
            }
        }
    }
}

static void apply_table_far_label(TableFieldKind kind, const uint8_t *paged_rom, size_t banks,
                                  LabelSet *bank_labels, LabelSet *system_labels, uint16_t addr,
                                  uint8_t bank)
{
    int bank_index;
    int is_code = kind == TABLE_FAR_CODE;
    int is_string = kind == TABLE_FAR_STRING;
    Label *label;

    if (bank == 0xffu && in_system_addr(addr)) {
        label = add_label(system_labels, addr, make_generated_label(addr), is_code);
        explain_label(label, table_far_ref_source(kind));
        explain_label_kind(label, table_far_ref_source(kind));
        if (!is_code) {
            mark_label_data(label);
        }
        if (is_string) {
            label->is_string = 1;
        }
        return;
    }
    bank_index = bank_index_for_far_ref(paged_rom, banks, bank);
    if (bank_index >= 0 && addr >= APEX_PAGED_ORG && addr < 0x8000u) {
        label = add_label(&bank_labels[bank_index], addr,
                          make_bank_label(bank_id_for_index(paged_rom, bank_index), addr),
                          is_code);
        explain_label(label, table_far_ref_source(kind));
        explain_label_kind(label, table_far_ref_source(kind));
        if (!is_code) {
            mark_label_data(label);
        }
        if (is_string) {
            label->is_string = 1;
        }
    }
}

static void apply_data_far_label(DataKind kind, const uint8_t *paged_rom, size_t banks,
                                 LabelSet *bank_labels, LabelSet *system_labels, uint16_t addr,
                                 uint8_t bank)
{
    TableFieldKind table_kind = TABLE_FAR_DATA;

    if (kind == DATA_FAR_STRING) {
        table_kind = TABLE_FAR_STRING;
    } else if (kind == DATA_FAR_TABLE) {
        table_kind = TABLE_FAR_TABLE;
    } else if (kind == DATA_FAR_CODE) {
        table_kind = TABLE_FAR_CODE;
    } else if (kind == DATA_FAR_DMD_FULLFRAME) {
        table_kind = TABLE_FAR_DMD_FULLFRAME;
    } else if (kind == DATA_FAR_SPRITE) {
        table_kind = TABLE_FAR_SPRITE;
    }
    apply_table_far_label(table_kind, paged_rom, banks, bank_labels, system_labels, addr, bank);
}

static void apply_table_ptr16_label(TableFieldKind kind, LabelSet *labels, uint8_t bank, uint16_t ptr)
{
    Label *target;

    if (ptr < APEX_PAGED_ORG || ptr >= 0x8000u) {
        return;
    }
    target = add_label(labels, ptr, make_bank_label(bank, ptr), kind == TABLE_PTR16_CODE);
    explain_label(target, table_ptr_ref_source(kind));
    explain_label_kind(target, table_ptr_ref_source(kind));
    if (kind != TABLE_PTR16_CODE) {
        mark_label_data(target);
    }
    if (kind == TABLE_PTR16_STRING) {
        target->is_string = 1;
    }
}

static void apply_table_field_labels(const TableDef *table, const uint8_t *data, size_t used,
                                     size_t *pos, uint8_t current_bank,
                                     const uint8_t *paged_rom, size_t banks, LabelSet *bank_labels,
                                     LabelSet *system_labels, const char *source,
                                     ReferenceSet *refs, size_t row_index, uint32_t row_cpu_addr)
{
    size_t row_start = *pos;
    size_t i;

    for (i = 0; i < table->schema.count; i++) {
        size_t n;

        for (n = 0; n < table->schema.items[i].count; n++) {
            TableFieldKind kind = table->schema.items[i].kind;

            if (kind == TABLE_BYTE) {
                (*pos)++;
            } else if (kind == TABLE_WORD) {
                *pos += 2u;
            } else if (table_kind_is_far(kind)) {
                uint16_t addr;
                uint8_t bank;
                int bank_index;
                uint8_t target_bank;

                if (*pos + 3u > used) {
                    return;
                }
                addr = read_be16(data + *pos);
                bank = data[*pos + 2u];
                apply_table_far_label(kind, paged_rom, banks, bank_labels, system_labels, addr,
                                      bank);
                if (bank == 0xffu && in_system_addr(addr)) {
                    add_table_row_reference(refs, 0xff, addr, table->bank, table->addr,
                                            source, (int)row_index, row_cpu_addr);
                } else {
                    bank_index = bank_index_for_far_ref(paged_rom, banks, bank);
                    if (bank_index >= 0 && addr >= APEX_PAGED_ORG && addr < 0x8000u) {
                        target_bank = bank_id_for_index(paged_rom, bank_index);
                        add_table_row_reference(refs, target_bank, addr, table->bank, table->addr,
                                                source, (int)row_index, row_cpu_addr);
                    }
                }
                *pos += 3u;
            } else {
                uint16_t ptr;

                if (*pos + 2u > used) {
                    return;
                }
                ptr = read_be16(data + *pos);
                if (ptr >= APEX_PAGED_ORG && ptr < 0x8000u) {
                    int bank_index = bank_index_for_id(paged_rom, banks, current_bank);

                    if (bank_index >= 0) {
                        apply_table_ptr16_label(kind, &bank_labels[bank_index], current_bank, ptr);
                        add_table_row_reference(refs, current_bank, ptr, table->bank, table->addr,
                                                source, (int)row_index, row_cpu_addr);
                    }
                } else if (in_system_addr(ptr)) {
                    apply_system_ptr16_label(kind, system_labels, ptr, "table");
                    add_table_row_reference(refs, 0xff, ptr, table->bank, table->addr,
                                            source, (int)row_index, row_cpu_addr);
                }
                *pos += 2u;
            }
        }
    }
    if (*pos < row_start + table_schema_width(&table->schema)) {
        *pos = row_start + table_schema_width(&table->schema);
    }
}

void apply_table_labels(const TableDefs *tables, const uint8_t *paged_rom, size_t banks,
                        LabelSet *bank_labels, LabelSet *system_labels,
                        const uint8_t *system_rom, ReferenceSet *refs)
{
    size_t i;

    for (i = 0; i < tables->count; i++) {
        const TableDef *table = &tables->items[i];
        int bank_index = bank_index_for_id(paged_rom, banks, table->bank);
        const uint8_t *bank;
        size_t used;
        size_t pos;
        uint16_t count;
        uint8_t row_width;
        size_t row;
        const char *source;

        if (table->bank == 0xffu) {
            if (!in_system_addr(table->addr)) {
                continue;
            }
            bank = system_rom;
            used = last_non_ff(bank, APEX_SYSTEM_SIZE);
            pos = (size_t)(table->addr - APEX_SYSTEM_ORG);
            {
                Label *table_label =
                    add_label(system_labels, table->addr, make_generated_label(table->addr), 0);

                explain_label(table_label, "config_table");
                explain_label_kind(table_label, "config_table");
                mark_label_data(table_label);
            }
            source = label_name_at(table->addr, system_labels->items, system_labels->count, 0);
            if (table->has_header && pos + 3u <= used) {
                count = read_be16(bank + pos);
                row_width = bank[pos + 2u];
                pos += 3u;
            } else {
                count = (uint16_t)table->rows;
                row_width = (uint8_t)table_schema_width(&table->schema);
            }
            for (row = 0; row < count && pos + row_width <= used; row++) {
                size_t row_pos = pos;
                uint32_t row_cpu = (uint32_t)(APEX_SYSTEM_ORG + row_pos);

                apply_table_field_labels(table, bank, used, &pos, table->bank, paged_rom, banks,
                                         bank_labels, system_labels, source, refs, row, row_cpu);
                pos = row_pos + row_width;
            }
            continue;
        }
        if (bank_index < 0 || table->addr < APEX_PAGED_ORG || table->addr >= 0x8000u) {
            continue;
        }
        bank = paged_rom + (size_t)bank_index * APEX_BANK_SIZE;
        used = last_non_ff(bank, APEX_BANK_SIZE);
        pos = (size_t)(table->addr - APEX_PAGED_ORG);
        {
            Label *table_label = add_label(&bank_labels[bank_index], table->addr,
                                           make_bank_label(table->bank, table->addr), 0);

            explain_label(table_label, "config_table");
            explain_label_kind(table_label, "config_table");
            mark_label_data(table_label);
        }
        source = label_name_at(table->addr, bank_labels[bank_index].items, bank_labels[bank_index].count, 0);
        if (!table->has_header) {
            row_width = (uint8_t)table_schema_width(&table->schema);
            for (row = 0; row < table->rows && pos + row_width <= used; row++) {
                size_t row_pos = pos;
                uint32_t row_cpu = (uint32_t)(APEX_PAGED_ORG + row_pos);

                apply_table_field_labels(table, bank, used, &pos, table->bank, paged_rom, banks,
                                         bank_labels, system_labels, source, refs, row, row_cpu);
                pos = row_pos + row_width;
            }
            continue;
        }
        if (pos + 3u > used) {
            continue;
        }
        count = read_be16(bank + pos);
        row_width = bank[pos + 2u];
        pos += 3u;
        for (row = 0; row < count && pos + row_width <= used; row++) {
            size_t row_pos = pos;
            uint32_t row_cpu = (uint32_t)(APEX_PAGED_ORG + row_pos);

            apply_table_field_labels(table, bank, used, &pos, table->bank, paged_rom, banks,
                                     bank_labels, system_labels, source, refs, row, row_cpu);
            pos = row_pos + row_width;
        }
    }
}

static void emit_preamble(FILE *out, const ApexProject *project)
{
    ApexRomInfo info;
    char timebuf[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    int j;

    apex_rominfo_compute(project->rom.data, project->rom.size, &info);
    if (tm_info)
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);
    else
        strcpy(timebuf, "unknown");

    fputs("; ============================================================\n", out);
    fputs("; ApexII Disassembly\n", out);
    fputs("; ============================================================\n", out);

    /* ROM path (basename only to keep it tidy) */
    {
        const char *path = project->rom_path ? project->rom_path : "(unknown)";
        const char *base = path;
        const char *p;
        for (p = path; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        fprintf(out, "; %-14s%s\n", "ROM:", base);
    }

    fprintf(out, "; %-14s%s\n", "Generated:", timebuf);

    /* Size */
    if (project->rom.size >= 1048576u)
        fprintf(out, "; %-14s%zu bytes (%zu MB)\n",
                "Size:", project->rom.size, project->rom.size / 1048576u);
    else
        fprintf(out, "; %-14s%zu bytes (%zu KB)\n",
                "Size:", project->rom.size, project->rom.size / 1024u);

    fputs(";\n", out);

    /* OS + game version */
    if (info.os_valid)
        fprintf(out, "; %-14s%u.%u\n",
                "OS Version:", (unsigned)info.os_major, (unsigned)info.os_minor);
    else
        fprintf(out, "; %-14sunknown\n", "OS Version:");

    if (info.game_version[0])
        fprintf(out, "; %-14s%s\n", "Game Version:", info.game_version);
    else
        fprintf(out, "; %-14snot found\n", "Game Version:");

    fputs(";\n", out);

    /* Checksum */
    fprintf(out, "; %-14s0x%04X  %s  (delta 0x%04X)\n",
            "Checksum:",
            info.stored_csum,
            info.computed_csum == info.stored_csum ? "VALID" : "INVALID",
            info.stored_delta);

    /* CRC-32 */
    fprintf(out, "; %-14s%08X\n", "CRC-32:", info.crc32_val);

    /* SHA-1 (40 hex chars) */
    fprintf(out, "; %-14s", "SHA-1:");
    for (j = 0; j < 20; j++) fprintf(out, "%02x", info.sha1[j]);
    fputc('\n', out);

    /* SHA-256 (64 hex chars — split at 32 for readability) */
    fprintf(out, "; %-14s", "SHA-256:");
    for (j = 0; j < 32; j++) fprintf(out, "%02x", info.sha256[j]);
    fputc('\n', out);

    fputs("; ============================================================\n", out);
    fputc('\n', out);
}

int apex_project_write_asm_stream(const ApexProject *project, FILE *out, int emit_xrefs,
                                  int emit_explain)
{
    size_t i;

    emit_preamble(out, project);
    fprintf(out, ".ROM_SIZE %lu\n\n", (unsigned long)project->rom.size);
    emit_config_symbols(out, &project->symbols);
    emit_type_equates(out, &project->config_types);
    emit_vector_equates(out, project->vectors, sizeof(project->vectors) / sizeof(project->vectors[0]));

    for (i = 0; i < project->banks; i++) {
        fprintf(out, ".BANK 0x%02lx\n", (unsigned long)i);
        fprintf(out, ".ORG 0x%04x\n", APEX_PAGED_ORG);
        emit_paged_region(out, project->rom.data + i * APEX_BANK_SIZE, &project->bank_labels[i],
                          &project->system_labels, &project->inline_sigs, project->rom.data,
                          project->banks, project->bank_labels, &project->tables,
                          &project->docs, &project->symbols,
                          &project->data_ranges, &project->refs, i * APEX_BANK_SIZE,
                          emit_explain, &project->config_types);
        fputc('\n', out);
    }

    fprintf(out, ".BANK SYSTEM\n");
    fprintf(out, ".ORG 0x%04x\n", APEX_SYSTEM_ORG);
    emit_system_region(out, project->rom.data + project->paged_size, project->vectors,
                       sizeof(project->vectors) / sizeof(project->vectors[0]),
                       &project->inline_sigs, &project->system_labels, project->rom.data,
                       project->banks, project->bank_labels, &project->tables,
                       &project->docs, &project->symbols,
                       &project->data_ranges, &project->refs, project->paged_size, emit_explain,
                       &project->config_types);
    if (emit_xrefs) {
        emit_xref_index(out, project->rom.data, project->bank_labels, project->banks,
                        &project->system_labels, &project->refs);
    }
    return 0;
}

int apex_project_write_asm(const ApexProject *project, const char *output_path, int emit_xrefs,
                           int emit_explain)
{
    FILE *out;

    out = fopen(output_path, "w");
    if (!out) {
        die("failed to open %s for writing", output_path);
    }

    apex_project_write_asm_stream(project, out, emit_xrefs, emit_explain);

    if (fclose(out) != 0) {
        die("failed to close %s", output_path);
    }
    return 0;
}

int apexdis_run(const ApexDisOptions *run_options)
{
    ApexProject *project = apex_project_open(run_options->input_path, run_options->config_path);
    int rc;

    rc = apex_project_analyze(project);
    if (rc == 0) {
        rc = apex_project_write_asm(project, run_options->output_path, run_options->emit_xrefs,
                                    run_options->emit_explain);
    }
    apex_project_free(project);
    return rc;
}
