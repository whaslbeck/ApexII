#include "apeximgui_core.h"
#include "apeximgui_views_internal.h"
#include "apex_rominfo.h"
#include "apex_nvram.h"
#include "ImGuiFileDialog.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>
#include <map>
#include <cctype>
#include <cfloat>
#include <strings.h>

// --- UI Rendering Helpers (Internal) ---

bool str_icontains(const char *hay, const char *needle)
{
    if (!needle || !*needle) return true;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t j;
        for (j = 0; j < nl; j++) {
            if (tolower((unsigned char)hay[j]) != tolower((unsigned char)needle[j])) break;
        }
        if (j == nl) return true;
    }
    return false;
}

/* Click-to-sort support for the data tables.  Add the flags below to a
   BeginTable, give each column a user id in TableSetupColumn, then call
   ui_table_sort() once after the header to learn the active sort column. */

/* Returns true and fills *col (column user id) + *asc when a column is sorted;
   false in the tri-state "unsorted" state.  Always clears SpecsDirty. */
bool ui_table_sort(int *col, bool *asc)
{
    ImGuiTableSortSpecs *sp = ImGui::TableGetSortSpecs();
    if (!sp || sp->SpecsCount == 0) {
        if (sp) sp->SpecsDirty = false;
        return false;
    }
    *col = (int)sp->Specs[0].ColumnUserID;
    *asc = sp->Specs[0].SortDirection != ImGuiSortDirection_Descending;
    sp->SpecsDirty = false;
    return true;
}

/* Three-way compares for use inside per-table sort comparators. */
int ui_cmp_u32(uint32_t a, uint32_t b) { return a < b ? -1 : a > b ? 1 : 0; }
int ui_cmp_sz(size_t a, size_t b)      { return a < b ? -1 : a > b ? 1 : 0; }
int ui_cmp_int(long a, long b)         { return a < b ? -1 : a > b ? 1 : 0; }

static void render_text_chunk(const char *start, const char *end, const ImVec4 *color)
{
    if (!start || !end || end <= start) {
        return;
    }
    if (color) {
        ImGui::PushStyleColor(ImGuiCol_Text, *color);
    }
    ImGui::TextUnformatted(start, end);
    if (color) {
        ImGui::PopStyleColor();
    }
}

static void render_line_text(const ApexRenderedDocument *document, UiState *state,
                             const ApexRenderedLine *line)
{
    static const ImVec4 label_color   = ImVec4(0.95f, 0.82f, 0.45f, 1.0f);
    static const ImVec4 target_color  = ImVec4(0.45f, 0.80f, 0.95f, 1.0f);
    static const ImVec4 comment_color = ImVec4(0.55f, 0.75f, 0.55f, 1.0f);
    if (line->kind == APEX_RENDER_LINE_LABEL) {
        render_text_chunk(line->text, line->text + line->length, &label_color);
        return;
    }
    if (line->kind == APEX_RENDER_LINE_COMMENT ||
        line->kind == APEX_RENDER_LINE_LOCATION) {
        render_text_chunk(line->text, line->text + line->length, &comment_color);
        return;
    }
    /* For instruction lines, find an inline '; comment' suffix and colour it. */
    const char *comment_start = NULL;
    if (line->kind == APEX_RENDER_LINE_INSTRUCTION) {
        for (int ci = 0; ci < (int)line->length; ci++) {
            if (line->text[ci] == ';') {
                comment_start = line->text + ci;
                break;
            }
        }
    }
    const char *line_end = line->text + (comment_start ? (size_t)(comment_start - line->text) : line->length);
    auto targets = find_line_targets(document, state, line);
    const char *cursor = line->text;
    for (auto &target : targets) {
        const char *m_start = line->text + target.match_pos;
        const char *m_end   = m_start + target.name.size();
        if (m_start >= line_end) break;
        if (m_start < cursor) continue;
        if (cursor < m_start) {
            render_text_chunk(cursor, m_start, NULL);
            ImGui::SameLine(0, 0);
        }
        render_text_chunk(m_start, m_end, &target_color);
        cursor = m_end;
        if (cursor < line_end) ImGui::SameLine(0, 0);
    }
    if (cursor < line_end) {
        render_text_chunk(cursor, line_end, NULL);
    }
    if (comment_start) {
        ImGui::SameLine(0, 0);
        render_text_chunk(comment_start, line->text + line->length, &comment_color);
    }
}

void render_dmd_preview(const DmdPreviewInfo &preview, float max_scale)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale = std::max(1.0f, std::min(max_scale, avail.x / (float)APEX_DMD_WIDTH));
    ImGui::TextUnformatted(preview.title);
    ImGui::Text("Address: B%02x_A%04x  ROM: 0x%06lx",
                preview.bank, (unsigned)preview.cpu_addr & 0xffffu,
                (unsigned long)preview.rom_offset);
    if (preview.two_plane) {
        ImGui::Text("Decoder: 0x%02x  Planes: 2 (4-colour)  Consumed: %lu  Size: %ux%u",
                    (unsigned)preview.decoder_type, (unsigned long)preview.consumed,
                    (unsigned)APEX_DMD_WIDTH, (unsigned)APEX_DMD_HEIGHT);
    } else {
        ImGui::Text("Decoder: 0x%02x  Consumed: %lu  Size: %ux%u",
                    (unsigned)preview.decoder_type, (unsigned long)preview.consumed,
                    (unsigned)APEX_DMD_WIDTH, (unsigned)APEX_DMD_HEIGHT);
    }
    ImGui::Separator();
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("dmd_canvas", ImVec2(APEX_DMD_WIDTH * scale, APEX_DMD_HEIGHT * scale));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(canvas_pos,
                        ImVec2(canvas_pos.x + APEX_DMD_WIDTH * scale,
                               canvas_pos.y + APEX_DMD_HEIGHT * scale),
                        IM_COL32(8, 8, 8, 255));
    /* 4-level amber ramp for 4-colour (two-plane) frames; index 0 = off. */
    static const ImU32 kAmber4[4] = {
        IM_COL32(28, 18, 6, 255), IM_COL32(120, 76, 20, 255),
        IM_COL32(190, 120, 30, 255), IM_COL32(255, 160, 40, 255)
    };
    for (size_t row = 0; row < APEX_DMD_HEIGHT; row++) {
        for (size_t col_byte = 0; col_byte < APEX_DMD_ROW_BYTES; col_byte++) {
            uint8_t bits = preview.plane[row * APEX_DMD_ROW_BYTES + col_byte];
            uint8_t bits1 = preview.two_plane
                                ? preview.plane1[row * APEX_DMD_ROW_BYTES + col_byte] : 0u;
            for (size_t bit = 0; bit < 8u; bit++) {
                ImVec2 p0(canvas_pos.x + (col_byte * 8 + bit) * scale,
                          canvas_pos.y + row * scale);
                ImU32 col;
                if (preview.two_plane) {
                    unsigned level = ((bits >> bit) & 1u) | (((bits1 >> bit) & 1u) << 1);
                    col = kAmber4[level];
                } else {
                    col = ((bits >> bit) & 1u) ? IM_COL32(255, 160, 40, 255)
                                               : IM_COL32(28, 18, 6, 255);
                }
                draw->AddRectFilled(p0, ImVec2(p0.x + scale - 1.0f, p0.y + scale - 1.0f), col);
            }
        }
    }
}

static const Bookmark *find_bookmark(const UiState *s, uint8_t bank, uint32_t addr)
{
    for (const auto &bm : s->bookmarks)
        if (bm.bank == bank && bm.addr == addr)
            return &bm;
    return nullptr;
}

/* Walk backwards from line_idx to find the nearest "; [row N]" comment that
   belongs to the same TABLE block. Returns the row index, or -1 if not found. */
static int find_table_row_index(const ApexRenderedDocument *d, size_t line_idx)
{
    static const char prefix[] = "; [row ";
    static const size_t plen   = sizeof(prefix) - 1;
    size_t i = line_idx;
    do {
        const ApexRenderedLine *l = &d->lines[i];
        if (l->has_location && l->block_kind != APEX_RENDER_BLOCK_TABLE)
            return -1;
        if (l->length >= plen && strncmp(l->text, prefix, plen) == 0)
            return atoi(l->text + plen);
    } while (i-- > 0);
    return -1;
}

/* From a line inside a TABLE block, find the table's start address (the header)
   by walking backwards to the top of the contiguous TABLE block. Returns false
   if the line is not part of a table. */
bool find_table_start(const ApexRenderedDocument *d, size_t line_idx,
                      uint8_t *out_bank, uint32_t *out_addr)
{
    size_t i = line_idx;
    bool found = false;
    uint8_t b = 0;
    uint32_t a = 0;
    for (;;) {
        const ApexRenderedLine *l = &d->lines[i];
        if (l->has_location) {
            if (l->block_kind != APEX_RENDER_BLOCK_TABLE) {
                break; /* reached the line before the table */
            }
            b = l->bank;      /* keep updating; the topmost TABLE location wins */
            a = l->cpu_addr;
            found = true;
        }
        if (i == 0) break;
        i--;
    }
    if (found) { *out_bank = b; *out_addr = a; }
    return found;
}

static bool is_dmd_fullframe_addr(const ApexProject *p, uint8_t bank, uint32_t addr)
{
    /* The byte belongs to a DMD frame only if the data range that *owns* it (the
       one with the greatest start address <= addr in this bank) is a DMD frame.
       A nearer ptr16_sprite / bytes[] / etc. range that the user classified
       after the frame takes precedence, so it is not mis-reported as DMD. */
    const DataRange *owner = NULL;
    size_t i;
    for (i = 0; i < p->data_ranges.count; i++) {
        const DataRange *dr = &p->data_ranges.items[i];
        if (dr->bank != bank || dr->addr > addr) {
            continue;
        }
        if (!owner || dr->addr > owner->addr) {
            owner = dr;
        }
    }
    return owner && owner->kind == DATA_DMD_FULLFRAME &&
           addr < owner->addr + APEX_DMD_PAGE_BYTES;
}

static bool is_sprite_addr(const ApexProject *p, uint8_t bank, uint32_t addr)
{
    size_t i;
    for (i = 0; i < p->data_ranges.count; i++) {
        const DataRange *dr = &p->data_ranges.items[i];
        if ((dr->kind == DATA_SPRITE || dr->kind == DATA_SPRITE_NOHEADER ||
             dr->kind == DATA_FAR_SPRITE) &&
            dr->bank == bank && addr == dr->addr)
            return true;
    }
    return false;
}

void render_sprite_preview(const SpritePreviewInfo &pr, float max_scale)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale_x = pr.width  > 0 ? avail.x / (float)pr.width  : 1.0f;
    float scale_y = pr.height > 0 ? avail.x / (float)pr.height : 1.0f;
    float scale = std::max(1.0f, std::min(max_scale, std::min(scale_x, scale_y)));
    ImGui::TextUnformatted(pr.title);
    ImGui::Text("Address: B%02x_A%04x  ROM: 0x%06lx",
                pr.bank, (unsigned)pr.cpu_addr & 0xffffu, (unsigned long)pr.rom_offset);
    ImGui::Text("Hdr: 0x%02x  Enc: 0x%02x  Size: %ux%u  Consumed: %lu",
                (unsigned)pr.header_type, (unsigned)pr.enc_type,
                (unsigned)pr.width, (unsigned)pr.height, (unsigned long)pr.consumed);
    ImGui::Separator();
    uint8_t row_bytes = (uint8_t)((pr.width + 7u) / 8u);
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("spr_canvas", ImVec2(pr.width * scale, pr.height * scale));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(canvas_pos,
                        ImVec2(canvas_pos.x + pr.width * scale, canvas_pos.y + pr.height * scale),
                        IM_COL32(8, 8, 8, 255));
    /* Bicolor sprites carry two planes -> 4 grey levels; monochrome uses 2. */
    static const ImU32 kGrey4[4] = {
        IM_COL32(6, 18, 28, 255), IM_COL32(60, 95, 120, 255),
        IM_COL32(110, 170, 210, 255), IM_COL32(150, 230, 255, 255)
    };
    for (uint8_t row = 0; row < pr.height; row++) {
        for (uint8_t col_byte = 0; col_byte < row_bytes; col_byte++) {
            uint8_t b0 = pr.pixels[row * row_bytes + col_byte];
            uint8_t b1 = pr.two_plane ? pr.pixels1[row * row_bytes + col_byte] : 0u;
            for (size_t bit = 0; bit < 8u; bit++) {
                int px = (int)(col_byte * 8u + bit);
                if (px >= (int)pr.width) break;
                /* LSB = leftmost pixel, same as DMD renderer */
                int level = ((b0 >> bit) & 1u) | (((b1 >> bit) & 1u) << 1);
                ImU32 col = pr.two_plane ? kGrey4[level]
                          : (level ? IM_COL32(120, 220, 255, 255) : IM_COL32(6, 18, 28, 255));
                ImVec2 p0(canvas_pos.x + px * scale, canvas_pos.y + row * scale);
                draw->AddRectFilled(p0,
                                    ImVec2(p0.x + scale - 1.0f, p0.y + scale - 1.0f), col);
            }
        }
    }
}

/* Render a short disassembly preview of up to kPreviewLines display lines
   starting at the given ROM offset / CPU address. Handles inline-byte payloads:
   after a JSR/LBSR with a known inline signature the payload bytes are shown as
   INLINE_BYTE / INLINE_WORD / INLINE_FAR lines and counted toward the budget. */
static void render_disasm_preview(const ApexProject *p, size_t rom_off,
                                   uint8_t bank, uint32_t cpu_addr)
{
    static const int kPreviewLines = 14;
    if (rom_off >= p->rom.size) return;
    char title[48];
    snprintf(title, sizeof(title), "Code preview at B%02x_A%04x",
             (unsigned)bank, (unsigned)cpu_addr & 0xffffu);
    ImGui::SeparatorText(title);

    const uint8_t *rom = p->rom.data;
    size_t pos = rom_off;
    uint32_t pc = cpu_addr;
    uint32_t bank_end = (cpu_addr < 0x8000u) ? 0x8000u : 0x10000u;
    int lines = 0;

    while (lines < kPreviewLines && pos < p->rom.size) {
        char mnem[64] = "?";
        size_t avail = p->rom.size - pos;
        Cpu6809InstrInfo info = cpu6809_disassemble_info(
            rom + pos, avail < 8u ? avail : 8u, pc, mnem, sizeof(mnem));
        if (info.size == 0) break;

        char hx[20] = "";
        size_t nb = info.size <= 4u ? info.size : 4u;
        for (size_t b = 0; b < nb; b++) {
            snprintf(hx + b * 3, sizeof(hx) - b * 3, "%02X ", rom[pos + b]);
        }
        if (info.size > 4u) { hx[12] = '.'; hx[13] = '.'; hx[14] = ' '; hx[15] = '\0'; }

        ImGui::Text("  %04X  %-15s %s", (unsigned)pc & 0xffffu, hx, mnem);
        lines++;

        pos += info.size;
        pc   = (uint32_t)((pc + info.size) & 0xffffu);

        if (info.has_target) {
            const InlineSignature *sig = inline_signature_for(&p->inline_sigs, bank, info.target);
            if (sig && pos + sig->length <= p->rom.size) {
                size_t fpos = pos;
                size_t fi, fn;
                for (fi = 0; fi < sig->schema.count && lines < kPreviewLines; fi++) {
                    const TableField *field = &sig->schema.items[fi];
                    for (fn = 0; fn < field->count && lines < kPreviewLines; fn++) {
                        if (field->kind == TABLE_BYTE && fpos < p->rom.size) {
                            ImGui::TextDisabled("            INLINE_BYTE 0x%02x", rom[fpos]);
                            fpos++;
                        } else if (field->kind == TABLE_WORD && fpos + 1 < p->rom.size) {
                            uint16_t w = (uint16_t)(((unsigned)rom[fpos] << 8) | rom[fpos + 1]);
                            ImGui::TextDisabled("            INLINE_WORD 0x%04x", (unsigned)w);
                            fpos += 2;
                        } else if (table_kind_is_far(field->kind) && fpos + 2 < p->rom.size) {
                            uint16_t ta = (uint16_t)(((unsigned)rom[fpos] << 8) | rom[fpos + 1]);
                            uint8_t  tb = rom[fpos + 2];
                            ImGui::TextDisabled("            INLINE_FAR  B%02x_A%04x",
                                               (unsigned)tb, (unsigned)ta);
                            fpos += 3;
                        } else {
                            fpos++;
                        }
                        lines++;
                    }
                }
                if (sig->schema.count == 0 && lines < kPreviewLines) {
                    /* No schema — show raw hex summary. */
                    char ihx[32] = "";
                    unsigned show = sig->length <= 6u ? sig->length : 6u;
                    for (unsigned bi = 0; bi < show && pos + bi < p->rom.size; bi++)
                        snprintf(ihx + bi * 3, sizeof(ihx) - bi * 3, "%02X ", rom[pos + bi]);
                    ImGui::TextDisabled("            ; inline[%u] %s%s",
                                       sig->length, ihx, sig->length > show ? "..." : "");
                    lines++;
                }
                pos += sig->length;
                pc   = (uint32_t)((pc + sig->length) & 0xffffu);
            }
        }

        if ((info.flags & CPU6809_FLOW_STOP) || pc >= bank_end) break;
    }
}

// --- Public Window Rendering ---

static const Label *find_explicit_entry_label(const ApexProject *project, uint8_t bank, uint32_t addr)
{
    const LabelSet *ls;
    size_t j;
    if (bank == 0xff) {
        ls = &project->system_labels;
    } else {
        int idx = bank_index_for_id(project->rom.data, project->banks, bank);
        if (idx < 0 || (size_t)idx >= project->banks)
            return nullptr;
        ls = &project->bank_labels[(size_t)idx];
    }
    for (j = 0; j < ls->count; j++) {
        if (ls->items[j].addr == addr && ls->items[j].is_explicit_entry)
            return &ls->items[j];
    }
    return nullptr;
}

/* Returns true when the instruction on `line` has a speculative addr_ref that
   is currently suppressed by a ref exclusion entry.
   Fills out_bank / out_addr with the excluded target address. */
static bool line_excluded_ref(const ApexProject *p, const ApexRenderedLine *line,
                              uint8_t *out_bank, uint32_t *out_addr)
{
    if (!line->has_location || line->kind != APEX_RENDER_LINE_INSTRUCTION) {
        return false;
    }
    const uint8_t *data;
    size_t remaining;
    if (!project_locate_rom_bytes(p, line->bank, line->cpu_addr, &data, &remaining, NULL)) {
        return false;
    }
    char inst_buf[64];
    Cpu6809InstrInfo info = cpu6809_disassemble_info(data, remaining, line->cpu_addr,
                                                     inst_buf, sizeof(inst_buf));
    if (!info.has_addr_ref || info.has_target) {
        return false;
    }
    uint8_t ref_bank = in_system_addr(info.addr_ref) ? 0xffu : line->bank;
    for (size_t i = 0; i < p->ref_exclusions.count; i++) {
        const ConfigEntry *e = &p->ref_exclusions.items[i];
        if (e->addr == info.addr_ref &&
            (!e->has_bank || e->bank == ref_bank)) {
            if (out_bank) *out_bank = ref_bank;
            if (out_addr) *out_addr = info.addr_ref;
            return true;
        }
    }
    return false;
}

/* Render a sprite or DMD preview relevant to `line`: the line's own address if
   it is a sprite/DMD, otherwise a sprite/DMD target it references (including the
   raw ptr16_sprite FDB-pointer fallback for not-yet-labeled targets).  Each
   preview is prefixed with a separator.  Returns true if a preview was drawn.
   Shared by the disassembly and hex view tooltips. */
/* Height for a no-header sprite at a ptr16_sprite/far_sprite table row, taken
   from the sprite field of the table that contains `addr`.  0 if none. */
static unsigned sprite_table_height_at(const ApexProject *p, uint8_t bank, uint32_t addr)
{
    for (size_t i = 0; i < p->tables.count; i++) {
        const TableDef *t = &p->tables.items[i];
        if (t->bank != bank || t->schema.count == 0) {
            continue;
        }
        size_t row_width = table_schema_width(&t->schema);
        if (row_width == 0) {
            continue;
        }
        uint32_t start = t->addr;
        size_t rows = t->rows;
        if (t->has_header) {
            const uint8_t *hsrc;
            size_t hrem;
            if (!project_locate_rom_bytes(p, bank, t->addr, &hsrc, &hrem, NULL) || hrem < 3u) {
                continue;
            }
            rows = ((size_t)hsrc[0] << 8) | hsrc[1];
            start = t->addr + 3u;
        }
        if (addr < start || addr >= start + rows * row_width) {
            continue;
        }
        for (size_t f = 0; f < t->schema.count; f++) {
            if (t->schema.items[f].kind == TABLE_PTR16_SPRITE ||
                t->schema.items[f].kind == TABLE_FAR_SPRITE) {
                return t->schema.items[f].param;
            }
        }
    }
    return 0;
}

/* Resolve a sprite/DMD table row's target straight from the pointer bytes and
   the table field (no-header sprites take their height from the field), so the
   preview works even when the target carries no label/classification yet.
   Returns true if a preview was drawn. */
static bool render_table_row_target_preview(const ApexProject *project,
                                            const ApexRenderedLine *line)
{
    int is_dmd = -1;        /* -1 until a sprite/DMD pointer row or range is identified */
    size_t ptr_len = 0;
    unsigned height = 0;

    /* (a) Table pointer rows render with kind INSTRUCTION (the TABLE_* pseudo-op),
       so identify them by block kind + mnemonic. */
    if (line->block_kind == APEX_RENDER_BLOCK_TABLE) {
        const char *txt = line->text;
        size_t tn = line->length;
        while (tn && (*txt == ' ' || *txt == '\t')) { txt++; tn--; }
        auto has = [&](const char *m) {
            size_t ml = strlen(m);
            return tn >= ml && memcmp(txt, m, ml) == 0;
        };
        if (has("TABLE_FAR_DMD_FULLFRAME"))      { is_dmd = 1; ptr_len = 3; }
        else if (has("TABLE_PTR_DMD_FULLFRAME")) { is_dmd = 1; ptr_len = 2; }
        else if (has("TABLE_FAR_SPRITE"))        { is_dmd = 0; ptr_len = 3; }
        else if (has("TABLE_PTR_SPRITE"))        { is_dmd = 0; ptr_len = 2; }
        if (is_dmd == 0) {
            height = sprite_table_height_at(project, line->bank, line->cpu_addr);
        }
    }

    /* (b) Standalone pointer data ranges (e.g. a byte-pair the user marked as
       ptr16_sprite): identify by the classification of the range starting here. */
    if (is_dmd < 0) {
        const DataRange *dr = data_range_at(line->bank, line->cpu_addr, &project->data_ranges);
        if (dr) {
            if (dr->kind == DATA_PTR16_SPRITE)        { is_dmd = 0; ptr_len = 2; height = dr->length; }
            else if (dr->kind == DATA_FAR_SPRITE)     { is_dmd = 0; ptr_len = 3; height = dr->length; }
            else if (dr->kind == DATA_FAR_DMD_FULLFRAME) { is_dmd = 1; ptr_len = 3; }
        }
    }
    if (is_dmd < 0) {
        return false;
    }

    const uint8_t *src;
    size_t rem;
    if (!project_locate_rom_bytes(project, line->bank, line->cpu_addr, &src, &rem, NULL) ||
        rem < ptr_len) {
        return false;
    }
    uint16_t taddr = (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
    uint8_t tbank = (ptr_len == 3) ? src[2] : ((taddr >= 0x8000u) ? 0xFFu : line->bank);

    if (is_dmd) {
        DmdPreviewInfo pr = {};
        if (decode_dmd_preview_at(project, tbank, taddr, &pr)) {
            snprintf(pr.title, sizeof(pr.title), "DMD Target");
            ImGui::Separator();
            render_dmd_preview(pr, 4.0f);
            return true;
        }
        return false;
    }
    SpritePreviewInfo pr = {};
    if (decode_sprite_preview_with_height(project, tbank, taddr, height, &pr)) {
        snprintf(pr.title, sizeof(pr.title), "Sprite Target");
        ImGui::Separator();
        render_sprite_preview(pr, 4.0f);
        return true;
    }
    return false;
}

static bool render_line_sprite_dmd_preview(const ApexProject *project,
                                           const ApexRenderedDocument *document,
                                           UiState *state, const ApexRenderedLine *line)
{
    if (!line || !line->has_location) {
        return false;
    }
    /* Table rows: resolve straight from the pointer + field (robust even for
       not-yet-classified targets, and supplies the no-header sprite height). */
    if (render_table_row_target_preview(project, line)) {
        return true;
    }

    DmdPreviewInfo dmd_pr = {};
    bool found_dmd = false;
    if (is_dmd_fullframe_addr(project, line->bank, line->cpu_addr) &&
        decode_dmd_preview_at(project, line->bank, line->cpu_addr, &dmd_pr)) {
        snprintf(dmd_pr.title, sizeof(dmd_pr.title), "DMD Preview");
        found_dmd = true;
    }
    if (!found_dmd) {
        auto ts = find_line_targets(document, state, line);
        for (auto &t : ts) {
            if (address_is_dmd_fullframe_start(project, t.bank, t.cpu_addr) &&
                decode_dmd_preview_at(project, t.bank, t.cpu_addr, &dmd_pr)) {
                snprintf(dmd_pr.title, sizeof(dmd_pr.title), "DMD Target");
                found_dmd = true;
                break;
            }
        }
    }
    if (found_dmd) {
        ImGui::Separator();
        render_dmd_preview(dmd_pr, 4.0f);
        return true;
    }

    SpritePreviewInfo spr_pr = {};
    bool found_spr = false;
    if (is_sprite_addr(project, line->bank, line->cpu_addr) &&
        decode_sprite_preview_at(project, line->bank, line->cpu_addr, &spr_pr)) {
        snprintf(spr_pr.title, sizeof(spr_pr.title), "Sprite Preview");
        found_spr = true;
    }
    if (!found_spr) {
        auto ts = find_line_targets(document, state, line);
        for (auto &t : ts) {
            if (address_is_sprite_start(project, t.bank, t.cpu_addr) &&
                decode_sprite_preview_at(project, t.bank, t.cpu_addr, &spr_pr)) {
                snprintf(spr_pr.title, sizeof(spr_pr.title), "Sprite Target");
                found_spr = true;
                break;
            }
        }
    }
    /* Fallback: for ptr16_sprite FDB rows whose target isn't labeled yet
       (unclassified), read the 2-byte pointer from ROM directly. */
    if (!found_spr && line->block_kind == APEX_RENDER_BLOCK_TABLE &&
        line->kind == APEX_RENDER_LINE_DIRECTIVE) {
        const uint8_t *fdb_src;
        size_t fdb_rem;
        if (project_locate_rom_bytes(project, line->bank, line->cpu_addr, &fdb_src,
                                     &fdb_rem, NULL) && fdb_rem >= 2u) {
            uint32_t tgt_addr = ((uint32_t)fdb_src[0] << 8) | fdb_src[1];
            uint8_t tgt_bank = (tgt_addr >= 0x8000u) ? 0xFFu : line->bank;
            if (decode_sprite_preview_at(project, tgt_bank, tgt_addr, &spr_pr)) {
                snprintf(spr_pr.title, sizeof(spr_pr.title), "Sprite Target");
                found_spr = true;
            }
        }
    }
    if (found_spr) {
        ImGui::Separator();
        render_sprite_preview(spr_pr, 4.0f);
        return true;
    }
    return false;
}

/* Hover-peek: preview the destination of a branch/call/pointer, whatever its
   kind.  Code targets get the rich disassembly preview; sprites/DMD get the
   image; strings/data/tables show their already-rendered document text so the
   preview matches exactly what the listing shows. */
static void render_target_preview(const ApexProject *p, const ApexRenderedDocument *d,
                                  UiState *state, uint8_t t_bank, uint32_t t_addr)
{
    size_t tl;
    if (!apex_render_find_line_by_address(d, t_bank, t_addr, &tl)) {
        return;
    }
    const ApexRenderedLine *tline = &d->lines[tl];
    if (!tline->has_location) {
        return;
    }
    if (tline->block_kind == APEX_RENDER_BLOCK_CODE) {
        if (tline->rom_addr < p->rom.size) {
            render_disasm_preview(p, tline->rom_addr, t_bank, t_addr);
        }
        return;
    }
    if (tline->block_kind == APEX_RENDER_BLOCK_SPRITE) {
        render_line_sprite_dmd_preview(p, d, state, tline);
        return;
    }
    /* String / data / table / unclassified: echo the rendered listing lines. */
    char title[48];
    snprintf(title, sizeof(title), "Target at B%02x_A%04x", (unsigned)t_bank,
             (unsigned)t_addr & 0xffffu);
    ImGui::SeparatorText(title);
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
    int shown = 0;
    static const int kMaxTargetLines = 12;
    for (size_t i = tl; i < d->line_count && shown < kMaxTargetLines; i++) {
        const ApexRenderedLine *l = &d->lines[i];
        if (l->kind == APEX_RENDER_LINE_LOCATION ||
            l->kind == APEX_RENDER_LINE_COMMENT) {
            continue;  /* skip anchor comments + "; referenced_by/doc" metadata */
        }
        if (l->length == 0) {
            if (shown > 0) break;  /* blank line ends the block once we've shown some */
            continue;
        }
        if (shown > 0 && l->kind == APEX_RENDER_LINE_LABEL) {
            break;  /* next block's label — stop */
        }
        ImGui::TextUnformatted(l->text, l->text + l->length);
        shown++;
    }
    ImGui::PopTextWrapPos();
}

/* Shared "Classify as ▸" submenu of pointer / sprite / DMD data kinds, used by
   both the disassembly row context menu and the hex view context menu.  Each
   item classifies the current selection (hex byte when the hex view is the edit
   target, else the selected disassembly line) via apply_data_at_selection. */
static void classify_kind_submenu(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    if (!ImGui::BeginMenu("Classify as")) {
        return;
    }
    if (ImGui::MenuItem("sprite"))             apply_data_at_selection(p, dp, s, "sprite");
    if (ImGui::MenuItem("dmd_fullframe"))      apply_data_at_selection(p, dp, s, "dmd_fullframe");
    {
        /* dmd_fullframe[N]: N bit-planes (4-colour frames are 2).  Uses the Edit
           panel's N (min 2) so the label shows it. */
        int n = s->edit_data_length > 1 ? s->edit_data_length : 2;
        char label[28], spec[28];
        snprintf(label, sizeof(label), "dmd_fullframe[%d]", n);
        snprintf(spec, sizeof(spec), "dmd_fullframe[%d]", n);
        if (ImGui::MenuItem(label)) apply_data_at_selection(p, dp, s, spec);
    }
    {
        /* bcd needs a length; use the Edit panel's current N so the label shows it. */
        int n = s->edit_data_length > 0 ? s->edit_data_length : 1;
        char label[24], spec[24];
        snprintf(label, sizeof(label), "bcd[%d]", n);
        snprintf(spec, sizeof(spec), "bcd[%d]", n);
        if (ImGui::MenuItem(label)) apply_data_at_selection(p, dp, s, spec);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("ptr16_code"))         apply_data_at_selection(p, dp, s, "ptr16_code");
    if (ImGui::MenuItem("ptr16_data"))         apply_data_at_selection(p, dp, s, "ptr16_data");
    if (ImGui::MenuItem("ptr16_string"))       apply_data_at_selection(p, dp, s, "ptr16_string");
    if (ImGui::MenuItem("ptr16_table"))        apply_data_at_selection(p, dp, s, "ptr16_table");
    if (ImGui::MenuItem("ptr16_sprite"))       apply_data_at_selection(p, dp, s, "ptr16_sprite");
    if (ImGui::MenuItem("ptr16_dmd_fullframe"))apply_data_at_selection(p, dp, s, "ptr16_dmd_fullframe");
    ImGui::Separator();
    if (ImGui::MenuItem("far_code"))           apply_data_at_selection(p, dp, s, "far_code");
    if (ImGui::MenuItem("far_data"))           apply_data_at_selection(p, dp, s, "far_data");
    if (ImGui::MenuItem("far_string"))         apply_data_at_selection(p, dp, s, "far_string");
    if (ImGui::MenuItem("far_table"))          apply_data_at_selection(p, dp, s, "far_table");
    if (ImGui::MenuItem("far_sprite"))         apply_data_at_selection(p, dp, s, "far_sprite");
    if (ImGui::MenuItem("far_dmd_fullframe"))  apply_data_at_selection(p, dp, s, "far_dmd_fullframe");
    ImGui::EndMenu();
}

/* An acknowledged warning renders as a "; WARNING_ACK ..." comment; the
   disassembly view hides those lines so acked warnings disappear from the code
   (they remain listed, green, in the Warnings panel). */
static bool is_acked_warning_line(const ApexRenderedLine *l)
{
    static const char ACK[] = "; WARNING_ACK ";
    const size_t ALEN = sizeof(ACK) - 1;
    return l->length >= ALEN && memcmp(l->text, ACK, ALEN) == 0;
}

void render_line_table(ApexProject *project, const ApexRenderedDocument **document_ptr,
                       UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;
    int selected_visible_row = -1;
    ensure_label_index(document, state);

    /* Rebuild the filtered row list only when the document (generation) or the
       filter text changed; otherwise reuse the cached vector. This runs every
       frame and the document can be ~200K lines, so the reuse matters. */
    if (state->cached_visible_gen != document->generation ||
        state->cached_visible_filter != state->filter_input) {
        state->cached_visible.clear();
        state->cached_visible.reserve(document->line_count);
        for (size_t i = 0; i < document->line_count; i++) {
            if (is_acked_warning_line(&document->lines[i]))
                continue;
            if (line_matches_filter(&document->lines[i], state->filter_input))
                state->cached_visible.push_back(i);
        }
        state->cached_visible_gen = document->generation;
        state->cached_visible_filter = state->filter_input;
    }
    const std::vector<size_t> &visible = state->cached_visible;
    /* The cursor moves without changing the visible set, so locate its row each
       frame — visible[] holds line indices in ascending order. */
    if (state->selected_line < document->line_count) {
        auto it = std::lower_bound(visible.begin(), visible.end(), state->selected_line);
        if (it != visible.end() && *it == state->selected_line)
            selected_visible_row = (int)(it - visible.begin());
    }
    /* Flow-arrow gutter constants */
    static constexpr int   FA_MAX_LANES  = 5;
    static constexpr float FA_LANE_PITCH = 8.0f;
    static constexpr float FA_MARGIN     = 4.0f;
    static constexpr float FA_GUTTER_W   = FA_MARGIN + FA_MAX_LANES * FA_LANE_PITCH + FA_MARGIN;

    if (ImGui::BeginTable("disasm", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(3, 1);
        ImGui::TableSetupColumn("##gutter", ImGuiTableColumnFlags_WidthFixed |
                                            ImGuiTableColumnFlags_NoHeaderLabel, FA_GUTTER_W);
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,   120.0f);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed,    60.0f);
        /* WidthFixed (auto-fit to content) rather than WidthStretch: a stretch
           column always shrinks to the viewport, so the table never reports
           horizontal overflow and no horizontal scrollbar appears — long lines and
           off-screen inline xref buttons get clipped with no way to reach them.
           Auto-fit lets the table grow past the viewport so ScrollX shows a usable
           horizontal scrollbar. */
        ImGui::TableSetupColumn("Text",  ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        /* Flow-arrow data collected during clipper loop, drawn after EndTable */
        struct FlowArrow { int src_row, dst_row; bool backward; int lane; ImU32 color; };
        std::vector<float>     fa_row_y(visible.size(), -1.0f);
        std::vector<FlowArrow> fa_arrows;
        float fa_win_x = ImGui::GetWindowPos().x;   /* absolute X of frozen gutter */

        ImGuiListClipper clipper;
        clipper.Begin((int)visible.size());
        if (state->request_scroll_to_selection && selected_visible_row >= 0) {
            clipper.IncludeItemByIndex(selected_visible_row);
        }
        /* Saved to detect mid-loop rerenders: classify/clear operations free and
           reallocate document->lines, making visible[] stale.  If that happens we break
           out of both loops immediately so the next frame rebuilds visible[] cleanly. */
        const ApexRenderedLine *orig_lines = document->lines;
        bool rerendered_in_loop = false;
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                size_t line_idx = visible[(size_t)row];
                if (line_idx >= document->line_count) {
                    continue;  /* defensive: never index past the current document */
                }
                const auto *line = &document->lines[line_idx];
                bool is_cursor = (line_idx == state->selected_line);
                bool in_block  = state->block_active &&
                                 line_idx >= std::min(state->block_a, state->block_b) &&
                                 line_idx <= std::max(state->block_a, state->block_b);
                bool in_range = is_cursor; /* the Selectable highlights the cursor line */
                ImGui::PushID((int)line_idx);
                ImGui::TableNextRow();
                /* PinMAME coverage overlay (lowest priority — later tints win): tint
                   executed code green, never-executed code red. */
                if (state->pinmame.cov_overlay && !state->pinmame.cov_reached.empty() &&
                    line->kind == APEX_RENDER_LINE_INSTRUCTION && line->has_location) {
                    uint32_t ckey = ((uint32_t)line->bank << 16) | (line->cpu_addr & 0xffff);
                    bool reached = state->pinmame.cov_reached.count(ckey) != 0;
                    ImU32 bg = reached
                        ? ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f, 0.70f, 0.30f, 0.16f))
                        : ImGui::ColorConvertFloat4ToU32(ImVec4(0.70f, 0.25f, 0.20f, 0.14f));
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, bg);
                }
                if (in_block && !is_cursor) {
                    /* distinct translucent-blue background for the marked block */
                    ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f, 0.45f, 0.85f, 0.28f));
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, bg);
                }
                if (line->has_conflict) {
                    ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.85f, 0.45f, 0.05f, 0.28f));
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, bg);
                }
                const Bookmark *bm = (line->has_location && !state->bookmarks.empty())
                    ? find_bookmark(state, line->bank, line->cpu_addr) : nullptr;
                if (bm) {
                    ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.60f, 0.30f, 0.85f, 0.22f));
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, bg);
                }
                /* Column 0: gutter (invisible Selectable + flow-arrow overlay) */
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable("##rowsel", in_range,
                        ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowOverlap)) {
                    handle_line_selection(state, line_idx, ImGui::GetIO().KeyShift);
                }
                // AllowWhenOverlappedByItem: needed because AllowOverlap+SpanAllColumns means
                // the text items in other columns overlap the Selectable and claim HoveredId.
                bool row_hovered        = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
                bool row_double_clicked = row_hovered && ImGui::IsMouseDoubleClicked(0);
                bool row_right_clicked  = row_hovered && ImGui::IsMouseClicked(1);
                if (row_right_clicked) {
                    if (!in_range) {
                        select_line(state, line_idx, 0);
                    }
                    ImGui::OpenPopup("row_context_menu");
                }
                /* Record row vertical centre for flow arrows */
                {
                    ImVec2 rmin = ImGui::GetItemRectMin();
                    ImVec2 rmax = ImGui::GetItemRectMax();
                    fa_row_y[(size_t)row] = (rmin.y + rmax.y) * 0.5f;
                }
                /* Collect branch target for flow arrows (code blocks only) */
                if (state->show_flow_arrows &&
                    line->kind == APEX_RENDER_LINE_INSTRUCTION &&
                    line->block_kind == APEX_RENDER_BLOCK_CODE &&
                    line->has_location &&
                    line->rom_addr < project->rom.size) {
                    const uint8_t *rb = project->rom.data + line->rom_addr;
                    size_t         rl = project->rom.size  - line->rom_addr;
                    char dummy[1];
                    Cpu6809InstrInfo finfo = cpu6809_disassemble_info(rb, rl,
                                                line->cpu_addr, dummy, sizeof(dummy));
                    if (finfo.has_target && (finfo.flags & CPU6809_TARGET_CODE)) {
                        /* Exclude JSR/LBSR (subroutine calls) */
                        const char *mt = line->text;
                        while (*mt == ' ') mt++;
                        bool is_call = (strncmp(mt,"JSR", 3)==0 && (mt[3]==' '||!mt[3])) ||
                                       (strncmp(mt,"LBSR",4)==0 && (mt[4]==' '||!mt[4]));
                        if (!is_call) {
                            size_t tgt_doc_line;
                            if (apex_render_find_line_by_address(document, line->bank,
                                                                  finfo.target, &tgt_doc_line)) {
                                /* find visible-row index of target */
                                auto it = std::lower_bound(visible.begin(), visible.end(),
                                                           tgt_doc_line);
                                if (it != visible.end() && *it == tgt_doc_line) {
                                    int tgt_row = (int)(it - visible.begin());
                                    bool bwd = tgt_row < row;
                                    ImU32 col = bwd
                                        ? IM_COL32(80,  200, 120, 200)
                                        : IM_COL32(120, 150, 220, 180);
                                    fa_arrows.push_back({row, tgt_row, bwd, -1, col});
                                }
                            }
                        }
                    }
                }

                /* Column 1: address */
                ImGui::TableSetColumnIndex(1);
                char addr_buf[32];
                if (line->has_location)
                    snprintf(addr_buf, sizeof(addr_buf), "B%02x_A%04x", line->bank,
                             (unsigned)line->cpu_addr & 0xffffu);
                else
                    addr_buf[0] = '\0';
                ImGui::TextUnformatted(addr_buf);

                ImGui::TableSetColumnIndex(2);
                if (line->kind == APEX_RENDER_LINE_LABEL && line->has_location) {
                    const Label *el = find_explicit_entry_label(project, line->bank, line->cpu_addr);
                    if (el) {
                        if (el->reached_by_flow)
                            ImGui::TextDisabled("entry~");
                        else
                            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.35f, 1.0f), "entry");
                    } else if (line->block_kind == APEX_RENDER_BLOCK_DATA &&
                               is_dmd_fullframe_addr(project, line->bank, line->cpu_addr)) {
                        ImGui::TextColored(ImVec4(1.00f, 0.30f, 0.70f, 1.0f), "dmd_fullframe");
                    } else if (line->block_kind == APEX_RENDER_BLOCK_SPRITE) {
                        ImGui::TextColored(ImVec4(0.47f, 0.86f, 1.00f, 1.0f), "sprite");
                    } else {
                        ImGui::TextUnformatted(block_name(line->block_kind));
                    }
                } else if (line->has_location && line->block_kind == APEX_RENDER_BLOCK_DATA &&
                           is_dmd_fullframe_addr(project, line->bank, line->cpu_addr)) {
                    ImGui::TextColored(ImVec4(1.00f, 0.30f, 0.70f, 1.0f), "dmd_fullframe");
                } else if (line->has_location && line->block_kind == APEX_RENDER_BLOCK_SPRITE) {
                    ImGui::TextColored(ImVec4(0.47f, 0.86f, 1.00f, 1.0f), "sprite");
                } else {
                    ImGui::TextUnformatted(block_name(line->block_kind));
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::BeginGroup();
                if (state->inline_edit_line == line_idx) {
                    /* Inline label editor (double-click a label to open). */
                    if (state->inline_edit_focus) {
                        ImGui::SetKeyboardFocusHere();
                        state->inline_edit_focus = false;
                    }
                    ImGui::SetNextItemWidth(320.0f);
                    bool commit = ImGui::InputText("##inline_label", state->inline_edit_buf,
                                       sizeof(state->inline_edit_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue |
                                       ImGuiInputTextFlags_AutoSelectAll);
                    bool cancel = ImGui::IsItemDeactivated() && !commit; /* Esc / click-away */
                    if (commit) {
                        if (state->inline_edit_buf[0] &&
                            apex_project_set_label(project, 1, state->inline_edit_bank,
                                       state->inline_edit_addr, state->inline_edit_buf) == 0) {
                            state->inline_edit_line = (size_t)-1;
                            rerender_and_reselect(project, document_ptr, state,
                                       state->inline_edit_bank, state->inline_edit_addr);
                        } else {
                            state->inline_edit_line = (size_t)-1; /* empty/failed → cancel */
                        }
                    } else if (cancel) {
                        state->inline_edit_line = (size_t)-1;
                    }
                    ImGui::EndGroup();
                } else {
                render_line_text(document, state, line);
                {
                    uint8_t excl_bank; uint32_t excl_addr;
                    if (line_excluded_ref(project, line, &excl_bank, &excl_addr)) {
                        char excl_buf[40];
                        snprintf(excl_buf, sizeof(excl_buf), "  [no ref: B%02x_A%04x]",
                                 excl_bank, (unsigned)excl_addr & 0xffff);
                        ImGui::SameLine(0, 0);
                        ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.20f, 1.0f), "%s", excl_buf);
                    }
                }
                /* Inline xref annotations for label lines.
                   These MUST stay on the same line as the label: the disasm table
                   uses ImGuiListClipper, which assumes a uniform row height.  If the
                   xref buttons stacked vertically, label rows with incoming refs would
                   be taller than the rest, the clipper would underestimate the total
                   height, and the final rows (e.g. the reset vector ".DW ENTRY_RESET")
                   would be pushed past the reachable scroll range. */
                if (line->kind == APEX_RENDER_LINE_LABEL && line->has_location) {
                    auto in_refs = find_incoming_refs(project, document, state,
                                                     line->bank, line->cpu_addr);
                    static const int kMaxInline = 3;
                    int shown = 0;
                    for (auto &r : in_refs) {
                        if (shown >= kMaxInline) break;
                        char rbuf[128];
                        if (!r.label.empty())
                            snprintf(rbuf, sizeof(rbuf), "<- %s  %s",
                                     r.kind.c_str(), r.label.c_str());
                        else
                            snprintf(rbuf, sizeof(rbuf), "<- %s  B%02x_A%04x",
                                     r.kind.c_str(), r.bank,
                                     (unsigned)r.cpu_addr & 0xffff);
                        ImGui::SameLine();
                        ImGui::PushID((int)(0xc0de0000u ^ line_idx ^ (size_t)shown));
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImVec4(0.55f, 0.75f, 0.55f, 1.0f));
                        if (ImGui::SmallButton(rbuf)) {
                            select_line(state, r.line_index, 1);
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                        shown++;
                    }
                    if ((int)in_refs.size() > kMaxInline) {
                        ImGui::SameLine();
                        ImGui::PushID((int)(0xc0de0000u ^ line_idx ^ 0xff));
                        char more[32];
                        snprintf(more, sizeof(more), "<- +%d more...",
                                 (int)in_refs.size() - kMaxInline);
                        if (ImGui::SmallButton(more)) {
                            state->request_xref_popup = true;
                            state->xref_popup_bank    = line->bank;
                            state->xref_popup_addr    = line->cpu_addr;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndGroup();
                }  /* end else: normal text (vs. inline label editor) */

                uint8_t t_bank;
                uint32_t t_addr;
                int t_far;
                bool has_pointer = resolve_pointer_target(project, line, &t_bank, &t_addr, &t_far);
                if (ImGui::IsItemHovered()) {
                    row_double_clicked |= ImGui::IsMouseDoubleClicked(0);
                }
                /* Doc-comment tooltip: collect consecutive "; doc " lines and
                   show them wrapped. */
                auto is_doc_line = [](const ApexRenderedLine *l) -> bool {
                    static const char kPrefix[] = "; doc ";
                    return l->kind == APEX_RENDER_LINE_COMMENT &&
                           l->length >= (int)(sizeof(kPrefix) - 1) &&
                           memcmp(l->text, kPrefix, sizeof(kPrefix) - 1) == 0;
                };
                if (row_hovered && is_doc_line(line)) {
                    static const char kPrefix[] = "; doc ";
                    static const int  kPLen     = (int)(sizeof(kPrefix) - 1);
                    /* Scan backward to the first doc line in this block. */
                    size_t first = line_idx;
                    while (first > 0 && is_doc_line(&document->lines[first - 1]))
                        first--;
                    /* Collect text from first doc line through end of block. */
                    std::string doc_text;
                    for (size_t di = first; di < document->line_count; di++) {
                        if (!is_doc_line(&document->lines[di])) break;
                        const ApexRenderedLine *dl = &document->lines[di];
                        if (!doc_text.empty()) doc_text += '\n';
                        int body_len = dl->length - kPLen;
                        if (body_len > 0)
                            doc_text.append(dl->text + kPLen, (size_t)body_len);
                    }
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                    ImGui::TextUnformatted(doc_text.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
                if (row_hovered &&
                    (line->kind == APEX_RENDER_LINE_INSTRUCTION ||
                     has_pointer || line->has_location)) {
                    ImGui::BeginTooltip();
                    if (line->kind == APEX_RENDER_LINE_INSTRUCTION) {
                        char mn[16];
                        size_t k = 0;
                        const char *p = line->text;
                        while (k < line->length && isspace((unsigned char)*p)) {
                            p++;
                        }
                        while (k < 15 && (p+k) < (line->text+line->length) &&
                               !isspace((unsigned char)p[k])) {
                            mn[k] = p[k];
                            k++;
                        }
                        mn[k] = 0;
                        const auto *h = lookup_cpu_help(mn);
                        if (h) {
                            ImGui::Text("Instruction: %s", h->mnemonic);
                            ImGui::Separator();
                            ImGui::Text("%s", h->desc);
                            ImGui::Text("Flags: %s", h->flags);
                            ImGui::Text("Cycles: %s", h->cycles);
                        }
                        /* Doc comment for this instruction address */
                        if (line->has_location) {
                            const char *idoc = config_doc_at(
                                &project->docs, line->bank, line->cpu_addr);
                            if (idoc && *idoc) {
                                ImGui::Separator();
                                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                                ImGui::TextUnformatted(idoc);
                                ImGui::PopTextWrapPos();
                            }
                        }
                    }
                    if (bm) {
                        ImGui::Separator();
                        ImGui::Text("Bookmark: %s", bm->name.c_str());
                    }
                    auto hw = find_hardware_in_text(line->text, line->length);
                    if (!hw.empty()) {
                        ImGui::Separator();
                        ImGui::TextUnformatted("Hardware:");
                        for (auto h : hw) {
                            ImGui::BulletText("%s ($%04X): %s", h->name, h->addr, h->desc);
                        }
                    }
                    if (has_pointer) {
                        ImGui::Separator();
                        std::string l = label_at_address(document, state, t_bank, t_addr);
                        if (!l.empty()) {
                            ImGui::Text("Jump: %s (B%02x_A%04x)", l.c_str(), t_bank,
                                        (unsigned)t_addr & 0xffffu);
                        } else {
                            ImGui::Text("Jump: B%02x_A%04x", t_bank, (unsigned)t_addr & 0xffffu);
                        }
                        render_target_preview(project, document, state, t_bank, t_addr);
                    }
                    /* Instruction: follow branch/call target, show code preview.
                       Restrict to real code — table/data pointer rows also render
                       with kind INSTRUCTION (the TABLE_*_PTR pseudo-op); those are
                       handled by the has_pointer preview above and must NOT be
                       disassembled as 6809 code (that showed junk instead of the
                       pointed-to string/data). */
                    if (line->kind == APEX_RENDER_LINE_INSTRUCTION && line->has_location &&
                        line->block_kind == APEX_RENDER_BLOCK_CODE) {
                        const uint8_t *isrc; size_t irem;
                        if (project_locate_rom_bytes(project, line->bank, line->cpu_addr,
                                                     &isrc, &irem, NULL)) {
                            char imn[64];
                            Cpu6809InstrInfo iinfo = cpu6809_disassemble_info(
                                isrc, irem < 8u ? irem : 8u, line->cpu_addr, imn, sizeof(imn));
                            if (iinfo.has_target) {
                                uint8_t tgt_bank = in_system_addr(iinfo.target) ? 0xffu : line->bank;
                                uint32_t tgt_addr = iinfo.target;
                                /* Follow far-code inline payload (e.g. WPC FarCall helper) */
                                const InlineSignature *sig = inline_signature_for(
                                    &project->inline_sigs, tgt_bank, tgt_addr);
                                if (sig) {
                                    for (size_t fi = 0; fi < sig->schema.count; fi++) {
                                        if (sig->schema.items[fi].kind == TABLE_FAR_CODE) {
                                            uint32_t pl_addr = (uint32_t)(line->cpu_addr + iinfo.size);
                                            const uint8_t *pl; size_t pl_rem;
                                            if (project_locate_rom_bytes(project, line->bank,
                                                                          pl_addr, &pl, &pl_rem, NULL)
                                                && pl_rem >= 3u) {
                                                tgt_addr = (uint32_t)(((unsigned)pl[0] << 8) | pl[1]);
                                                tgt_bank = pl[2];
                                            }
                                            break;
                                        }
                                    }
                                }
                                render_target_preview(project, document, state, tgt_bank, tgt_addr);
                            }
                        }
                    }
                    if (line->has_location) {
                        auto in_refs = find_incoming_refs(project, document, state,
                                                          line->bank, line->cpu_addr);
                        if (!in_refs.empty()) {
                            ImGui::Separator();
                            ImGui::Text("Called by (%lu):", (unsigned long)in_refs.size());
                            for (size_t ri = 0; ri < in_refs.size() && ri < 5; ri++) {
                                ImGui::BulletText("%s %s",
                                    in_refs[ri].label.empty() ? "-" : in_refs[ri].label.c_str(),
                                    in_refs[ri].kind.c_str());
                            }
                        }
                    }
                    if (line->has_location &&
                        line->block_kind == APEX_RENDER_BLOCK_TABLE) {
                        int row_idx = find_table_row_index(document, line_idx);
                        if (row_idx >= 0) {
                            ImGui::Separator();
                            ImGui::Text("Table row: %d", row_idx);
                        }
                    }
                    /* Sprite/DMD preview FIRST so it stays visible even when the
                       disassembly preview below it is long. */
                    render_line_sprite_dmd_preview(project, document, state, line);
                    /* Raw "what these bytes look like as code" preview — helpful
                       for unclassified/data bytes, but NOT for a pointer row (a
                       ptr16_string/table entry): there has_pointer is set and the
                       target's real content is already previewed above, so the
                       raw disassembly of the pointer bytes would just be noise. */
                    if (line->has_location && !has_pointer &&
                        (line->block_kind == APEX_RENDER_BLOCK_DATA ||
                         line->block_kind == APEX_RENDER_BLOCK_SPRITE ||
                         line->block_kind == APEX_RENDER_BLOCK_TABLE ||
                         line->block_kind == APEX_RENDER_BLOCK_UNCLASSIFIED)) {
                        ImGui::Separator();
                        render_disasm_preview(project, line->rom_addr, line->bank, line->cpu_addr);
                    }
                    ImGui::EndTooltip();
                }
                /* Capture line fields before the popup: any classify/clear action inside the
                   popup calls rerender_and_reselect which frees document->lines, making the
                   `line` pointer dangle.  Menu items that run AFTER the triggering item must
                   not dereference `line`. */
                const bool     pop_has_loc  = line->has_location;
                const uint8_t  pop_bank     = line->bank;
                const uint32_t pop_cpu_addr = line->cpu_addr;
                /* If the line is inside a table, where the table header is. */
                uint8_t  pop_table_bank = 0;
                uint32_t pop_table_addr = 0;
                const bool pop_in_table =
                    pop_has_loc && line->block_kind == APEX_RENDER_BLOCK_TABLE &&
                    find_table_start(document, line_idx, &pop_table_bank, &pop_table_addr);
                uint8_t  pop_excl_bank = 0;
                uint32_t pop_excl_addr = 0;
                const bool pop_has_excl =
                    pop_has_loc && line_excluded_ref(project, line, &pop_excl_bank, &pop_excl_addr);
                /* A literal candidate is a code instruction with an immediate operand
                   that resolves to an address (has_addr_ref, not a branch/call target). */
                bool pop_lit_cand = false;
                bool pop_is_lit   = false;
                bool pop_is_far   = false;
                int  pop_far_detected = -1;   /* bank guessed from a following LDA/LDB #imm8 */
                uint32_t pop_far_load_addr = 0; /* cpu addr of that bank-load instruction */
                if (pop_has_loc && line->kind == APEX_RENDER_LINE_INSTRUCTION &&
                    line->rom_addr < project->rom.size) {
                    const uint8_t *rb = project->rom.data + line->rom_addr;
                    size_t         rl = project->rom.size  - line->rom_addr;
                    char inst[64];
                    Cpu6809InstrInfo li = cpu6809_disassemble_info(rb, rl, line->cpu_addr,
                                                                   inst, sizeof(inst));
                    /* Confirm the line really renders this instruction and is not a
                       data pseudo-op (.DB/STRING/FAR_/…) whose bytes merely decode as
                       one: its text must begin with the decoded mnemonic.  This is
                       more reliable than block_kind, which can be UNKNOWN for code. */
                    const char *lt = line->text;
                    size_t      ll = line->length;
                    while (ll && (*lt == ' ' || *lt == '\t')) { lt++; ll--; }
                    size_t mlen = 0;
                    while (inst[mlen] && inst[mlen] != ' ') mlen++;
                    bool real_instr = li.size > 0 && mlen > 0 && ll >= mlen &&
                                      memcmp(lt, inst, mlen) == 0 &&
                                      (ll == mlen || lt[mlen] == ' ' || lt[mlen] == '\t');
                    pop_lit_cand = real_instr && (li.has_addr_ref && !li.has_target);
                    pop_is_lit   = apex_project_is_literal(project, pop_bank, pop_cpu_addr) != 0;
                    pop_is_far   = apex_project_far_imm_target(project, pop_bank, pop_cpu_addr,
                                                               NULL, NULL, NULL) != 0;
                    /* Guess the target bank: scan a few instructions ahead for an
                       LDA/LDB #imm8 (opcodes 0x86 / 0xC6) that loads the bank. */
                    if (pop_lit_cand && !pop_is_far) {
                        size_t off = li.size;
                        for (int step = 0; step < 4 && off < rl; step++) {
                            uint8_t opc = rb[off];
                            if ((opc == 0x86u || opc == 0xc6u) && off + 1u < rl) {
                                pop_far_detected  = rb[off + 1u];
                                pop_far_load_addr = line->cpu_addr + (uint32_t)off;
                                break;
                            }
                            Cpu6809InstrInfo nx = cpu6809_disassemble_info(
                                rb + off, rl - off, line->cpu_addr + (uint32_t)off,
                                inst, sizeof(inst));
                            if (nx.size == 0) break;
                            off += nx.size;
                        }
                    }
                }

                if (ImGui::BeginPopup("row_context_menu")) {
                    if (has_pointer && ImGui::MenuItem("Jump to target", "F / Enter")) {
                        size_t tl;
                        if (apex_render_find_line_by_address(document, t_bank, t_addr, &tl)) {
                            select_line(state, tl, 1);
                        }
                    }
                    if (pop_in_table && ImGui::MenuItem("Jump to table header", "Home")) {
                        size_t tl;
                        if (apex_render_find_line_by_address(document, pop_table_bank,
                                                             pop_table_addr, &tl)) {
                            select_line(state, tl, 1);
                        }
                    }
                    if (pop_has_loc && ImGui::MenuItem("Show incoming references", "X")) {
                        state->request_xref_popup = true;
                        state->xref_popup_bank = pop_bank;
                        state->xref_popup_addr = pop_cpu_addr;
                    }
                    if (pop_has_loc && state->pinmame.connected &&
                        ImGui::MenuItem("Set PinMAME breakpoint")) {
                        /* System addresses (>=0x8000) take no bank; paged use the
                           line's bank — a bank-aware breakpoint. */
                        int bpbank = (pop_cpu_addr >= 0x8000u) ? -1 : (int)pop_bank;
                        const char *cond = state->pinmame.bp_cond[0] ? state->pinmame.bp_cond
                                                                     : nullptr;
                        if (apex_pinmame_breakpoint(state->pinmame.port, "add",
                                                    pop_cpu_addr, bpbank, cond))
                            set_status(state, cond ? "PinMAME conditional breakpoint set"
                                                   : "PinMAME breakpoint set");
                    }
                    if (pop_has_loc && state->pinmame.connected &&
                        ImGui::BeginMenu("Set PinMAME watchpoint")) {
                        int wbank = (pop_cpu_addr >= 0x8000u) ? -1 : (int)pop_bank;
                        struct { const char *label; int mode; } wm[] = {
                            {"read", 1}, {"write", 2}, {"read+write", 3}};
                        for (auto &w : wm) {
                            if (ImGui::MenuItem(w.label)) {
                                if (apex_pinmame_watchpoint(state->pinmame.port, "add",
                                                            pop_cpu_addr, wbank, 1, w.mode))
                                    set_status(state, "PinMAME watchpoint set");
                            }
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy selection", "Ctrl+C")) {
                        copy_selection_to_clipboard(document, state);
                        set_status(state, "copied");
                    }
                    if (ImGui::MenuItem("Set block start", "A")) {
                        state->block_a = state->selected_line;
                        if (!state->block_active) state->block_b = state->block_a;
                        state->block_active = true;
                    }
                    if (ImGui::MenuItem("Set block end", "E")) {
                        state->block_b = state->selected_line;
                        if (!state->block_active) state->block_a = state->block_b;
                        state->block_active = true;
                    }
                    if (state->block_active && ImGui::MenuItem("Clear block", "Esc")) {
                        state->block_active = false;
                    }
                    if (ImGui::MenuItem("Mark as Code", "C")) {
                        apply_code_at_selection(project, document_ptr, state);
                    }
                    if (ImGui::MenuItem("Mark as Data", "D")) {
                        char spec[32];
                        snprintf(spec, sizeof(spec), "bytes[%d]",
                                 state->edit_data_length > 0 ? state->edit_data_length : 1);
                        apply_data_at_selection(project, document_ptr, state, spec);
                    }
                    if (ImGui::MenuItem("Mark as String", "S")) {
                        apply_string_at_selection(project, document_ptr, state);
                    }
                    if (ImGui::MenuItem("Mark as Table", "T")) {
                        char spec[320] = "counted(ptr16_data)";
                        if (state->edit_schema_count > 0) {
                            char schema[256];
                            fields_to_spec(schema, sizeof(schema),
                                           state->edit_schema_fields, state->edit_schema_count);
                            if (state->edit_table_is_rows) {
                                snprintf(spec, sizeof(spec), "rows[%d](%s)",
                                         state->edit_table_rows, schema);
                            } else {
                                snprintf(spec, sizeof(spec), "counted(%s)", schema);
                            }
                        }
                        apply_table_at_selection(project, document_ptr, state, spec);
                    }
                    classify_kind_submenu(project, document_ptr, state);
                    if (ImGui::MenuItem("Clear Classification", "Del")) {
                        clear_kind_at_selection(project, document_ptr, state);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Edit Label", "L")) {
                        state->request_focus_label = 1;
                    }
                    if (ImGui::MenuItem("Edit Comment", "Shift+D")) {
                        state->request_focus_doc = 1;
                    }
                    ImGui::Separator();
                    if (pop_has_loc && ImGui::MenuItem("Add Bookmark", "B")) {
                        char n[64];
                        snprintf(n, 64, "Bookmark @ B%02x_%04x", pop_bank, pop_cpu_addr);
                        state->bookmarks.push_back({pop_bank, pop_cpu_addr, n});
                        state->request_focus_new_bookmark = 1;
                        set_status(state, "bookmark added");
                    }
                    if (pop_has_loc) {
                        auto out_refs = find_outgoing_refs(project, document, state,
                                                          pop_bank, pop_cpu_addr);
                        const bool has_excl = pop_has_excl;
                        const uint8_t  excl_bank = pop_excl_bank;
                        const uint32_t excl_addr = pop_excl_addr;
                        bool any_code_ref = has_excl;
                        for (auto &ref : out_refs) {
                            if (ref.kind == "code" && ref.row_index < 0) {
                                any_code_ref = true;
                                break;
                            }
                        }
                        if (any_code_ref) {
                            ImGui::Separator();
                            /* Already-excluded ref on this instruction → offer removal */
                            if (has_excl) {
                                char item_label[64];
                                snprintf(item_label, sizeof(item_label),
                                         "Remove exclusion: B%02x_A%04x",
                                         excl_bank, (unsigned)excl_addr & 0xffff);
                                if (ImGui::MenuItem(item_label)) {
                                    apex_project_remove_ref_exclusion(project, 1,
                                                                      excl_bank, excl_addr);
                                    const ApexRenderedDocument *nd =
                                        apex_project_render(project, 1, 0);
                                    if (nd) { *document_ptr = nd; }
                                    state->labels_valid = false;
                                    state->overlay_dirty = true;
                                    set_status(state, "ref exclusion removed");
                                }
                            }
                            /* Active (non-excluded) refs → offer exclusion */
                            for (auto &ref : out_refs) {
                                if (ref.kind != "code" || ref.row_index >= 0) {
                                    continue;
                                }
                                char item_label[64];
                                snprintf(item_label, sizeof(item_label),
                                         "Exclude ref to B%02x_A%04x",
                                         ref.bank, (unsigned)ref.cpu_addr & 0xffff);
                                if (ImGui::MenuItem(item_label)) {
                                    apex_project_add_ref_exclusion(project, 1,
                                                                   ref.bank, ref.cpu_addr);
                                    const ApexRenderedDocument *nd =
                                        apex_project_render(project, 1, 0);
                                    if (nd) { *document_ptr = nd; }
                                    state->labels_valid = false;
                                    state->overlay_dirty = true;
                                    set_status(state, "ref excluded");
                                }
                            }
                        }
                    }
                    if (pop_lit_cand) {
                        ImGui::Separator();
                        if (pop_is_lit) {
                            if (ImGui::MenuItem("Clear literal (resolve operand)")) {
                                apex_project_remove_literal(project, 1, pop_bank, pop_cpu_addr);
                                const ApexRenderedDocument *nd =
                                    apex_project_render(project, 1, 0);
                                if (nd) { *document_ptr = nd; }
                                state->labels_valid = false;
                                state->overlay_dirty = true;
                                set_status(state, "literal cleared");
                            }
                        } else {
                            if (ImGui::MenuItem("Mark immediate as literal")) {
                                apex_project_add_literal(project, 1, pop_bank, pop_cpu_addr);
                                const ApexRenderedDocument *nd =
                                    apex_project_render(project, 1, 0);
                                if (nd) { *document_ptr = nd; }
                                state->labels_valid = false;
                                state->overlay_dirty = true;
                                set_status(state, "immediate marked as literal");
                            }
                        }
                    }
                    /* Far immediate: resolve the operand as an address in another
                       bank (split far pointer: LDX #addr here, LDB #bank nearby). */
                    if (pop_lit_cand) {
                        if (pop_is_far) {
                            if (ImGui::MenuItem("Clear far pointer (resolve locally)")) {
                                apex_project_clear_far_imm(project, 1, pop_bank, pop_cpu_addr);
                                const ApexRenderedDocument *nd =
                                    apex_project_render(project, 1, 0);
                                if (nd) { *document_ptr = nd; }
                                state->labels_valid = false;
                                state->overlay_dirty = true;
                                set_status(state, "far pointer cleared");
                            }
                        } else {
                            static char fbuf[8];
                            static int  ftype = 1; /* default far_code */
                            static const char *kFarTypes[] = {
                                "far_data", "far_code", "far_table",
                                "far_string", "far_sprite", "far_dmd_fullframe" };
                            if (ImGui::IsWindowAppearing()) {
                                if (pop_far_detected >= 0)
                                    snprintf(fbuf, sizeof(fbuf), "%02x", (unsigned)pop_far_detected);
                                else
                                    fbuf[0] = '\0';
                            }
                            ImGui::SetNextItemWidth(150.0f);
                            ImGui::Combo("target type##farimm", &ftype, kFarTypes,
                                         IM_ARRAYSIZE(kFarTypes));
                            ImGui::SetNextItemWidth(60.0f);
                            ImGui::InputText("target bank (hex)##farimm", fbuf, sizeof(fbuf),
                                             ImGuiInputTextFlags_CharsHexadecimal);
                            if (pop_far_load_addr)
                                ImGui::TextDisabled("bank load: B%02x_A%04x", pop_bank,
                                                    pop_far_load_addr);
                            if (ImGui::MenuItem("Resolve immediate as far pointer")) {
                                unsigned tb = 0;
                                if (sscanf(fbuf, "%x", &tb) == 1 && tb <= 0xffu) {
                                    apex_project_set_far_imm(project, 1, pop_bank, pop_cpu_addr,
                                                             (uint8_t)tb, (uint8_t)ftype,
                                                             pop_far_load_addr);
                                    const ApexRenderedDocument *nd =
                                        apex_project_render(project, 1, 0);
                                    if (nd) { *document_ptr = nd; }
                                    state->labels_valid = false;
                                    state->overlay_dirty = true;
                                    set_status(state, "far pointer resolved");
                                } else {
                                    set_status(state, "far pointer: enter a bank 00-ff");
                                }
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
                /* If a classify/clear operation triggered a rerender inside the popup,
                   document->lines has been freed and reallocated — visible[] is stale.
                   Bail out immediately; the next frame rebuilds everything cleanly. */
                if (document->lines != orig_lines) {
                    ImGui::PopID();
                    rerendered_in_loop = true;
                    break;
                }
                if (row_double_clicked) {
                    size_t tl;
                    if (line->kind == APEX_RENDER_LINE_LABEL && line->has_location) {
                        /* Double-click a label → rename it in place. */
                        state->inline_edit_line  = line_idx;
                        state->inline_edit_bank  = line->bank;
                        state->inline_edit_addr  = line->cpu_addr;
                        state->inline_edit_focus = true;
                        std::string nm = label_name(line);
                        snprintf(state->inline_edit_buf, sizeof(state->inline_edit_buf),
                                 "%s", nm.c_str());
                    } else if (has_pointer &&
                               apex_render_find_line_by_address(document, t_bank, t_addr, &tl)) {
                        select_line(state, tl, 1);
                    } else {
                        jump_to_first_line_target(document, state, line);
                    }
                }
                if (line_idx == state->selected_line && state->request_scroll_to_selection) {
                    ImGui::SetScrollHereY(0.35f);
                    state->request_scroll_to_selection = 0;
                }
                ImGui::PopID();
            }
            if (rerendered_in_loop) break;
        }
        ImGui::EndTable();

        /* --- Flow arrows --- */
        if (state->show_flow_arrows && !fa_arrows.empty()) {
            /* Lane assignment: sort by span length (shortest → innermost lane) */
            std::sort(fa_arrows.begin(), fa_arrows.end(), [](const FlowArrow &a, const FlowArrow &b) {
                return std::abs(a.dst_row - a.src_row) < std::abs(b.dst_row - b.src_row);
            });
            /* lane_end[lane] = the max row already used on that lane */
            std::vector<int> lane_max_row(FA_MAX_LANES, -1);
            for (auto &a : fa_arrows) {
                int lo = std::min(a.src_row, a.dst_row);
                int hi = std::max(a.src_row, a.dst_row);
                for (int lane = 0; lane < FA_MAX_LANES; lane++) {
                    if (lane_max_row[lane] < lo) {
                        a.lane = lane;
                        lane_max_row[lane] = hi;
                        break;
                    }
                }
            }

            /* Highlight arrows connected to the selected line */
            for (auto &a : fa_arrows) {
                bool sel = ((size_t)a.src_row < visible.size() &&
                            visible[(size_t)a.src_row] == state->selected_line) ||
                           ((size_t)a.dst_row < visible.size() &&
                            visible[(size_t)a.dst_row] == state->selected_line);
                if (sel) {
                    /* Brighten: set alpha to 255 */
                    a.color = (a.color & 0x00ffffffu) | 0xff000000u;
                }
            }

            ImDrawList *dl   = ImGui::GetWindowDrawList();
            float win_scroll = ImGui::GetScrollX();
            float gx         = fa_win_x - win_scroll; /* left edge of gutter (frozen = no scroll) */
            float gx_edge    = gx + FA_GUTTER_W;       /* right edge of gutter */

            /* clip to gutter column only */
            float wy_min = ImGui::GetWindowPos().y;
            float wy_max = wy_min + ImGui::GetWindowSize().y;
            dl->PushClipRect({gx, wy_min}, {gx_edge, wy_max}, true);

            for (auto &a : fa_arrows) {
                if (a.lane < 0) continue;               /* no lane assigned */
                float y_src = fa_row_y[(size_t)a.src_row];
                float y_dst = (a.dst_row >= 0 && (size_t)a.dst_row < fa_row_y.size())
                              ? fa_row_y[(size_t)a.dst_row] : -1.0f;
                if (y_src < 0.0f || y_dst < 0.0f) continue;

                float x_lane = gx + FA_MARGIN + (float)(a.lane + 1) * FA_LANE_PITCH;
                float thick  = ((a.color >> 24) == 255) ? 2.0f : 1.5f;

                dl->AddLine({gx_edge, y_src}, {x_lane, y_src}, a.color, thick);
                dl->AddLine({x_lane,  y_src}, {x_lane, y_dst}, a.color, thick);
                dl->AddLine({x_lane,  y_dst}, {gx_edge, y_dst}, a.color, thick);
                /* arrowhead at destination */
                float aw = 4.0f;
                dl->AddTriangleFilled({gx_edge, y_dst},
                                      {gx_edge - aw, y_dst - aw},
                                      {gx_edge - aw, y_dst + aw}, a.color);
            }
            dl->PopClipRect();
        }
    }
}

void render_label_list(const ApexRenderedDocument *document, UiState *state)
{
    ensure_label_index(document, state);
    ImGui::InputText("Search", state->label_filter_input, 128);
    std::vector<size_t> visible;
    for (size_t i = 0; i < state->cached_labels.size(); i++) {
        if (label_entry_matches_filter(state->cached_labels[i], state->label_filter_input)) {
            visible.push_back(i);
        }
    }
    if (ImGui::BeginTable("labels", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,  120.0f, 0);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed,   60.0f, 1);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.0f, 2);
        ImGui::TableHeadersRow();
        int sort_col; bool sort_asc;
        if (ui_table_sort(&sort_col, &sort_asc)) {
            const auto &cl = state->cached_labels;
            std::stable_sort(visible.begin(), visible.end(), [&](size_t ia, size_t ib) {
                const auto &a = cl[ia]; const auto &b = cl[ib];
                int c = 0;
                switch (sort_col) {
                case 0: c = ui_cmp_u32(((uint32_t)a.bank<<16)|(a.cpu_addr&0xffffu),
                                       ((uint32_t)b.bank<<16)|(b.cpu_addr&0xffffu)); break;
                case 1: c = ui_cmp_int(a.block_kind, b.block_kind); break;
                case 2: c = a.name.compare(b.name); break;
                }
                return sort_asc ? c < 0 : c > 0;
            });
        }
        ImGuiListClipper clipper;
        clipper.Begin((int)visible.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const auto &e = state->cached_labels[visible[row]];
                char a[32];
                snprintf(a, 32, "B%02x_A%04x", e.bank, (unsigned)e.cpu_addr & 0xffffu);
                ImGui::PushID((int)e.line_index);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(a, state->selected_line == e.line_index,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    select_line(state, e.line_index, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(block_name(e.block_kind));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(e.name.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void render_bank_list(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    uint8_t cur_b = 0;
    bool has_sel = false;
    if (s->selected_line < d->line_count && d->lines[s->selected_line].has_location) {
        cur_b = d->lines[s->selected_line].bank;
        has_sel = true;
    }
    if (ImGui::BeginTable("banks", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Bank ID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Selectable("System (Prime)", has_sel && cur_b == 0xffu)) {
            size_t li;
            if (find_first_line_in_bank(d, 0xffu, &li)) {
                select_line(s, li, 0);
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            size_t li;
            if (find_first_line_in_bank(d, 0xffu, &li)) {
                select_line(s, li, 1);
            }
        }
        for (size_t i = 0; i < p->banks; i++) {
            uint8_t bid = bank_id_for_index(p->banks, (int)i);
            char lbl[64];
            snprintf(lbl, 64, "Bank 0x%02x", bid);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(lbl, has_sel && cur_b == bid)) {
                size_t li;
                if (find_first_line_in_bank(d, bid, &li)) {
                    select_line(s, li, 0);
                }
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                size_t li;
                if (find_first_line_in_bank(d, bid, &li)) {
                    select_line(s, li, 1);
                }
            }
        }
        ImGui::EndTable();
    }
}

void render_transition_list(const ApexRenderedDocument *d, UiState *s)
{
    /* Collect matching transition rows (cached on generation+filter), then render
       through a clipper — a large ROM can have thousands of transitions and both
       the scan and unclipped widgets were a major per-frame cost. */
    if (s->cached_transitions_gen != d->generation ||
        s->cached_transitions_filter != s->filter_input) {
        s->cached_transitions.clear();
        for (size_t i = 0; i < d->line_count; i++) {
            const auto *l = &d->lines[i];
            if (l->transition_kind == APEX_RENDER_TRANSITION_NONE || !l->has_location ||
                !line_matches_filter(l, s->filter_input)) {
                continue;
            }
            s->cached_transitions.push_back(i);
        }
        s->cached_transitions_gen = d->generation;
        s->cached_transitions_filter = s->filter_input;
    }
    const std::vector<size_t> &rows = s->cached_transitions;
    ImGuiListClipper clipper;
    clipper.Begin((int)rows.size());
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
            size_t i = rows[(size_t)r];
            const auto *l = &d->lines[i];
            char lbl[128];
            snprintf(lbl, 128, "%s @ B%02x_A%04x", transition_name(l->transition_kind),
                     l->bank, (unsigned)l->cpu_addr & 0xffffu);
            ImGui::PushID((int)i);
            if (ImGui::Selectable(lbl, s->selected_line == i)) {
                select_line(s, i, 1);
            }
            ImGui::PopID();
        }
    }
}

/* Command palette (Ctrl+P): fuzzy-jump to any label, or type an address. */
void render_command_palette(const ApexRenderedDocument **dp, UiState *s)
{
    const ApexRenderedDocument *d = *dp;
    if (s->request_command_palette) {
        s->request_command_palette = false;
        s->cmd_palette_input[0] = '\0';
        s->cmd_palette_sel = 0;
        s->cmd_palette_focus = true;
        ImGui::OpenPopup("Command Palette");
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.3f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 400.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("Command Palette")) {
        return;
    }
    ensure_label_index(d, s);

    if (s->cmd_palette_focus) {
        ImGui::SetKeyboardFocusHere();
        s->cmd_palette_focus = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    bool entered = ImGui::InputText("##cmd", s->cmd_palette_input, sizeof(s->cmd_palette_input),
                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("label name or address (e.g. B3d_A784b, 8000) — \xE2\x86\x91\xE2\x86\x93 select, Enter jump, Esc close");
    ImGui::Separator();

    /* Optional address result on top when the query parses as an address. */
    uint8_t ab = 0; uint32_t aa = 0; size_t addr_line = 0;
    bool addr_ok = parse_target_address(s->cmd_palette_input, &ab, &aa) &&
                   apex_render_find_line_by_address(d, ab, aa, &addr_line);

    std::vector<size_t> results;  /* indices into cached_labels */
    for (size_t i = 0; i < s->cached_labels.size(); i++) {
        if (label_entry_matches_filter(s->cached_labels[i], s->cmd_palette_input)) {
            results.push_back(i);
        }
    }
    int addr_rows = addr_ok ? 1 : 0;
    int total = (int)results.size() + addr_rows;
    if (total == 0) {
        ImGui::TextDisabled("No matches.");
        ImGui::EndPopup();
        return;
    }
    if (s->cmd_palette_sel >= total) s->cmd_palette_sel = total - 1;
    if (s->cmd_palette_sel < 0)      s->cmd_palette_sel = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        s->cmd_palette_sel = (s->cmd_palette_sel + 1) % total;
        s->cmd_palette_scroll = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        s->cmd_palette_sel = (s->cmd_palette_sel + total - 1) % total;
        s->cmd_palette_scroll = true;
    }

    auto activate = [&](int idx) {
        if (addr_ok && idx == 0) {
            select_line(s, addr_line, 1);
        } else {
            const LabelIndexEntry &e = s->cached_labels[results[(size_t)(idx - addr_rows)]];
            select_line(s, e.line_index, 1);
        }
        ImGui::CloseCurrentPopup();
    };

    if (entered) {
        activate(s->cmd_palette_sel);
        ImGui::EndPopup();
        return;
    }

    ImGui::BeginChild("cmd_results", ImVec2(0.0f, 0.0f), false);
    ImGuiListClipper clip;
    clip.Begin(total);
    if (s->cmd_palette_scroll) {
        clip.IncludeItemByIndex(s->cmd_palette_sel);
    }
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            bool sel = (r == s->cmd_palette_sel);
            char buf[192];
            if (addr_ok && r == 0) {
                snprintf(buf, sizeof(buf), "\xE2\x86\x92 go to  B%02x_A%04x", ab, (unsigned)aa & 0xffffu);
            } else {
                const LabelIndexEntry &e = s->cached_labels[results[(size_t)(r - addr_rows)]];
                snprintf(buf, sizeof(buf), "%s      B%02x_A%04x", e.name.c_str(), e.bank,
                         (unsigned)e.cpu_addr & 0xffffu);
            }
            ImGui::PushID(r);
            if (ImGui::Selectable(buf, sel)) {
                activate(r);
            }
            if (sel && s->cmd_palette_scroll) {
                ImGui::SetScrollHereY(0.5f);
            }
            ImGui::PopID();
        }
    }
    s->cmd_palette_scroll = false;
    ImGui::EndChild();
    ImGui::EndPopup();
}

void render_xref_popup(ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    if (s->request_xref_popup) {
        ImGui::OpenPopup("XRefs");
        s->request_xref_popup = false;
    }
    if (ImGui::BeginPopup("XRefs")) {
        auto in = find_incoming_refs(p, d, s, s->xref_popup_bank, s->xref_popup_addr);
        ImGui::Text("Refs to B%02x_A%04x", s->xref_popup_bank, s->xref_popup_addr);
        ImGui::Separator();
        if (in.empty()) {
            ImGui::TextDisabled("None.");
        } else if (ImGui::BeginTable("xref_table", 2,
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                       ImGuiTableFlags_Resizable, ImVec2(400, 300))) {
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();
            int ref_id = 0;
            for (auto &r : in) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char lbl[128];
                if (!r.label.empty()) {
                    snprintf(lbl, 128, "%s (B%02x_A%04x)", r.label.c_str(), r.bank,
                             (unsigned)r.cpu_addr & 0xffffu);
                } else {
                    snprintf(lbl, 128, "B%02x_A%04x", r.bank, (unsigned)r.cpu_addr & 0xffffu);
                }
                /* Unique ID per row: distinct sources may share a label/address
                   string, which would otherwise collide and trip ImGui's
                   conflicting-ID warning. */
                ImGui::PushID(ref_id++);
                if (ImGui::Selectable(lbl, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    select_line(s, r.line_index, 1);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                if (r.row_index >= 0)
                    ImGui::Text("%s [%d]", r.kind.c_str(), r.row_index);
                else
                    ImGui::TextUnformatted(r.kind.c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void render_bookmark_list(const ApexRenderedDocument *d, UiState *s)
{
    if (ImGui::Button("Add Bookmark")) {
        uint8_t b;
        uint32_t a;
        if (selected_address(d, s, &b, &a)) {
            char n[64];
            snprintf(n, 64, "Bookmark @ B%02x_%04x", b, a);
            s->bookmarks.push_back({b, a, n});
            s->request_focus_new_bookmark = 1;
        }
    }
    if (ImGui::BeginTable("bookmarks", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Addr",    ImGuiTableColumnFlags_WidthFixed,  100.0f, 0);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch, 0.0f, 1);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                60.0f, 2);
        ImGui::TableHeadersRow();
        {
            int sort_col; bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc) && s->bookmarks.size() > 1) {
                std::stable_sort(s->bookmarks.begin(), s->bookmarks.end(),
                    [&](const Bookmark &a, const Bookmark &b) {
                        int c = sort_col == 1
                            ? a.name.compare(b.name)
                            : ui_cmp_u32(((uint32_t)a.bank<<16)|(a.addr&0xffffu),
                                         ((uint32_t)b.bank<<16)|(b.addr&0xffffu));
                        return sort_asc ? c < 0 : c > 0;
                    });
            }
        }
        ImGuiListClipper clipper;
        clipper.Begin((int)s->bookmarks.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                auto &bm = s->bookmarks[row];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char a[32];
                snprintf(a, 32, "B%02x_A%04x", bm.bank, bm.addr);
                if (ImGui::Selectable(a, false, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(0)) {
                    size_t li;
                    if (apex_render_find_line_by_address(d, bm.bank, bm.addr, &li)) {
                        select_line(s, li, 1);
                    }
                }
                ImGui::TableSetColumnIndex(1);
                char nb[256];
                strcpy(nb, bm.name.c_str());
                if ((size_t)row == s->bookmarks.size() - 1 && s->request_focus_new_bookmark) {
                    ImGui::SetKeyboardFocusHere();
                    s->request_focus_new_bookmark = 0;
                }
                if (ImGui::InputText("##name", nb, 256)) {
                    bm.name = nb;
                }
                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("Del")) {
                    s->bookmarks.erase(s->bookmarks.begin() + row);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void render_global_search(const ApexRenderedDocument *d, UiState *s)
{
    if (s->request_focus_global_search) {
        ImGui::SetKeyboardFocusHere();
        s->request_focus_global_search = 0;
    }
    bool changed = ImGui::InputText("##gsquery", s->global_search_input, 128);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Wildcards: * = any chars, ? = any char\n"
                          "Sequence:  lda #*\\nstb *  (\\n = next instruction)");
    ImGui::SameLine();
    ImGui::TextDisabled("(* ? \\n)");
    if (changed)
        run_global_search(d, s->global_search_input, s->search_results);
    if (!s->search_results.empty())
        ImGui::TextDisabled("%zu result(s)", s->search_results.size());
    else if (s->global_search_input[0])
        ImGui::TextDisabled("no results");
    if (ImGui::BeginTable("search_results", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,  100.0f, 0);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed,   60.0f, 1);
        ImGui::TableSetupColumn("Text",  ImGuiTableColumnFlags_WidthStretch, 0.0f, 2);
        ImGui::TableHeadersRow();
        {
            int sort_col; bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc)) {
                std::stable_sort(s->search_results.begin(), s->search_results.end(),
                    [&](size_t ia, size_t ib) {
                        const ApexRenderedLine *a = &d->lines[ia];
                        const ApexRenderedLine *b = &d->lines[ib];
                        int c = 0;
                        if (sort_col == 0)
                            c = ui_cmp_u32(((uint32_t)a->bank<<16)|(a->cpu_addr&0xffffu),
                                           ((uint32_t)b->bank<<16)|(b->cpu_addr&0xffffu));
                        else if (sort_col == 1)
                            c = ui_cmp_int(a->block_kind, b->block_kind);
                        else {
                            size_t n = a->length < b->length ? a->length : b->length;
                            c = memcmp(a->text, b->text, n);
                            if (c == 0) c = ui_cmp_sz(a->length, b->length);
                        }
                        return sort_asc ? c < 0 : c > 0;
                    });
            }
        }
        ImGuiListClipper clipper;
        clipper.Begin((int)s->search_results.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                size_t li = s->search_results[row];
                const auto *l = &d->lines[li];
                ImGui::PushID((int)li);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char a[32];
                if (l->has_location) {
                    snprintf(a, 32, "B%02x_A%04x", l->bank, (unsigned)l->cpu_addr & 0xffffu);
                } else {
                    strcpy(a, "-");
                }
                if (ImGui::Selectable(a, s->selected_line == li,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    select_line(s, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(block_name(l->block_kind));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(l->text, l->text + l->length);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

/* Parse a hex search string ("4F 5A" or "4F5A") into bytes.
   Returns the number of bytes parsed (0 if input is empty or invalid). */
static int parse_hex_search_bytes(const char *input, uint8_t *out, int max_len)
{
    int count = 0;
    const char *p = input;
    while (*p && count < max_len) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char hi = *p++;
        if (!*p) break;
        char lo = *p++;
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nibble(hi), l = nibble(lo);
        if (h < 0 || l < 0) break;
        out[count++] = (uint8_t)((h << 4) | l);
    }
    return count;
}

/* ROM-offset window [*lo, *hi) of the bank containing `off`: the 16 KB paged
   bank, or the trailing system bank.  Used to scope the byte search. */
static void hex_bank_range(const ApexProject *p, size_t off, size_t *lo, size_t *hi)
{
    if (off >= p->paged_size) {
        *lo = p->paged_size;
        *hi = p->rom.size;
    } else {
        size_t bidx = off / (size_t)APEX_BANK_SIZE;
        *lo = bidx * (size_t)APEX_BANK_SIZE;
        *hi = *lo + (size_t)APEX_BANK_SIZE;
        if (*hi > p->rom.size) *hi = p->rom.size;
    }
}

/* Byte-run equality; when ci is set, ASCII letters compare case-insensitively. */
static bool bytes_match(const uint8_t *a, const uint8_t *b, size_t n, bool ci)
{
    if (!ci) return memcmp(a, b, n) == 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (uint8_t)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (uint8_t)(y + 32);
        if (x != y) return false;
    }
    return true;
}

/* Search forward from cur_off+1 within the window [lo, hi); wraps to lo if not
   found before hi. Returns SIZE_MAX if the needle is not present in the window.
   Sets *wrapped=true on wrap. A match must lie wholly inside [lo, hi). */
static size_t hex_search_forward(const uint8_t *rom, size_t lo, size_t hi,
                                 size_t cur_off, const uint8_t *needle, size_t nlen,
                                 bool ci, bool *wrapped)
{
    *wrapped = false;
    if (nlen == 0 || hi < lo || nlen > hi - lo) return SIZE_MAX;
    size_t limit = hi - nlen; /* last valid start position, inclusive */
    /* primary pass: max(cur_off+1, lo) .. limit */
    size_t start = cur_off + 1 < lo ? lo : cur_off + 1;
    for (size_t i = start; i <= limit; i++) {
        if (bytes_match(rom + i, needle, nlen, ci)) return i;
    }
    /* wrap: lo .. min(cur_off, limit) */
    if (cur_off >= lo) {
        size_t wrap_end = cur_off < limit ? cur_off : limit;
        for (size_t i = lo; i <= wrap_end; i++) {
            if (bytes_match(rom + i, needle, nlen, ci)) { *wrapped = true; return i; }
        }
    }
    return SIZE_MAX;
}

/* Search backward from cur_off-1 within the window [lo, hi); wraps to hi. */
static size_t hex_search_backward(const uint8_t *rom, size_t lo, size_t hi,
                                  size_t cur_off, const uint8_t *needle, size_t nlen,
                                  bool ci, bool *wrapped)
{
    *wrapped = false;
    if (nlen == 0 || hi < lo || nlen > hi - lo) return SIZE_MAX;
    size_t limit = hi - nlen; /* last valid start position, inclusive */
    /* primary pass: min(cur_off-1, limit) .. lo */
    if (cur_off > lo) {
        size_t start = (cur_off - 1) < limit ? (cur_off - 1) : limit;
        for (size_t i = start + 1; i-- > lo; ) {
            if (bytes_match(rom + i, needle, nlen, ci)) return i;
        }
    }
    /* wrap: limit .. max(cur_off, lo) */
    size_t wrap_lo = cur_off > lo ? cur_off : lo;
    for (size_t i = limit + 1; i-- > wrap_lo; ) {
        if (bytes_match(rom + i, needle, nlen, ci)) { *wrapped = true; return i; }
    }
    return SIZE_MAX;
}

/* Extent (rom offsets [lo,hi)) of the classification unit containing `off`, for
   the hex view's hover highlight.  Code spans the whole contiguous code run
   (entry through end of flow); data/table/sprite/DMD units are delimited by
   their labels (one data range / one frame).  0xff-fill is not highlighted. */
static bool hex_block_extent(const ApexProject *p, const ApexRenderedDocument *d,
                             size_t off, size_t *lo, size_t *hi)
{
    size_t li;
    if (!find_line_by_rom_offset(d, off, &li)) {
        return false;
    }
    ApexRenderedBlockKind base = d->lines[li].block_kind;
    if (base == APEX_RENDER_BLOCK_FREE || base == APEX_RENDER_BLOCK_UNKNOWN) {
        return false;
    }
    bool code = (base == APEX_RENDER_BLOCK_CODE);

    size_t start_off = d->lines[li].rom_addr;
    for (size_t i = li;; i--) {
        const ApexRenderedLine *l = &d->lines[i];
        if (l->has_location) {
            if (l->block_kind != base) {
                break; /* crossed into the previous block */
            }
            start_off = l->rom_addr;
            if (!code && l->kind == APEX_RENDER_LINE_LABEL) {
                break; /* a label begins this data/table/sprite unit */
            }
        }
        if (i == 0) {
            break;
        }
    }

    size_t end_off = p->rom.size;
    for (size_t j = li + 1; j < d->line_count; j++) {
        const ApexRenderedLine *l = &d->lines[j];
        if (!l->has_location) {
            continue;
        }
        if (l->block_kind != base ||
            (!code && l->kind == APEX_RENDER_LINE_LABEL)) {
            end_off = l->rom_addr;
            break;
        }
    }
    if (end_off <= start_off) {
        end_off = start_off + 1u;
    }
    *lo = start_off;
    *hi = end_off;
    return true;
}

/* Extended block-kind for a rendered line: DATA lines get sub-kinds 7-10 for
   STRING/.DW/FAR/DMD so the Hex/ROM-Map views can colour them distinctly. */
static uint8_t hex_line_extended_kind(const ApexProject *p, const ApexRenderedLine *l)
{
    uint8_t lk = (uint8_t)l->block_kind;
    if (l->block_kind == APEX_RENDER_BLOCK_DATA && l->text && l->length >= 3) {
        const char *t = l->text;
        size_t rem = l->length;
        while (rem > 0 && (*t == ' ' || *t == '\t')) { t++; rem--; }
        if ((rem >= 12 && memcmp(t, "STRING_FIXED", 12) == 0 &&
             (rem == 12 || t[12] == ' ' || t[12] == '\t')) ||
            (rem >= 6 && memcmp(t, "STRING", 6) == 0 &&
             (rem == 6 || t[6] == ' ' || t[6] == '\t')))
            lk = 7;
        else if (rem >= 3 && memcmp(t, ".DW", 3) == 0 && (rem == 3 || t[3] == ' ' || t[3] == '\t'))
            lk = 8;
        else if (rem >= 4 && memcmp(t, "FAR_", 4) == 0)
            lk = 9;
        else if (is_dmd_fullframe_addr(p, l->bank, l->cpu_addr))
            lk = 10;
    }
    return lk;
}

/* Build (once per document generation) the full per-ROM-byte extended kind map. */
static void ensure_hex_kind_map(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    if (s->cached_hex_kinds_gen == d->generation &&
        s->cached_hex_kinds.size() == p->rom.size) {
        return;
    }
    size_t rom_size = p->rom.size, fill = 0;
    uint8_t cur = (uint8_t)APEX_RENDER_BLOCK_UNKNOWN;
    s->cached_hex_kinds.assign(rom_size, (uint8_t)APEX_RENDER_BLOCK_UNKNOWN);
    for (size_t li = 0; li < d->line_count && fill < rom_size; li++) {
        const ApexRenderedLine *l = &d->lines[li];
        if (!l->has_location) continue;
        uint8_t lk = hex_line_extended_kind(p, l);
        if (l->rom_addr <= fill) {
            cur = lk;
        } else {
            size_t end = l->rom_addr < rom_size ? l->rom_addr : rom_size;
            while (fill < end) s->cached_hex_kinds[fill++] = cur;
            cur = lk;
        }
    }
    while (fill < rom_size) s->cached_hex_kinds[fill++] = cur;
    s->cached_hex_kinds_gen = d->generation;
}

void render_hex_view(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    if (!p || !dp || !*dp || !p->rom.data || p->rom.size == 0) {
        ImGui::TextDisabled("No ROM loaded.");
        return;
    }
    const ApexRenderedDocument *d = *dp;

    /* Sync disasm → hex cursor: when the disassembly selection changes, move the hex
       cursor to the corresponding ROM offset and request a scroll. */
    if (s->selected_line != s->hex_prev_selected_line) {
        if (d->line_count > 0 && s->selected_line < d->line_count &&
                d->lines[s->selected_line].has_location &&
                d->lines[s->selected_line].rom_addr < p->rom.size) {
            s->hex_selected_offset = d->lines[s->selected_line].rom_addr;
            s->hex_active = true;
            /* The disassembly selection drove this change, so the hex cursor only
               follows for display — the disassembly is the edit target. */
            s->hex_is_edit_target = false;
            s->hex_request_follow = 1;
        }
        s->hex_prev_selected_line = s->selected_line;
    }

    static const ImVec4 kind_colors[] = {
        ImVec4(0.50f, 0.50f, 0.50f, 1.0f), /* [0] UNKNOWN       — gray       */
        ImVec4(0.40f, 0.90f, 0.40f, 1.0f), /* [1] CODE          — green      */
        ImVec4(0.45f, 0.70f, 1.00f, 1.0f), /* [2] DATA (.DB)    — blue       */
        ImVec4(0.95f, 0.65f, 0.20f, 1.0f), /* [3] TABLE         — orange     */
        ImVec4(0.65f, 0.65f, 0.65f, 1.0f), /* [4] UNCLASSIFIED  — light gray */
        ImVec4(0.55f, 0.35f, 0.10f, 1.0f), /* [5] FREE (0xFF)   — dark amber */
        ImVec4(0.47f, 0.86f, 1.00f, 1.0f), /* [6] SPRITE        — sky blue   */
        ImVec4(0.90f, 0.55f, 0.90f, 1.0f), /* [7] STRING        — purple     */
        ImVec4(0.30f, 0.90f, 0.90f, 1.0f), /* [8] .DW           — cyan       */
        ImVec4(1.00f, 0.40f, 0.35f, 1.0f), /* [9] FAR pointer   — red        */
        ImVec4(1.00f, 0.30f, 0.70f, 1.0f), /* [10] DMD fullframe — magenta   */
    };
    static const size_t kind_colors_count = sizeof(kind_colors) / sizeof(kind_colors[0]);

    const int bytes_per_row = 16;
    const int total_rows = (int)((p->rom.size + (size_t)(bytes_per_row - 1)) / (size_t)bytes_per_row);
    const float row_h   = ImGui::GetTextLineHeightWithSpacing();
    const float char_w  = ImGui::CalcTextSize("0").x;
    /* Layout: "000000 Bff_Affff: " = 18 chars, then 16 × "xx " (3 chars each), 2-char gap, 16 ASCII chars */
    const float hex_x0  = char_w * 18.5f;
    const float gap_w   = char_w * 2.0f;
    const float asc_x   = hex_x0 + (float)bytes_per_row * char_w * 3.0f + gap_w;

    /* Inspector strip at bottom: search row + address/value info row. */
    const float inspector_h = (row_h + ImGui::GetStyle().ItemSpacing.y) * 2.0f +
                              ImGui::GetStyle().SeparatorTextBorderSize;

    /* Search strip (rendered before the child so it sits above the hex grid). */
    {
        /* Ctrl+F focuses search when the hex panel is active. */
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            s->request_focus_hex_search = 1;
        }
        if (s->request_focus_hex_search) {
            ImGui::SetKeyboardFocusHere();
            s->request_focus_hex_search = 0;
        }
        float btn_w = ImGui::CalcTextSize("Next").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float bank_w = ImGui::CalcTextSize("Bank only").x + ImGui::GetFrameHeight() +
                       ImGui::GetStyle().ItemSpacing.x * 2.0f;
        float ascii_w = ImGui::CalcTextSize("ASCII").x + ImGui::GetFrameHeight() +
                        ImGui::GetStyle().ItemSpacing.x * 2.0f;
        float ci_w = ImGui::CalcTextSize("Aa").x + ImGui::GetFrameHeight() +
                     ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x -
                                (btn_w + ImGui::GetStyle().ItemSpacing.x) * 2.0f -
                                bank_w - ascii_w - ci_w - 2.0f);
        bool enter_next = ImGui::InputText("##hexsearch", s->hex_search_input,
                                           sizeof(s->hex_search_input),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        bool do_next = enter_next || ImGui::Button("Next##hexsrch");
        ImGui::SameLine();
        bool do_prev = ImGui::Button("Prev##hexsrch");
        ImGui::SameLine();
        ImGui::Checkbox("ASCII", &s->hex_search_ascii);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Search the query as ASCII text (e.g. FREE) instead of hex bytes.\n"
                              "Finds strings in the ROM even where they are not yet classified.");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!s->hex_search_ascii);
        ImGui::Checkbox("Aa", &s->hex_search_ci);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Case-insensitive (ASCII text search only)");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Bank only", &s->hex_search_bank_only);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Restrict the search to the bank the cursor is in");
        }

        if ((do_next || do_prev) && s->hex_search_input[0]) {
            uint8_t needle[32];
            int nlen;
            if (s->hex_search_ascii) {
                size_t n = strnlen(s->hex_search_input, sizeof(s->hex_search_input));
                if (n > sizeof(needle)) n = sizeof(needle);
                memcpy(needle, s->hex_search_input, n);
                nlen = (int)n;
            } else {
                nlen = parse_hex_search_bytes(s->hex_search_input, needle, (int)sizeof(needle));
            }
            if (nlen > 0) {
                bool wrapped = false;
                size_t cur = s->hex_active ? s->hex_selected_offset : 0;
                size_t lo = 0, hi = p->rom.size;
                if (s->hex_search_bank_only) {
                    hex_bank_range(p, cur, &lo, &hi);
                }
                bool ci = s->hex_search_ascii && s->hex_search_ci;
                size_t found = do_next
                    ? hex_search_forward (p->rom.data, lo, hi, cur, needle, (size_t)nlen, ci, &wrapped)
                    : hex_search_backward(p->rom.data, lo, hi, cur, needle, (size_t)nlen, ci, &wrapped);
                if (found != SIZE_MAX) {
                    s->hex_selected_offset = found;
                    s->hex_active          = true;
                    s->hex_is_edit_target  = true;
                    s->hex_request_follow  = 1;
                    size_t li;
                    if (find_line_by_rom_offset(*dp, found, &li)) {
                        select_line(s, li, 1);
                        s->hex_prev_selected_line = s->selected_line;
                    }
                    set_status(s, wrapped ? "search: found (wrapped)" : "search: found");
                } else {
                    set_status(s, s->hex_search_bank_only ? "search: not found in bank"
                                                          : "search: not found");
                }
            } else {
                set_status(s, s->hex_search_ascii ? "search: empty query"
                                                   : "search: invalid hex");
            }
        }
    }

    /* Legend for the code-annotation corner marks (the label colors match the
       corner-mark colors; no glyph, since the default font has no box char). */
    {
        ImGui::TextDisabled("code marks:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.27f, 0.90f, 0.35f, 1.0f), "entry");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.43f, 1.0f, 1.0f), "entry+inline");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.27f, 0.67f, 1.0f, 1.0f), "end:flow-stop");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.24f, 0.16f, 1.0f), "end:into-data");
    }

    ImGui::BeginChild("hex_grid", ImVec2(0.0f, -inspector_h), false);
    s->hex_window_focused = ImGui::IsWindowFocused();

    /* Keyboard cursor navigation (arrow keys / PgUp / PgDn) when hex view is focused. */
    if (s->hex_window_focused && s->hex_active && p->rom.size > 0) {
        auto hex_move = [&](int64_t delta) {
            int64_t next = (int64_t)s->hex_selected_offset + delta;
            if (next < 0) next = 0;
            if (next >= (int64_t)p->rom.size) next = (int64_t)p->rom.size - 1;
            s->hex_selected_offset = (size_t)next;   /* cursor only; block untouched */
            s->hex_is_edit_target  = true;
            s->hex_request_follow  = 1;
            size_t li;
            if (find_line_by_rom_offset(*dp, (size_t)next, &li)) {
                select_line(s, li, 1);
                s->hex_prev_selected_line = s->selected_line;
            }
        };
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  hex_move(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) hex_move(+1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    hex_move(-(int64_t)bytes_per_row);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  hex_move(+(int64_t)bytes_per_row);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))     hex_move(-(int64_t)bytes_per_row * 16);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown))   hex_move(+(int64_t)bytes_per_row * 16);
        /* Block hotkeys (A/E/Esc) are handled centrally in the main hotkey block
           so Escape is not swallowed by ImGui keyboard-nav in the focused child. */
    }

    /* Scroll to cursor when requested (must be called inside BeginChild). */
    if (s->hex_request_follow && s->hex_active && s->hex_selected_offset < p->rom.size) {
        float target = (float)(s->hex_selected_offset / (size_t)bytes_per_row) * row_h
                       - ImGui::GetWindowHeight() * 0.35f;
        if (target < 0.0f) {
            target = 0.0f;
        }
        ImGui::SetScrollY(target);
        s->hex_request_follow = 0;
    }

    /* Must be obtained after BeginChild so it belongs to the child window's draw layer. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool open_ctx = false;

    /* Classification block under the cursor — computed once per frame from last
       frame's hover (one-frame lag is imperceptible) and drawn as a faint
       background spanning the whole block so its extent is visible.  Must live
       outside the clipper Step loop, which iterates several times per frame. */
    size_t blk_lo = 0, blk_hi = 0;
    bool blk_show = false;
    if (s->hex_hover_valid && s->hex_hover_off < p->rom.size) {
        blk_show = hex_block_extent(p, d, s->hex_hover_off, &blk_lo, &blk_hi);
    }
    s->hex_hover_valid = 0; /* set again below if a cell is hovered this frame */

    ImGuiListClipper clipper;
    clipper.Begin(total_rows);
    while (clipper.Step()) {
        size_t vis_start = (size_t)clipper.DisplayStart * (size_t)bytes_per_row;
        size_t vis_end   = std::min(p->rom.size, (size_t)clipper.DisplayEnd * (size_t)bytes_per_row);
        if (vis_end <= vis_start) {
            continue; /* safety: should not happen with valid clipper */
        }

        /* Precompute block-kind for every byte in the visible range.
           Single forward pass through the (rom-address-ordered) document lines:
           lines before vis_start track the current block kind; lines within or
           after vis_start fill the gap up to their start address.
           Non-located lines (comments, section headers) have no ROM bytes and
           are skipped entirely. */
        size_t vis_count = vis_end - vis_start;
        /* Extended block-kind per byte, built once per document generation and
           cached (was an O(line_count) forward-fill every frame). */
        ensure_hex_kind_map(p, d, s);
        const std::vector<uint8_t> &kinds_full = s->cached_hex_kinds;

        /* Per-byte code annotations, drawn as small corner marks on the hex digit:
             top-left    = entry point (green) / entry with inline params (magenta)
             bottom-right = code-block end — clean flow-stop (blue, RTS/RTI/JMP/
                            PULS..PC/BRA) vs. falls straight into a non-code block
                            (red — often an undeclared inline-byte callee). */
        const uint8_t ANNOT_ENTRY        = 1u;
        const uint8_t ANNOT_ENTRY_INLINE = 2u;
        const uint8_t ANNOT_END_CLEAN    = 4u;
        const uint8_t ANNOT_END_FALL     = 8u;
        std::vector<uint8_t> annot(vis_count, 0u);
        {
            size_t li0 = 0;
            find_line_by_rom_offset(d, vis_start, &li0);
            const ApexRenderedLine *last_code = NULL;
            for (size_t li = li0; li < d->line_count; li++) {
                const ApexRenderedLine *l = &d->lines[li];
                if (!l->has_location) continue;
                bool past = l->rom_addr >= vis_end;
                if (l->block_kind == APEX_RENDER_BLOCK_CODE) {
                    if (!past && l->kind == APEX_RENDER_LINE_LABEL &&
                        l->rom_addr >= vis_start) {
                        bool is_entry  = find_explicit_entry_label(p, l->bank, l->cpu_addr) != NULL;
                        bool has_inline = inline_signature_for(&p->inline_sigs,
                                                               l->bank, l->cpu_addr) != NULL;
                        if (is_entry || has_inline) {
                            annot[l->rom_addr - vis_start] |= ANNOT_ENTRY;
                            if (has_inline)
                                annot[l->rom_addr - vis_start] |= ANNOT_ENTRY_INLINE;
                        }
                    }
                    if (l->kind == APEX_RENDER_LINE_INSTRUCTION) last_code = l;
                    if (past) break;  /* code continues past the view */
                } else {
                    /* code -> non-code transition: classify the last code instr */
                    if (last_code && last_code->rom_addr >= vis_start &&
                        last_code->rom_addr < vis_end) {
                        const uint8_t *isrc; size_t irem;
                        if (project_locate_rom_bytes(p, last_code->bank, last_code->cpu_addr,
                                                     &isrc, &irem, NULL)) {
                            char mn[32];
                            Cpu6809InstrInfo info = cpu6809_disassemble_info(
                                isrc, irem < 8u ? irem : 8u, last_code->cpu_addr, mn, sizeof(mn));
                            annot[last_code->rom_addr - vis_start] |=
                                (info.flags & CPU6809_FLOW_STOP) ? ANNOT_END_CLEAN
                                                                 : ANNOT_END_FALL;
                        }
                    }
                    last_code = NULL;
                    if (past) break;
                }
            }
        }

        for (int row_idx = clipper.DisplayStart; row_idx < clipper.DisplayEnd; row_idx++) {
            size_t row_start = (size_t)row_idx * (size_t)bytes_per_row;

            /* Address label: ROM offset + virtual bank/CPU address */
            {
                uint8_t vbank; uint32_t vcpu;
                char addr_buf[32];
                if (rom_offset_to_cpu_address(p, row_start, &vbank, &vcpu)) {
                    if (vbank == 0xffu) {
                        snprintf(addr_buf, sizeof(addr_buf), "%06lx Bff_A%04x:",
                                 (unsigned long)row_start, (unsigned)vcpu);
                    } else {
                        snprintf(addr_buf, sizeof(addr_buf), "%06lx B%02x_A%04x:",
                                 (unsigned long)row_start, (unsigned)vbank, (unsigned)vcpu);
                    }
                } else {
                    snprintf(addr_buf, sizeof(addr_buf), "%06lx            :",
                             (unsigned long)row_start);
                }
                ImGui::TextDisabled("%s", addr_buf);
            }

            /* Hex bytes */
            size_t rng_lo = s->hex_has_range
                ? std::min(s->hex_anchor_offset, s->hex_block_end) : 0;
            size_t rng_hi = s->hex_has_range
                ? std::max(s->hex_anchor_offset, s->hex_block_end) : 0;
            for (int col = 0; col < bytes_per_row; col++) {
                size_t o = row_start + (size_t)col;
                if (o >= p->rom.size) {
                    break;
                }
                uint8_t v        = p->rom.data[o];
                bool is_cur      = s->hex_active && o == s->hex_selected_offset;
                bool is_in_range = s->hex_active && s->hex_has_range
                                   && o >= rng_lo && o <= rng_hi;
                uint8_t ki  = o < kinds_full.size() ? kinds_full[o] : 0u;
                const ImVec4 &tc = kind_colors[ki < kind_colors_count ? ki : 0u];

                bool is_in_block = blk_show && o >= blk_lo && o < blk_hi;
                ImGui::SameLine(hex_x0 + (float)col * char_w * 3.0f);
                if (is_in_block && !is_cur && !is_in_range) {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(pos,
                        ImVec2(pos.x + char_w * 2.2f, pos.y + ImGui::GetTextLineHeight()),
                        IM_COL32(140, 170, 240, 55));
                }
                if (is_cur) {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(pos,
                        ImVec2(pos.x + char_w * 2.2f, pos.y + ImGui::GetTextLineHeight()),
                        IM_COL32(255, 220, 0, 255));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
                } else if (is_in_range) {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(pos,
                        ImVec2(pos.x + char_w * 2.2f, pos.y + ImGui::GetTextLineHeight()),
                        IM_COL32(60, 140, 220, 130));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, tc);
                }
                char hbuf[3];
                snprintf(hbuf, sizeof(hbuf), "%02x", v);
                ImGui::TextUnformatted(hbuf);
                ImGui::PopStyleColor();
                if (!s->bookmarks.empty()) {
                    uint8_t bk_bank = 0; uint32_t bk_addr = 0;
                    if (rom_offset_to_cpu_address(p, o, &bk_bank, &bk_addr) &&
                        find_bookmark(s, bk_bank, bk_addr)) {
                        ImVec2 rmin = ImGui::GetItemRectMin();
                        ImVec2 rmax = ImGui::GetItemRectMax();
                        rmax.x += char_w * 0.15f;
                        dl->AddRect(rmin, rmax, IM_COL32(180, 100, 255, 220), 0.0f, 0, 1.5f);
                    }
                }
                {
                    uint8_t an = (o >= vis_start && o < vis_end) ? annot[o - vis_start] : 0u;
                    if (an) {
                        ImVec2 rmin = ImGui::GetItemRectMin();
                        ImVec2 rmax = ImGui::GetItemRectMax();
                        float m = char_w * 0.55f;
                        if (an & ANNOT_ENTRY) {
                            ImU32 c = (an & ANNOT_ENTRY_INLINE) ? IM_COL32(255, 110, 255, 255)
                                                               : IM_COL32(70, 230, 90, 255);
                            dl->AddRectFilled(rmin, ImVec2(rmin.x + m, rmin.y + m), c);
                        }
                        if (an & (ANNOT_END_CLEAN | ANNOT_END_FALL)) {
                            ImU32 c = (an & ANNOT_END_FALL) ? IM_COL32(255, 60, 40, 255)
                                                            : IM_COL32(70, 170, 255, 255);
                            dl->AddRectFilled(ImVec2(rmax.x - m, rmax.y - m), rmax, c);
                        }
                    }
                }

                if (ImGui::IsItemHovered()) {
                    s->hex_hover_off = o;       /* drives next frame's block highlight */
                    s->hex_hover_valid = 1;
                    ImGui::BeginTooltip();
                    uint8_t bank = 0;
                    uint32_t cpu_addr = 0;
                    bool has_cpu = rom_offset_to_cpu_address(p, o, &bank, &cpu_addr) != 0;
                    if (has_cpu) {
                        ImGui::Text("B%02x:A%04x  ROM:0x%06lx", bank, cpu_addr, (unsigned long)o);
                    } else {
                        ImGui::Text("ROM:0x%06lx", (unsigned long)o);
                    }
                    ImGui::Text("$%02X  %u  '%c'", v, v, (v >= 32 && v <= 126) ? (char)v : '.');
                    if (o + 1 < p->rom.size) {
                        ImGui::Text("BE16:$%04X", ((uint16_t)v << 8) | p->rom.data[o + 1]);
                    }
                    if (has_cpu) {
                        size_t li;
                        bool have_line = find_line_by_rom_offset(d, o, &li);
                        /* Sprite/DMD preview first so it stays visible above a
                           potentially long disassembly preview. */
                        if (have_line) {
                            render_line_sprite_dmd_preview(p, d, s, &d->lines[li]);
                        }
                        bool is_data = !have_line ||
                                       d->lines[li].block_kind == APEX_RENDER_BLOCK_DATA ||
                                       d->lines[li].block_kind == APEX_RENDER_BLOCK_TABLE ||
                                       d->lines[li].block_kind == APEX_RENDER_BLOCK_UNCLASSIFIED;
                        if (is_data) render_disasm_preview(p, o, bank, cpu_addr);
                    }
                    uint8_t an = (o >= vis_start && o < vis_end) ? annot[o - vis_start] : 0u;
                    if (an & ANNOT_ENTRY)
                        ImGui::TextColored(
                            (an & ANNOT_ENTRY_INLINE) ? ImVec4(1.0f, 0.43f, 1.0f, 1.0f)
                                                      : ImVec4(0.27f, 0.90f, 0.35f, 1.0f),
                            (an & ANNOT_ENTRY_INLINE) ? "entry (inline params)"
                                                      : "entry point");
                    if (an & ANNOT_END_CLEAN)
                        ImGui::TextColored(ImVec4(0.27f, 0.67f, 1.0f, 1.0f),
                                           "code-block end (flow-stop)");
                    if (an & ANNOT_END_FALL)
                        ImGui::TextColored(ImVec4(1.0f, 0.24f, 0.16f, 1.0f),
                                           "code falls into non-code (inline bytes?)");
                    ImGui::EndTooltip();
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    bool shift = ImGui::GetIO().KeyShift;
                    if (shift && s->hex_active) {
                        /* set/extend the block, anchored at the cursor */
                        if (!s->hex_has_range) {
                            s->hex_anchor_offset = s->hex_selected_offset;
                            s->hex_has_range     = true;
                        }
                        s->hex_block_end       = o;
                        s->hex_selected_offset = o;   /* cursor follows */
                    } else {
                        s->hex_selected_offset = o;   /* cursor only; block untouched */
                    }
                    s->hex_active = true;
                    s->hex_is_edit_target = true;
                    size_t li;
                    if (find_line_by_rom_offset(d, o, &li)) {
                        select_line(s, li, 1);
                        s->hex_prev_selected_line = s->selected_line;
                    }
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    s->hex_selected_offset = o;
                    s->hex_active = true;
                    s->hex_is_edit_target = true;
                    size_t li;
                    if (find_line_by_rom_offset(d, o, &li)) {
                        select_line(s, li, 1);
                        s->hex_prev_selected_line = s->selected_line;
                    }
                    open_ctx = true;
                }
            }

            /* ASCII column */
            for (int col = 0; col < bytes_per_row; col++) {
                size_t o = row_start + (size_t)col;
                if (o >= p->rom.size) {
                    break;
                }
                uint8_t v        = p->rom.data[o];
                bool is_cur      = s->hex_active && o == s->hex_selected_offset;
                bool is_in_range = s->hex_active && s->hex_has_range
                                   && o >= rng_lo && o <= rng_hi;
                uint8_t ki  = o < kinds_full.size() ? kinds_full[o] : 0u;
                const ImVec4 &tc = kind_colors[ki < kind_colors_count ? ki : 0u];

                bool is_in_block = blk_show && o >= blk_lo && o < blk_hi;
                ImGui::SameLine(asc_x + (float)col * char_w);
                if (is_in_block && !is_cur && !is_in_range) {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(pos,
                        ImVec2(pos.x + char_w, pos.y + ImGui::GetTextLineHeight()),
                        IM_COL32(140, 170, 240, 55));
                }
                if (is_cur) {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(pos,
                        ImVec2(pos.x + char_w, pos.y + ImGui::GetTextLineHeight()),
                        IM_COL32(255, 220, 0, 255));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
                } else if (is_in_range) {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(pos,
                        ImVec2(pos.x + char_w, pos.y + ImGui::GetTextLineHeight()),
                        IM_COL32(60, 140, 220, 130));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, tc);
                }
                char ch[2] = {(v >= 32 && v <= 126) ? (char)v : '.', '\0'};
                ImGui::TextUnformatted(ch);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    s->hex_hover_off = o;
                    s->hex_hover_valid = 1;
                }

                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    bool shift = ImGui::GetIO().KeyShift;
                    if (shift && s->hex_active) {
                        /* set/extend the block, anchored at the cursor */
                        if (!s->hex_has_range) {
                            s->hex_anchor_offset = s->hex_selected_offset;
                            s->hex_has_range     = true;
                        }
                        s->hex_block_end       = o;
                        s->hex_selected_offset = o;   /* cursor follows */
                    } else {
                        s->hex_selected_offset = o;   /* cursor only; block untouched */
                    }
                    s->hex_active = true;
                    s->hex_is_edit_target = true;
                    size_t li;
                    if (find_line_by_rom_offset(d, o, &li)) {
                        select_line(s, li, 1);
                        s->hex_prev_selected_line = s->selected_line;
                    }
                }
            }
        }
    }
    clipper.End();
    ImGui::EndChild(); /* hex_grid */

    /* Context menu — opened via right-click on a hex byte */
    if (open_ctx) {
        ImGui::OpenPopup("hex_ctx");
    }
    if (ImGui::BeginPopup("hex_ctx")) {
        if (ImGui::MenuItem(s->hex_has_range ? "Copy block as hex" : "Copy byte as hex", "Ctrl+C")) {
            copy_hex_block_to_clipboard(p, s);
            set_status(s, "copied hex bytes");
        }
        if (s->hex_has_range && ImGui::MenuItem("Clear block", "Esc")) {
            s->hex_has_range = false;
        }
        if (ImGui::MenuItem("Mark as Code",   "C")) { apply_code_at_selection(p, dp, s); }
        if (s->hex_has_range) {
            size_t rlo = std::min(s->hex_anchor_offset, s->hex_block_end);
            size_t rhi = std::max(s->hex_anchor_offset, s->hex_block_end);
            size_t rn  = rhi - rlo + 1;
            char blabel[48], slabel[48], clabel[48];
            snprintf(blabel, sizeof(blabel), "Assign bytes[%zu]", rn);
            snprintf(slabel, sizeof(slabel), "Assign string[%zu]", rn);
            snprintf(clabel, sizeof(clabel), "Assign bcd[%zu]", rn);
            if (ImGui::MenuItem(blabel)) {
                char spec[32];
                snprintf(spec, sizeof(spec), "bytes[%zu]", rn);
                apply_data_at_selection(p, dp, s, spec);
            }
            if (ImGui::MenuItem(slabel)) {
                char spec[32];
                snprintf(spec, sizeof(spec), "string[%zu]", rn);
                apply_data_at_selection(p, dp, s, spec);
            }
            if (ImGui::MenuItem(clabel)) {
                char spec[32];
                snprintf(spec, sizeof(spec), "bcd[%zu]", rn);
                apply_data_at_selection(p, dp, s, spec);
            }
        } else {
            if (ImGui::MenuItem("Mark as Data", "D")) {
                char spec[32];
                snprintf(spec, sizeof(spec), "bytes[%d]",
                         s->edit_data_length > 0 ? s->edit_data_length : 1);
                apply_data_at_selection(p, dp, s, spec);
            }
        }
        if (ImGui::MenuItem("Mark as String", "S")) { apply_string_at_selection(p, dp, s); }
        if (ImGui::MenuItem("Mark as Table",  "T")) {
            char spec[320] = "counted(ptr16_data)";
            if (s->edit_schema_count > 0) {
                char schema[256];
                fields_to_spec(schema, sizeof(schema),
                               s->edit_schema_fields, s->edit_schema_count);
                if (s->edit_table_is_rows) {
                    snprintf(spec, sizeof(spec), "rows[%d](%s)", s->edit_table_rows, schema);
                } else {
                    snprintf(spec, sizeof(spec), "counted(%s)", schema);
                }
            }
            apply_table_at_selection(p, dp, s, spec);
        }
        classify_kind_submenu(p, dp, s);
        if (ImGui::MenuItem("Clear Classification", "Del")) { clear_kind_at_selection(p, dp, s); }
        ImGui::Separator();
        if (ImGui::MenuItem("Edit Label",   "L"))       { s->request_focus_label = 1; }
        if (ImGui::MenuItem("Edit Comment", "Shift+D")) { s->request_focus_doc = 1; }
        ImGui::Separator();
        {
            uint8_t b; uint32_t a;
            bool has_addr = selected_address(*dp, s, &b, &a) != 0;
            if (has_addr && ImGui::MenuItem("Show XRefs", "X")) {
                s->request_xref_popup = true;
                s->xref_popup_bank = b;
                s->xref_popup_addr = a;
            }
            if (has_addr && ImGui::MenuItem("Add Bookmark", "B")) {
                char n[64];
                snprintf(n, 64, "Bookmark @ B%02x_%04x", b, a);
                s->bookmarks.push_back({b, a, n});
                s->request_focus_new_bookmark = 1;
                set_status(s, "bookmark added");
            }
        }
        ImGui::EndPopup();
    }

    /* Compact inspector strip */
    ImGui::Separator();
    if (s->hex_active && s->hex_selected_offset < p->rom.size) {
        size_t o = s->hex_selected_offset;
        uint8_t v = p->rom.data[o];
        uint8_t bank;
        uint32_t cpu_addr;
        char loc[24] = "-";
        if (rom_offset_to_cpu_address(p, o, &bank, &cpu_addr)) {
            snprintf(loc, sizeof(loc), "B%02x:A%04x", bank, cpu_addr);
        }
        char word_buf[20] = "";
        if (o + 1 < p->rom.size) {
            snprintf(word_buf, sizeof(word_buf), "  BE16:$%04X",
                     ((uint16_t)v << 8) | p->rom.data[o + 1]);
        }
        ImGui::Text("ROM:0x%06lx  %s  $%02X (%u) '%c'%s",
            (unsigned long)o, loc, v, v, (v >= 32 && v <= 126) ? (char)v : '.', word_buf);
    } else {
        ImGui::TextDisabled("Click a byte to inspect.");
    }
}

void render_call_graph(ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    if (s->graph_needs_rebuild) {
        rebuild_call_graph(p, d, s);
    }

    /* Pin/Unpin button — mirrors the References panel pattern. */
    {
        uint8_t cur_bank = 0;
        uint32_t cur_addr = 0;
        bool has_addr = selected_address(d, s, &cur_bank, &cur_addr);

        if (s->graph_pinned) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.35f, 0.05f, 1.0f));
            if (ImGui::SmallButton("Unpin")) {
                s->graph_pinned = false;
                s->graph_needs_rebuild = true;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            std::string lbl = label_at_address(d, s, s->graph_pinned_bank, s->graph_pinned_addr);
            char hdr[192];
            if (lbl.empty())
                snprintf(hdr, sizeof(hdr), "Pinned: B%02x_A%04x",
                         s->graph_pinned_bank, (unsigned)s->graph_pinned_addr & 0xffffu);
            else
                snprintf(hdr, sizeof(hdr), "Pinned: %s", lbl.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.70f, 0.20f, 1.0f));
            ImGui::TextUnformatted(hdr);
            ImGui::PopStyleColor();
        } else {
            if (has_addr) {
                if (ImGui::SmallButton("Pin")) {
                    s->graph_pinned = true;
                    s->graph_pinned_bank = cur_bank;
                    s->graph_pinned_addr = cur_addr;
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::SmallButton("Pin");
                ImGui::EndDisabled();
            }
        }
        ImGui::SameLine();
    }

    ImGui::SliderInt("In Depth", &s->graph_depth_in, 1, 4);
    ImGui::SameLine();
    ImGui::SliderInt("Out Depth", &s->graph_depth_out, 1, 4);
    if (ImGui::Button("Rebuild Graph")) {
        s->graph_needs_rebuild = true;
    }
    ImVec2 cp0 = ImGui::GetCursorScreenPos();
    ImVec2 csz = ImGui::GetContentRegionAvail();
    if (csz.x < 50) { csz.x = 50; }
    if (csz.y < 50) { csz.y = 50; }
    ImGui::InvisibleButton("canvas", csz);
    if (s->graph_nodes.empty()) {
        return;
    }
    std::map<int, std::vector<size_t>> lys;
    int min_l = 0, max_l = 0;
    for (size_t i = 0; i < s->graph_nodes.size(); i++) {
        int l = s->graph_nodes[i].layer;
        lys[l].push_back(i);
        if (l < min_l) { min_l = l; }
        if (l > max_l) { max_l = l; }
    }
    float lc = (float)(max_l - min_l + 1);
    float lw = csz.x / std::max(1.0f, lc);
    for (auto const &e : lys) {
        float x = cp0.x + (e.first - min_l + 0.5f) * lw;
        float ys = csz.y / std::max((size_t)1, e.second.size());
        for (size_t i = 0; i < e.second.size(); i++) {
            auto &n = s->graph_nodes[e.second[i]];
            n.pos  = ImVec2(x, cp0.y + (i + 0.5f) * ys);
            n.size = ImVec2(120, 30);
        }
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (auto &n : s->graph_nodes) {
        for (auto ci : n.callee_indices) {
            auto &c = s->graph_nodes[ci];
            ImVec2 p1(n.pos.x + n.size.x * 0.5f, n.pos.y);
            ImVec2 p2(c.pos.x - c.size.x * 0.5f, c.pos.y);
            dl->AddBezierCubic(p1, ImVec2(p1.x + 50, p1.y),
                               ImVec2(p2.x - 50, p2.y), p2,
                               IM_COL32(200, 200, 200, 150), 2.0f);
        }
    }
    for (size_t i = 0; i < s->graph_nodes.size(); i++) {
        auto &n = s->graph_nodes[i];
        ImVec2 pmin(n.pos.x - n.size.x * 0.5f, n.pos.y - n.size.y * 0.5f);
        ImVec2 pmax(n.pos.x + n.size.x * 0.5f, n.pos.y + n.size.y * 0.5f);
        bool hov = ImGui::IsMouseHoveringRect(pmin, pmax);
        dl->AddRectFilled(pmin, pmax,
            hov ? IM_COL32(100, 100, 150, 255) : IM_COL32(50, 50, 80, 255), 5.0f);
        dl->AddRect(pmin, pmax,
            (int)i == s->graph_root_idx ? IM_COL32(255, 255, 0, 255) : IM_COL32(200, 200, 200, 255),
            5.0f, 0, 2.0f);
        ImVec2 tsz = ImGui::CalcTextSize(n.name.c_str());
        dl->AddText(ImVec2(n.pos.x - std::min(tsz.x, n.size.x - 10) * 0.5f,
                           n.pos.y - tsz.y * 0.5f),
                    IM_COL32(255, 255, 255, 255), n.name.c_str());
        if (hov && ImGui::IsMouseClicked(0)) {
            size_t li;
            if (apex_render_find_line_by_address(d, n.bank, n.addr, &li)) {
                select_line(s, li, 1);
                s->graph_needs_rebuild = true;
            }
        }
        if (hov)
            ImGui::SetTooltip("B%02x_A%04x\nClick to navigate",
                              n.bank, (unsigned)n.addr & 0xffff);
    }
}

/* Renders a row of field-kind buttons and named-type buttons for the field builder.
   Clicking a button appends a field to `fields`/`count` if there is room.
   Returns true if any button was clicked. */
static bool render_field_buttons(ApexProject *p, ApexEditField *fields, int *count,
                                 int add_count, int *sprite_height, bool allow_variable = false)
{
    /* Row 1: primitive and 16-bit pointer kinds */
    static const struct { int kind; const char *label; } kRow1[] = {
        { TABLE_BYTE,               "byte"        },
        { TABLE_WORD,               "word"        },
        { TABLE_PTR16_STRING,       "ptr16_string"},
        { TABLE_PTR16_DATA,         "ptr16_data"  },
        { TABLE_PTR16_CODE,         "ptr16_code"  },
        { TABLE_PTR16_DMD_FULLFRAME,"ptr16_dmd"   },
        { TABLE_PTR16_SPRITE,      "ptr16_spr"   },
    };
    /* Row 2: far pointer kinds */
    static const struct { int kind; const char *label; } kRow2[] = {
        { TABLE_FAR_STRING,        "far_string"  },
        { TABLE_FAR_DATA,          "far_data"    },
        { TABLE_FAR_TABLE,         "far_table"   },
        { TABLE_FAR_CODE,          "far_code"    },
        { TABLE_FAR_DMD_FULLFRAME, "far_dmd"     },
        { TABLE_FAR_SPRITE,        "far_spr"     },
    };

    bool changed = false;
    auto push_kind = [&](int kind, const char *label) {
        if (ImGui::SmallButton(label)) {
            if (*count < APEX_MAX_EDIT_FIELDS) {
                ApexEditField f = {};
                f.kind  = kind;
                f.count = add_count > 0 ? add_count : 1;
                if ((kind == TABLE_PTR16_SPRITE || kind == TABLE_FAR_SPRITE) &&
                    sprite_height && *sprite_height > 0) {
                    f.param = *sprite_height;
                }
                fields[(*count)++] = f;
                changed = true;
            }
        }
    };

    for (int i = 0; i < (int)(sizeof(kRow1)/sizeof(kRow1[0])); i++) {
        if (i > 0) ImGui::SameLine();
        push_kind(kRow1[i].kind, kRow1[i].label);
    }
    for (int i = 0; i < (int)(sizeof(kRow2)/sizeof(kRow2[0])); i++) {
        if (i > 0) ImGui::SameLine();
        push_kind(kRow2[i].kind, kRow2[i].label);
    }
    /* Variable-length kinds are inline-only (a table needs a fixed row width). */
    if (allow_variable) {
        push_kind(TABLE_BYTES_UNTIL, "bytes_until(0x00)");
        ImGui::SameLine();
        push_kind(TABLE_COUNTED_BYTES, "counted_bytes");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextUnformatted(
                "Variable-length inline payloads. bytes_until uses a 0x00 terminator; for a "
                "different terminator edit the config line to bytes_until(0xNN). counted_bytes "
                "reads a leading count byte, then that many bytes.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    /* No-header sprite image height applied to ptr16_spr / far_spr fields. */
    if (sprite_height) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(46.0f);
        ImGui::InputInt("sprH", sprite_height, 0, 0);
        if (*sprite_height < 0) *sprite_height = 0;
        if (*sprite_height > 32) *sprite_height = 32;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("no-header sprite image height (px) for ptr16_spr / far_spr fields");
    }
    /* named types from config — combo with search filter */
    if (p && p->config_types.count > 0) {
        static char cust_filter[64] = {};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##cust_type", "Custom type...",
                              ImGuiComboFlags_HeightLarge)) {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere();
                cust_filter[0] = '\0';
            }
            ImGui::InputTextWithHint("##cust_filter", "search...",
                                     cust_filter, sizeof(cust_filter));
            ImGui::Separator();
            for (size_t ti = 0; ti < p->config_types.count; ti++) {
                const ConfigType *ct = &p->config_types.items[ti];
                if (cust_filter[0] &&
                    !strstr(ct->name, cust_filter)) {
                    continue;
                }
                if (ImGui::Selectable(ct->name)) {
                    if (*count < APEX_MAX_EDIT_FIELDS) {
                        ApexEditField f = {};
                        f.kind  = -1;
                        f.count = add_count > 0 ? add_count : 1;
                        snprintf(f.type_name, sizeof(f.type_name), "%s", ct->name);
                        fields[(*count)++] = f;
                        changed = true;
                    }
                    cust_filter[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered() && ct->value_count > 0) {
                    ImGui::BeginTooltip();
                    for (size_t vi = 0; vi < ct->value_count; vi++) {
                        ImGui::Text(ct->kind == TABLE_WORD ? "0x%04x = %s" : "0x%02x = %s",
                                    ct->values[vi].value, ct->values[vi].name);
                    }
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndCombo();
        }
    }
    return changed;
}

/* Renders the visual chip list for a field array with a Clear-all button. */
static void render_field_chips(ApexEditField *fields, int *count)
{
    for (int i = 0; i < *count; i++) {
        char chip[96];
        const char *kname = (fields[i].kind >= 0) ? fields[i].type_name : fields[i].type_name;
        if (fields[i].kind >= 0) {
            /* look up name from kind */
            static const struct { int kind; const char *name; } kN[] = {
                { TABLE_BYTE,              "byte"          },
                { TABLE_WORD,              "word"          },
                { TABLE_PTR16_STRING,      "ptr16_string"  },
                { TABLE_PTR16_DATA,        "ptr16_data"    },
                { TABLE_PTR16_CODE,        "ptr16_code"    },
                { TABLE_PTR16_TABLE,       "ptr16_table"   },
                { TABLE_PTR16_DMD_FULLFRAME,"ptr16_dmd"    },
                { TABLE_PTR16_SPRITE,      "ptr16_spr"    },
                { TABLE_FAR_STRING,        "far_string"    },
                { TABLE_FAR_DATA,          "far_data"      },
                { TABLE_FAR_TABLE,         "far_table"     },
                { TABLE_FAR_CODE,          "far_code"      },
                { TABLE_FAR_DMD_FULLFRAME, "far_dmd"       },
                { TABLE_FAR_SPRITE,        "far_spr"       },
                { TABLE_BYTES_UNTIL,       "bytes_until"   },
                { TABLE_COUNTED_BYTES,     "counted_bytes" },
            };
            kname = "?";
            for (int k = 0; k < (int)(sizeof(kN)/sizeof(kN[0])); k++) {
                if (kN[k].kind == fields[i].kind) { kname = kN[k].name; break; }
            }
        }
        char hbuf[16] = "";
        if (fields[i].param > 0 &&
            (fields[i].kind == TABLE_PTR16_SPRITE || fields[i].kind == TABLE_FAR_SPRITE)) {
            snprintf(hbuf, sizeof(hbuf), "(%d)", fields[i].param);
        }
        if (fields[i].kind == TABLE_BYTES_UNTIL) {
            snprintf(hbuf, sizeof(hbuf), "(0x%02x)", fields[i].param & 0xff);
        }
        if (fields[i].count > 1) {
            snprintf(chip, sizeof(chip), "%s%s[%d]##chip%d", kname, hbuf, fields[i].count, i);
        } else {
            snprintf(chip, sizeof(chip), "%s%s##chip%d", kname, hbuf, i);
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.70f, 1.0f));
        if (ImGui::SmallButton(chip)) {
            /* click chip to remove it */
            for (int j = i; j < *count - 1; j++) fields[j] = fields[j+1];
            (*count)--;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("click to remove");
        ImGui::SameLine();
    }
    if (*count > 0) {
        if (ImGui::SmallButton("X##clrfields")) *count = 0;
    }
    if (*count == 0) {
        ImGui::TextDisabled("(no fields)");
    }
}

void render_editor(ApexProject *p, const ApexRenderedDocument **dp,
                   const OriginalSnapshot *sn, UiState *s)
{
    uint8_t b;
    uint32_t a;
    /* When the hex view was the last view interacted with, the editor targets
       the exact selected byte (matching the Classify As / Label actions below),
       not the start of the corresponding disassembly line. */
    if (s->hex_is_edit_target && s->hex_selected_offset < p->rom.size) {
        if (!rom_offset_to_cpu_address(p, s->hex_selected_offset, &b, &a)) {
            ImGui::TextUnformatted("No addressable byte selected.");
            return;
        }
    } else if (!selected_address(*dp, s, &b, &a)) {
        ImGui::TextUnformatted("No addressable line selected.");
        return;
    }

    /* ── Label ──────────────────────────────────────────────────── */
    ImGui::SeparatorText("Label");
    ImGui::Text("B%02x_A%04x", b, (unsigned)a & 0xffffu);
    if (s->request_focus_label) {
        ImGui::SetKeyboardFocusHere();
        s->request_focus_label = 0;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##label", s->edit_label_input, 128);
    if (ImGui::Button("Apply##lbl")) {
        if (s->edit_label_input[0] == 0) {
            set_status(s, "empty");
        } else if (apex_project_set_label(p, 1, b, a, s->edit_label_input) == 0) {
            rerender_and_reselect(p, dp, s, b, a);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##lbl")) {
        if (apex_project_clear_label(p, 1, b, a) == 0) {
            rerender_and_reselect(p, dp, s, b, a);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto-Label Targets")) {
        auto_label_targets(p, dp, s);
    }

    /* ── Classify As ────────────────────────────────────────────── */
    ImGui::SeparatorText("Classify As");
    /* row: special kinds */
    if (ImGui::Button("Code##cls"))   { apply_code_at_selection(p, dp, s); }
    ImGui::SameLine();
    if (ImGui::Button("String##cls"))   { apply_string_at_selection(p, dp, s); }
    ImGui::SameLine();
    if (ImGui::Button("Clear##cls"))    { clear_kind_at_selection(p, dp, s); }
    /* row: raw byte/word + bytes[N] */
    ImGui::TextDisabled("raw:");
    ImGui::SameLine();
    if (ImGui::Button("byte##data"))  { apply_data_at_selection(p, dp, s, "bytes[1]"); }
    ImGui::SameLine();
    if (ImGui::Button("word##data"))  { apply_data_at_selection(p, dp, s, "bytes[2]"); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputInt("##datalen", &s->edit_data_length, 0, 0)) {
        if (s->edit_data_length < 1) s->edit_data_length = 1;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("N for bytes[N]");
    ImGui::SameLine();
    if (ImGui::Button("bytes[N]##data")) {
        char spec[32];
        snprintf(spec, sizeof(spec), "bytes[%d]", s->edit_data_length);
        apply_data_at_selection(p, dp, s, spec);
    }
    ImGui::SameLine();
    if (ImGui::Button("bcd[N]##data")) {
        char spec[32];
        snprintf(spec, sizeof(spec), "bcd[%d]", s->edit_data_length);
        apply_data_at_selection(p, dp, s, spec);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("N-byte binary-coded decimal");
    ImGui::SameLine();
    if (ImGui::Button("sprite##raw")) { apply_data_at_selection(p, dp, s, "sprite"); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(42.0f);
    ImGui::InputInt("##snhh", &s->sprite_nh_height, 0, 0);
    if (s->sprite_nh_height < 1)  s->sprite_nh_height = 1;
    if (s->sprite_nh_height > 128) s->sprite_nh_height = 128;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("height for sprite_noheader");
    ImGui::SameLine();
    if (ImGui::Button("spr_nh##raw")) {
        char spec[32];
        snprintf(spec, sizeof(spec), "sprite_noheader[%d]", s->sprite_nh_height);
        apply_data_at_selection(p, dp, s, spec);
    }
    /* row: 16-bit pointer types */
    ImGui::TextDisabled("ptr16:");
    ImGui::SameLine();
    if (ImGui::Button("code##p16"))   { apply_data_at_selection(p, dp, s, "ptr16_code");   }
    ImGui::SameLine();
    if (ImGui::Button("data##p16"))   { apply_data_at_selection(p, dp, s, "ptr16_data");   }
    ImGui::SameLine();
    if (ImGui::Button("string##p16")) { apply_data_at_selection(p, dp, s, "ptr16_string"); }
    ImGui::SameLine();
    if (ImGui::Button("table##p16"))  { apply_data_at_selection(p, dp, s, "ptr16_table");  }
    ImGui::SameLine();
    if (ImGui::Button("spr##p16"))    { apply_data_at_selection(p, dp, s, "ptr16_sprite"); }
    /* row: far pointer types */
    ImGui::TextDisabled("far:  ");
    ImGui::SameLine();
    if (ImGui::Button("code##far"))    { apply_data_at_selection(p, dp, s, "far_code");    }
    ImGui::SameLine();
    if (ImGui::Button("data##far"))    { apply_data_at_selection(p, dp, s, "far_data");    }
    ImGui::SameLine();
    if (ImGui::Button("string##far"))  { apply_data_at_selection(p, dp, s, "far_string");  }
    ImGui::SameLine();
    if (ImGui::Button("table##far"))   { apply_data_at_selection(p, dp, s, "far_table");   }
    ImGui::SameLine();
    if (ImGui::Button("dmd##far"))     { apply_data_at_selection(p, dp, s, "dmd_fullframe"); }
    ImGui::SameLine();
    if (ImGui::Button("far_spr##far")) { apply_data_at_selection(p, dp, s, "far_sprite");  }

    /* ── Inline ─────────────────────────────────────────────────── */
    ImGui::SeparatorText("Inline Signature");
    ImGui::TextDisabled("N:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(36);
    ImGui::InputInt("##inln", &s->edit_field_add_count, 0, 0);
    if (s->edit_field_add_count < 1) s->edit_field_add_count = 1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("repeat count for next added field");
    ImGui::PushID("inl");
    render_field_buttons(p, s->edit_inline_fields, &s->edit_inline_count,
                         s->edit_field_add_count, NULL, /*allow_variable=*/true);
    render_field_chips(s->edit_inline_fields, &s->edit_inline_count);
    ImGui::PopID();
    ImGui::Checkbox("flow_stop (tail-call: never returns)", &s->edit_inline_flow_stop);
    if (ImGui::Button("Apply##inl")) {
        if (s->edit_inline_count > 0) {
            char spec[256];
            fields_to_spec(spec, sizeof(spec), s->edit_inline_fields, s->edit_inline_count);
            if (s->edit_inline_flow_stop) {
                size_t n = strlen(spec);
                snprintf(spec + n, sizeof(spec) - n, ", flow_stop");
            }
            if (apex_project_set_inline(p, 1, b, a, spec) == 0) {
                rerender_and_reselect(p, dp, s, b, a);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##inl")) {
        if (apex_project_clear_inline(p, 1, b, a) == 0) {
            rerender_and_reselect(p, dp, s, b, a);
        }
    }

    /* ── Table ──────────────────────────────────────────────────── */
    ImGui::SeparatorText("Table");
    int is_rows = s->edit_table_is_rows;
    if (ImGui::RadioButton("counted", &is_rows, 0)) s->edit_table_is_rows = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("rows", &is_rows, 1))    s->edit_table_is_rows = 1;
    if (s->edit_table_is_rows) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputInt("##tblrows", &s->edit_table_rows, 1, 8)) {
            if (s->edit_table_rows < 1) s->edit_table_rows = 1;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("row count");
    }
    ImGui::TextDisabled("N:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(36);
    ImGui::InputInt("##tbln", &s->edit_field_add_count, 0, 0);
    if (s->edit_field_add_count < 1) s->edit_field_add_count = 1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("repeat count for next added field");
    ImGui::PushID("tbl");
    render_field_buttons(p, s->edit_schema_fields, &s->edit_schema_count,
                         s->edit_field_add_count, &s->sprite_nh_height);
    render_field_chips(s->edit_schema_fields, &s->edit_schema_count);
    ImGui::PopID();
    if (ImGui::Button("Apply##tbl")) {
        if (s->edit_schema_count > 0) {
            char schema[256];
            fields_to_spec(schema, sizeof(schema), s->edit_schema_fields, s->edit_schema_count);
            char spec[320];
            if (s->edit_table_is_rows) {
                snprintf(spec, sizeof(spec), "rows[%d](%s)", s->edit_table_rows, schema);
            } else {
                snprintf(spec, sizeof(spec), "counted(%s)", schema);
            }
            apply_table_at_selection(p, dp, s, spec);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##tbl")) {
        clear_kind_at_selection(p, dp, s);
    }

    /* ── Doc ────────────────────────────────────────────────────── */
    ImGui::SeparatorText("Doc");
    if (s->request_focus_doc) {
        ImGui::SetKeyboardFocusHere();
        s->request_focus_doc = 0;
    }
    ImGui::InputTextMultiline("##doc", s->edit_doc_input, 1024,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5),
                              ImGuiInputTextFlags_WordWrap);
    if (ImGui::Button("Apply##doc")) {
        if (s->edit_doc_input[0]) {
            if (apex_project_set_doc(p, 1, b, a, s->edit_doc_input) == 0) {
                rerender_and_reselect(p, dp, s, b, a);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##doc")) {
        if (apex_project_clear_doc(p, 1, b, a) == 0) {
            rerender_and_reselect(p, dp, s, b, a);
        }
    }

    /* ── Save ───────────────────────────────────────────────────── */
    ImGui::SeparatorText("Save");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##overlay", s->save_path_input, 512);
    if (ImGui::Button("Save Overlay") || s->request_save_overlay) {
        s->request_save_overlay = 0;
        std::string st;
        int rc = write_delta_overlay(p, sn, s->save_path_input, s->base_config_path, &st);
        if (rc > 0) {
            set_status(s, st.c_str());
            s->overlay_dirty = false;
        } else if (rc == 0) {
            if (apex_project_save_overlay(p, s->save_path_input, s->base_config_path) == 0) {
                set_status(s, "saved full");
                s->overlay_dirty = false;
            }
        } else {
            set_status(s, "failed");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Session")) {
        clear_session();
        set_status(s, "cleared");
    }
    if (s->status_message[0]) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", s->status_message);
    }
}

void render_dmd_view(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    auto pr = find_dmd_preview(p, d, s);
    ImGui::PushItemWidth(-1);
    ImGui::SliderInt("##scrub", &s->dmd_scrub_offset, -2048, 2048, "Scrub: %d");
    ImGui::PopItemWidth();
    if (ImGui::Button("Reset")) {
        s->dmd_scrub_offset = 0;
    }
    ImGui::SameLine();
    if (pr.valid && s->dmd_scrub_offset != 0 && ImGui::Button("Mark DMD")) {
        uint8_t b;
        uint32_t a;
        if (selected_address(d, s, &b, &a)) {
            uint32_t ta = (uint32_t)((int64_t)a + s->dmd_scrub_offset);
            if (apex_project_set_kind((ApexProject*)p, 1, b, ta, APEX_KIND_DATA,
                                      "dmd_fullframe") == 0) {
                rerender_and_reselect((ApexProject*)p, (const ApexRenderedDocument**)&d, s, b, ta);
                s->dmd_scrub_offset = 0;
            }
        }
    }
    ImGui::Separator();
    if (pr.valid) {
        render_dmd_preview(pr);
    } else {
        ImGui::TextDisabled("None.");
    }
}

void render_hardware_window(ApexProject *project, const ApexRenderedDocument *document,
                            UiState *state)
{
    static std::vector<HardwareAccess> accesses;
    static bool needs_refresh = true;

    if (ImGui::Button("Refresh Scan") || needs_refresh) {
        accesses = find_hardware_accesses(project, document);
        needs_refresh = false;
    }

    ImGui::Separator();

    if (ImGui::BeginTable("hardware_mapping", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Address",     ImGuiTableColumnFlags_WidthFixed,   80.0f);
        ImGui::TableSetupColumn("Name",        ImGuiTableColumnFlags_WidthFixed,  150.0f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Usage",       ImGuiTableColumnFlags_WidthFixed,   80.0f);
        ImGui::TableHeadersRow();

        for (auto &acc : accesses) {
            ImGui::PushID(acc.reg->addr);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("$%04X", acc.reg->addr);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(acc.reg->name);

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(acc.reg->desc);

            ImGui::TableSetColumnIndex(3);
            char usage_lbl[32];
            snprintf(usage_lbl, 32, "%lu refs##link", (unsigned long)acc.line_indices.size());
            if (acc.line_indices.empty()) {
                ImGui::TextDisabled("not found");
            } else {
                if (ImGui::TreeNode(usage_lbl)) {
                    for (size_t lidx : acc.line_indices) {
                        const auto *line = &document->lines[lidx];
                        char loc_lbl[64];
                        if (line->has_location) {
                            snprintf(loc_lbl, 64, "B%02x:A%04x", line->bank, line->cpu_addr);
                        } else {
                            snprintf(loc_lbl, 64, "Line %lu", (unsigned long)lidx);
                        }
                        if (ImGui::Selectable(loc_lbl)) {
                            select_line(state, lidx, 1);
                        }
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static bool valid_identifier(const char *s);

/* Turn arbitrary text into a valid enum identifier: keep [A-Za-z0-9_], collapse
   other runs to a single '_', trim, prefix '_' if it would start with a digit.
   Returns "" if nothing usable remains. */
static std::string sanitize_enum_name(const std::string &in)
{
    std::string out;
    bool last_us = false;
    for (char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out += c;
            last_us = false;
        } else if (!out.empty() && !last_us) {
            out += '_';
            last_us = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (!out.empty() && out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
    return out;
}

/* If `t` is a text table (rows of ptr16_string or far_string), read each row's
   string into `out` (empty string where a row can't be resolved).  Returns false
   if it is not a string table or its bytes can't be located. */
static bool text_table_row_strings(const ApexProject *p, const TableDef *t,
                                   std::vector<std::string> &out)
{
    out.clear();
    if (t->schema.count == 0) return false;
    TableFieldKind k = t->schema.items[0].kind;
    bool is_far = (k == TABLE_FAR_STRING);
    if (k != TABLE_PTR16_STRING && !is_far) return false;

    const uint8_t *tsrc; size_t trem;
    if (!project_locate_rom_bytes(p, t->bank, t->addr, &tsrc, &trem, NULL)) return false;

    size_t header = 0, count;
    if (t->has_header) {
        if (trem < 3u) return false;
        count = ((size_t)tsrc[0] << 8) | tsrc[1];
        header = 3u;
    } else {
        count = t->rows;
    }
    size_t row_width = table_schema_width(&t->schema);
    size_t ptr_len = is_far ? 3u : 2u;
    if (row_width == 0) return false;

    for (size_t r = 0; r < count; r++) {
        size_t off = header + r * row_width;
        std::string sval;
        if (off + ptr_len <= trem) {
            uint16_t addr = (uint16_t)(((uint16_t)tsrc[off] << 8) | tsrc[off + 1]);
            uint8_t bank = is_far ? tsrc[off + 2] : (addr >= 0x8000u ? 0xffu : t->bank);
            const uint8_t *ssrc; size_t srem;
            if (project_locate_rom_bytes(p, bank, addr, &ssrc, &srem, NULL)) {
                size_t slen = valid_string_len(ssrc, srem);
                sval.assign((const char *)ssrc, slen);
            }
        }
        out.push_back(std::move(sval));
    }
    return true;
}

void render_tables_window(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    /* ---- Suggestion/accept flow: scan proposes, the user accepts (like sprites) ---- */
    if (ImGui::Button("Scan for Tables")) {
        scan_table_candidates(p, s);
    }
    if (s->table_scan_done) {
        size_t pending = 0;
        for (auto &c : s->table_candidates) if (!c.already) pending++;
        ImGui::SameLine();
        ImGui::Text("%zu candidate(s), %zu new", s->table_candidates.size(), pending);
        if (pending > 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Accept all new")) {
                /* snapshot the specs first — applying rerenders and could move state */
                std::vector<std::tuple<uint8_t,uint32_t,std::string>> todo;
                for (auto &c : s->table_candidates)
                    if (!c.already) todo.emplace_back(c.bank, c.cpu_addr, c.spec);
                int done = 0;
                for (auto &t : todo) {
                    if (apex_project_set_kind(p, 1, std::get<0>(t), std::get<1>(t),
                                              APEX_KIND_TABLE, std::get<2>(t).c_str()) == 0)
                        done++;
                }
                if (done) rerender_and_reselect(p, dp, s, 0xffu, 0u);
                scan_table_candidates(p, s); /* refresh 'already' flags */
                set_status(s, (std::to_string(done) + " table(s) accepted").c_str());
            }
        }
    }
    if (!s->table_candidates.empty()) {
        if (ImGui::BeginTable("tbl_cands", 5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                ImVec2(0, 140.0f))) {
            ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Rows", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Spec", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();
            int accept_idx = -1;
            for (size_t ci = 0; ci < s->table_candidates.size(); ci++) {
                auto &c = s->table_candidates[ci];
                ImGui::PushID((int)ci);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char ab[24]; snprintf(ab, sizeof(ab), "B%02x_A%04x", c.bank, c.cpu_addr);
                if (ImGui::Selectable(ab, false, ImGuiSelectableFlags_SpanAllColumns |
                                                 ImGuiSelectableFlags_AllowOverlap)) {
                    size_t li;
                    if (apex_render_find_line_by_address(*dp, c.bank, c.cpu_addr, &li))
                        select_line(s, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(c.kind.c_str());
                ImGui::TableSetColumnIndex(2);
                if (c.rows) ImGui::Text("%d", c.rows); else ImGui::TextDisabled("hdr");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(c.spec.c_str());
                ImGui::TableSetColumnIndex(4);
                if (c.already) {
                    ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "in table");
                } else if (ImGui::SmallButton("Accept")) {
                    accept_idx = (int)ci;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (accept_idx >= 0) {
                auto &c = s->table_candidates[accept_idx];
                if (apex_project_set_kind(p, 1, c.bank, c.cpu_addr, APEX_KIND_TABLE,
                                          c.spec.c_str()) == 0) {
                    uint8_t b = c.bank; uint32_t a = c.cpu_addr;
                    rerender_and_reselect(p, dp, s, b, a);
                    scan_table_candidates(p, s); /* refresh 'already' flags */
                    set_status(s, "Table accepted");
                }
            }
        }
    }
    ImGui::Separator();
    if (ImGui::BeginTable("tables_list", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Addr",    ImGuiTableColumnFlags_WidthFixed,  100.0f, 0);
        ImGui::TableSetupColumn("Setup",   ImGuiTableColumnFlags_WidthFixed,  200.0f, 1);
        ImGui::TableSetupColumn("Rows",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                50.0f, 2);
        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                                0.0f, 3);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                80.0f, 4);
        ImGui::TableHeadersRow();

        std::vector<size_t> order(p->tables.count);
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        {
            int sort_col; bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc)) {
                std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib) {
                    const TableDef *a = &p->tables.items[ia];
                    const TableDef *b = &p->tables.items[ib];
                    int c = sort_col == 1
                        ? table_def_spec_string(a).compare(table_def_spec_string(b))
                        : ui_cmp_u32(((uint32_t)a->bank<<16)|(a->addr&0xffffu),
                                     ((uint32_t)b->bank<<16)|(b->addr&0xffffu));
                    return sort_asc ? c < 0 : c > 0;
                });
            }
        }

        for (size_t oi = 0; oi < order.size(); oi++) {
            size_t i = order[oi];
            const auto *t = &p->tables.items[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            char a[32];
            snprintf(a, 32, "B%02x_A%04x", t->bank, t->addr);
            if (ImGui::Selectable(a, false)) {
                /* single click → go to the address in the disassembly */
                size_t li;
                const uint8_t *src; size_t len, off;
                if (apex_render_find_line_by_address(*dp, t->bank, t->addr, &li))
                    select_line(s, li, 1);
                else if (project_locate_rom_bytes(p, t->bank, t->addr, &src, &len, &off) &&
                         find_line_by_rom_offset(*dp, off, &li))
                    select_line(s, li, 1);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("go to this address in the disassembly");

            ImGui::TableSetColumnIndex(1);
            char spec_buf[128];
            strncpy(spec_buf, table_def_spec_string(t).c_str(), 127);
            spec_buf[127] = '\0';
            ImGui::PushItemWidth(-FLT_MIN);
            if (ImGui::InputText("##spec", spec_buf, 128, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (apex_project_set_kind(p, 1, t->bank, t->addr, APEX_KIND_TABLE, spec_buf) == 0) {
                    rerender_and_reselect(p, dp, s, t->bank, t->addr);
                    set_status(s, "Table setup updated");
                }
            }
            ImGui::PopItemWidth();

            /* Rows: counted tables store their length as a leading BE16 in ROM;
               fixed tables carry it in the spec. */
            ImGui::TableSetColumnIndex(2);
            if (t->has_header) {
                const uint8_t *tsrc; size_t trem;
                if (project_locate_rom_bytes(p, t->bank, t->addr, &tsrc, &trem, NULL) &&
                    trem >= 2u) {
                    unsigned cnt = ((unsigned)tsrc[0] << 8) | tsrc[1];
                    ImGui::Text("%u", cnt);
                } else {
                    ImGui::TextDisabled("?");
                }
            } else {
                ImGui::Text("%lu", (unsigned long)t->rows);
            }

            ImGui::TableSetColumnIndex(3);
            const char *existing_doc = config_doc_at(&p->docs, t->bank, t->addr);
            char doc_buf[512] = "";
            if (existing_doc) {
                strncpy(doc_buf, existing_doc, 511);
                doc_buf[511] = '\0';
            }
            ImGui::PushItemWidth(-FLT_MIN);
            if (ImGui::InputText("##doc", doc_buf, 512, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (apex_project_set_doc(p, 1, t->bank, t->addr, doc_buf) == 0) {
                    rerender_and_reselect(p, dp, s, t->bank, t->addr);
                    set_status(s, "Table comment updated");
                }
            }
            ImGui::PopItemWidth();

            ImGui::TableSetColumnIndex(4);
            bool is_text = t->schema.count > 0 &&
                           (t->schema.items[0].kind == TABLE_PTR16_STRING ||
                            t->schema.items[0].kind == TABLE_FAR_STRING);
            if (is_text) {
                if (ImGui::SmallButton("Type")) {
                    s->tt_type_bank = t->bank;
                    s->tt_type_addr = t->addr;
                    s->tt_type_word = 0;
                    /* default name: type_<table label>, else type_B##_A#### */
                    const char *lbl = NULL;
                    for (size_t li = 0; li < p->config_labels.count; li++) {
                        const ConfigLabel *cl = &p->config_labels.items[li];
                        if (cl->addr == t->addr && (!cl->has_bank || cl->bank == t->bank)) {
                            lbl = cl->name; break;
                        }
                    }
                    if (lbl) snprintf(s->tt_type_name, sizeof(s->tt_type_name), "type_%s", lbl);
                    else snprintf(s->tt_type_name, sizeof(s->tt_type_name),
                                  "type_B%02x_A%04x", t->bank, t->addr);
                    s->tt_type_request = true;
                }
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Del")) {
                uint8_t  del_bank = t->bank;
                uint32_t del_addr = t->addr;
                apex_project_clear_kind(p, 1, del_bank, del_addr);
                rerender_and_reselect(p, dp, s, del_bank, del_addr);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    /* ---- Create Type from a text table's strings ---- */
    if (s->tt_type_request) {
        ImGui::OpenPopup("mk_type_from_table");
        s->tt_type_request = false;
    }
    if (ImGui::BeginPopup("mk_type_from_table")) {
        const TableDef *tt = table_def_at(s->tt_type_bank, s->tt_type_addr, &p->tables);
        ImGui::TextUnformatted("Create enum type from table strings");
        ImGui::Separator();
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("Type name", s->tt_type_name, sizeof(s->tt_type_name));
        ImGui::RadioButton("byte", &s->tt_type_word, 0); ImGui::SameLine();
        ImGui::RadioButton("word", &s->tt_type_word, 1);

        std::vector<std::string> strings;
        bool have = tt && text_table_row_strings(p, tt, strings);
        int usable = 0;
        if (have)
            for (auto &str : strings)
                if (!sanitize_enum_name(str).empty()) usable++;

        if (!have) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Not a resolvable text table.");
        } else {
            ImGui::TextDisabled("%d value(s) from %zu row(s). Preview:",
                                usable, strings.size());
            ImGui::BeginChild("ttprev", ImVec2(360, 120), true);
            for (size_t r = 0; r < strings.size(); r++) {
                std::string nm = sanitize_enum_name(strings[r]);
                if (nm.empty()) continue;
                ImGui::Text(s->tt_type_word ? "0x%04zx : %s" : "0x%02zx : %s", r, nm.c_str());
            }
            ImGui::EndChild();
        }

        bool name_ok = valid_identifier(s->tt_type_name);
        ImGui::BeginDisabled(!have || usable == 0 || !name_ok);
        if (ImGui::Button("Create")) {
            std::string vs;
            char buf[24];
            for (size_t r = 0; r < strings.size(); r++) {
                std::string nm = sanitize_enum_name(strings[r]);
                if (nm.empty()) continue;
                snprintf(buf, sizeof(buf), s->tt_type_word ? "0x%04zx:" : "0x%02zx:", r);
                if (!vs.empty()) vs += ", ";
                vs += buf; vs += nm;
            }
            if (apex_project_set_type(p, s->tt_type_name, s->tt_type_word, vs.c_str()) == 0) {
                s->overlay_dirty = true;
                set_status(s, (std::string("Created type ") + s->tt_type_name +
                               " (" + std::to_string(usable) + " values)").c_str());
                ImGui::CloseCurrentPopup();
            } else {
                set_status(s, "Type creation failed (name in use?)");
            }
        }
        ImGui::EndDisabled();
        if (!name_ok && s->tt_type_name[0]) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "invalid name");
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* Build a values_str like "0x00:name, 0x01:other" from a ConfigType, excluding index `skip`.
   If skip < 0 append the new_val/new_name pair. */
static std::string build_values_str(const ConfigType *ct, int skip,
                                    uint32_t new_val, const char *new_name)
{
    std::string out;
    char tmp[64];
    const char *vfmt = (ct->kind == TABLE_WORD) ? "0x%04x:%s" : "0x%02x:%s";
    for (size_t i = 0; i < ct->value_count; i++) {
        if ((int)i == skip) continue;
        if (!out.empty()) out += ", ";
        snprintf(tmp, sizeof(tmp), vfmt, ct->values[i].value, ct->values[i].name);
        out += tmp;
    }
    if (new_name && *new_name) {
        if (!out.empty()) out += ", ";
        snprintf(tmp, sizeof(tmp), vfmt, new_val, new_name);
        out += tmp;
    }
    return out;
}

static bool valid_identifier(const char *s)
{
    if (!s || !*s) return false;
    if (!std::isalpha((unsigned char)*s) && *s != '_') return false;
    for (const char *p = s + 1; *p; p++) {
        if (!std::isalnum((unsigned char)*p) && *p != '_') return false;
    }
    return true;
}

/* Find lines in the rendered document whose text contains the given word (symbol name).
   Returns a list of line indices. */
static std::vector<size_t> find_symbol_usages(const ApexRenderedDocument *d, const char *name)
{
    std::vector<size_t> out;
    if (!d || !name || !*name) return out;
    size_t nlen = strlen(name);
    for (size_t i = 0; i < d->line_count; i++) {
        const ApexRenderedLine *l = &d->lines[i];
        if (!l->has_location || l->kind == APEX_RENDER_LINE_COMMENT) continue;
        /* search for name as a word (bounded by non-identifier chars) */
        const char *p2 = l->text;
        size_t rem = (size_t)l->length;
        while (rem >= nlen) {
            const char *hit = (const char *)memchr(p2, (unsigned char)name[0], rem - nlen + 1);
            if (!hit) break;
            size_t off = (size_t)(hit - l->text);
            if (memcmp(hit, name, nlen) == 0) {
                /* check word boundaries */
                bool lb = (off == 0 || (!isalnum((unsigned char)hit[-1]) && hit[-1] != '_'));
                bool rb = (off + nlen >= (size_t)l->length ||
                           (!isalnum((unsigned char)hit[nlen]) && hit[nlen] != '_'));
                if (lb && rb) { out.push_back(i); break; }
            }
            size_t skip = (size_t)(hit - p2) + 1;
            p2  += skip;
            rem -= skip;
        }
    }
    return out;
}

void render_symbols_editor(ApexProject *p, const ApexRenderedDocument *document, UiState *s)
{
    /* ---- Analysis options (opt-in; persisted to the .apexgui.ini overlay) ---- */
    if (ImGui::CollapsingHeader("Analysis options")) {
        ConfigOptions *op = &p->options;
        bool changed = false;
        bool b;

        b = op->min_immediate_symbol != 0;
        if (ImGui::Checkbox("Suppress small symbols in immediates (< 0x200)", &b)) {
            op->min_immediate_symbol = b ? 0x200u : 0u;
            changed = true;
        }
        b = op->reference_counts != 0;
        if (ImGui::Checkbox("Reference counts at labels", &b)) { op->reference_counts = b; changed = true; }
        b = op->hex_index_offsets != 0;
        if (ImGui::Checkbox("Hex index offsets", &b)) { op->hex_index_offsets = b; changed = true; }
        b = op->instruction_addresses != 0;
        if (ImGui::Checkbox("Instruction address comments", &b)) { op->instruction_addresses = b; changed = true; }
        b = op->far_code_allow_null != 0;
        if (ImGui::Checkbox("far_code tolerates null (0x0000)", &b)) { op->far_code_allow_null = b; changed = true; }
        b = op->report_code_in_data != 0;
        if (ImGui::Checkbox("Report code in [data] ranges", &b)) { op->report_code_in_data = b; changed = true; }
        b = op->check_inline_length != 0;
        if (ImGui::Checkbox("Check inline length vs stack fixup", &b)) { op->check_inline_length = b; changed = true; }
        b = op->report_dmd_short != 0;
        if (ImGui::Checkbox("Report short DMD frames (missing bit-plane)", &b)) { op->report_dmd_short = b; changed = true; }

        if (changed) {
            s->overlay_dirty = true;
            apex_project_invalidate(p, APEX_DIRTY_RENDER);
        }
    }

    /* ---- Add / Edit form ---- */
    bool name_ok = config_valid_symbol_name(s->sym_edit_name) != 0;
    unsigned long parsed_val = 0;
    bool val_ok = false;
    {
        char *ep = NULL;
        if (s->sym_edit_value[0]) {
            parsed_val = strtoul(s->sym_edit_value, &ep, 0);
            val_ok = (ep && *ep == '\0' && parsed_val <= 0xffffu);
        }
    }

    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##sym_name", "NAME", s->sym_edit_name, sizeof(s->sym_edit_name));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputTextWithHint("##sym_val", "0x0000", s->sym_edit_value, sizeof(s->sym_edit_value));
    ImGui::SameLine();

    bool can_submit = name_ok && val_ok;
    if (!can_submit) ImGui::BeginDisabled();
    bool is_update = false;
    for (size_t i = 0; i < p->symbols.count; i++) {
        if (strcmp(p->symbols.items[i].name, s->sym_edit_name) == 0) { is_update = true; break; }
    }
    if (ImGui::Button(is_update ? "Update##sym" : "Add##sym")) {
        if (apex_project_set_symbol(p, s->sym_edit_name, (uint32_t)parsed_val) == 0) {
            s->overlay_dirty = true;
            s->labels_valid  = false;
        }
    }
    if (!can_submit) ImGui::EndDisabled();

    if (!name_ok && s->sym_edit_name[0])
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "invalid name");
    else if (!val_ok && s->sym_edit_value[0])
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "value must be 0x0000..0xffff");

    ImGui::Separator();

    /* ---- Symbol list ---- */
    if (ImGui::BeginTable("sym_list", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS,
            ImVec2(0, p->symbols.count > 0 ? 200.0f : 60.0f))) {
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 70.0f, 1);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                60.0f, 2);
        ImGui::TableHeadersRow();

        std::vector<size_t> order(p->symbols.count);
        for (size_t k = 0; k < order.size(); k++) order[k] = k;
        {
            int sort_col; bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc)) {
                std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib) {
                    const ConfigSymbol *a = &p->symbols.items[ia];
                    const ConfigSymbol *b = &p->symbols.items[ib];
                    int c = sort_col == 1 ? ui_cmp_u32(a->value, b->value)
                                          : strcmp(a->name, b->name);
                    return sort_asc ? c < 0 : c > 0;
                });
            }
        }

        bool deleted = false;
        for (size_t oi = 0; oi < order.size() && !deleted; oi++) {
            size_t i = order[oi];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            bool selected = (s->sym_selected == (int)i);

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(p->symbols.items[i].name, selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                s->sym_selected = (int)i;
                snprintf(s->sym_edit_name, sizeof(s->sym_edit_name), "%s",
                         p->symbols.items[i].name);
                snprintf(s->sym_edit_value, sizeof(s->sym_edit_value), "0x%04x",
                         p->symbols.items[i].value);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%04x", p->symbols.items[i].value);

            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
            if (ImGui::SmallButton("Del")) {
                char del_name[64];
                snprintf(del_name, sizeof(del_name), "%s", p->symbols.items[i].name);
                if (s->sym_selected == (int)i) s->sym_selected = -1;
                apex_project_clear_symbol(p, del_name);
                s->overlay_dirty = true;
                s->labels_valid  = false;
                deleted = true;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    /* ---- Usages of selected symbol (cached — recompute only on selection/doc change) ---- */
    if (s->sym_selected >= 0 && (size_t)s->sym_selected < p->symbols.count && document) {
        if (s->sym_usages_sel != s->sym_selected || s->sym_usages_doc != document) {
            const char *selname = p->symbols.items[s->sym_selected].name;
            s->sym_usages_cache = find_symbol_usages(document, selname);
            s->sym_usages_sel   = s->sym_selected;
            s->sym_usages_doc   = document;
        }
        const auto &usages = s->sym_usages_cache;
        ImGui::SeparatorText("Usages in disassembly");
        if (usages.empty()) {
            ImGui::TextDisabled("none found");
        } else {
            float list_h = std::min((float)usages.size() * ImGui::GetFrameHeightWithSpacing() + 4.0f,
                                    120.0f);
            ImGui::BeginChild("sym_usages", ImVec2(0, list_h), false);
            for (size_t i = 0; i < usages.size() && usages[i] < document->line_count; i++) {
                const ApexRenderedLine *l = &document->lines[usages[i]];
                char lbuf[160];
                snprintf(lbuf, sizeof(lbuf), "B%02x_A%04x  %.*s",
                         l->bank, (unsigned)l->cpu_addr & 0xffff,
                         std::min((int)l->length, 100), l->text);
                ImGui::PushID((int)(0xf0000000u ^ (unsigned)i));
                if (ImGui::SmallButton(lbuf))
                    select_line(s, usages[i], 1);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    }
}

void render_types_editor(ApexProject *p, UiState *s)
{
    static char new_type_name[64] = "";
    static int  new_type_word     = 0;   /* 0=byte, 1=word */
    /* per-type "add value" form state, keyed by selected type index */
    static int  sel_type          = -1;
    static char new_val_hex[16]   = "";
    static char new_val_name[64]  = "";

    /* ── Add new type ─────────────────────────────────────────── */
    ImGui::SeparatorText("New Type");
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("Name##nt", new_type_name, sizeof(new_type_name));
    ImGui::SameLine();
    ImGui::RadioButton("byte##nt", &new_type_word, 0); ImGui::SameLine();
    ImGui::RadioButton("word##nt", &new_type_word, 1); ImGui::SameLine();
    bool name_ok = valid_identifier(new_type_name) &&
                   !find_config_type(&p->config_types, new_type_name);
    ImGui::BeginDisabled(!name_ok);
    if (ImGui::Button("Add Type")) {
        apex_project_set_type(p, new_type_name, new_type_word, "");
        s->overlay_dirty = true;
        new_type_name[0] = '\0';
    }
    ImGui::EndDisabled();
    if (!name_ok && new_type_name[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled(find_config_type(&p->config_types, new_type_name)
                            ? "(exists)" : "(invalid)");
    }

    /* ── Type list ────────────────────────────────────────────── */
    ImGui::SeparatorText("Types");
    if (p->config_types.count == 0) {
        ImGui::TextDisabled("No types defined.");
        return;
    }

    for (size_t ti = 0; ti < p->config_types.count; ti++) {
        ConfigType *ct = &p->config_types.items[ti];
        ImGui::PushID((int)ti);

        /* header line: "TypeName : byte/word  [X]" */
        bool open = ImGui::TreeNodeEx(ct->name,
                        ImGuiTreeNodeFlags_DefaultOpen |
                        ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::SameLine();
        ImGui::TextDisabled(":%s", ct->kind == TABLE_WORD ? "word" : "byte");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
        if (ImGui::SmallButton("X##deltype")) {
            apex_project_remove_type(p, ct->name);
            s->overlay_dirty = true;
            if (sel_type == (int)ti) sel_type = -1;
            ImGui::PopStyleColor();
            if (open) ImGui::TreePop();
            ImGui::PopID();
            break; /* array mutated, stop iterating */
        }
        ImGui::PopStyleColor();

        if (open) {
            /* existing enum values */
            for (size_t vi = 0; vi < ct->value_count; vi++) {
                ImGui::PushID((int)vi);
                ImGui::Text(ct->kind == TABLE_WORD ? "  0x%04x = %s" : "  0x%02x = %s",
                            ct->values[vi].value, ct->values[vi].name);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
                if (ImGui::SmallButton("x##delval")) {
                    std::string vs = build_values_str(ct, (int)vi, 0, nullptr);
                    apex_project_set_type(p, ct->name,
                                         ct->kind == TABLE_WORD, vs.c_str());
                    s->overlay_dirty = true;
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    break;
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }

            /* add value form */
            if (sel_type != (int)ti) {
                if (ImGui::SmallButton("+ Add value")) {
                    sel_type      = (int)ti;
                    new_val_hex[0] = '\0';
                    new_val_name[0] = '\0';
                }
            } else {
                ImGui::SetNextItemWidth(60);
                ImGui::InputText("hex##av", new_val_hex, sizeof(new_val_hex));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::InputText("name##av", new_val_name, sizeof(new_val_name));
                ImGui::SameLine();
                uint32_t parsed_val = 0;
                bool hex_ok = new_val_hex[0] &&
                              sscanf(new_val_hex, "%i", (int *)&parsed_val) == 1;
                bool val_name_ok = valid_identifier(new_val_name);
                ImGui::BeginDisabled(!hex_ok || !val_name_ok);
                if (ImGui::SmallButton("Add##av")) {
                    std::string vs = build_values_str(ct, -1, parsed_val, new_val_name);
                    apex_project_set_type(p, ct->name,
                                         ct->kind == TABLE_WORD, vs.c_str());
                    s->overlay_dirty = true;
                    sel_type = -1;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel##av")) sel_type = -1;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void render_inline_list(ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    static char filter[128] = "";
    ImGui::InputText("Filter##inllist", filter, sizeof(filter));
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", p->inline_sigs.count);

    if (ImGui::BeginTable("inlinesigs", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 110.0f, 0);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                130.0f, 1);
        ImGui::TableSetupColumn("Inline",  ImGuiTableColumnFlags_WidthStretch, 0.0f, 2);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        /* build a filtered list first */
        std::vector<size_t> rows;
        for (size_t i = 0; i < p->inline_sigs.count; i++) {
            const InlineSignature *sig = &p->inline_sigs.items[i];
            uint8_t bank = sig->has_bank ? sig->bank : 0xffu;
            uint32_t addr = sig->addr;
            if (filter[0]) {
                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x", bank, (unsigned)addr & 0xffffu);
                std::string spec = inline_sig_spec_string(sig);
                std::string lbl  = label_at_address(d, s, bank, addr);
                bool match = str_icontains(addrstr, filter) ||
                             str_icontains(spec.c_str(), filter) ||
                             (!lbl.empty() && str_icontains(lbl.c_str(), filter));
                if (!match) continue;
            }
            rows.push_back(i);
        }
        {
            int sort_col; bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc)) {
                std::stable_sort(rows.begin(), rows.end(), [&](size_t ia, size_t ib) {
                    const InlineSignature *a = &p->inline_sigs.items[ia];
                    const InlineSignature *b = &p->inline_sigs.items[ib];
                    int c;
                    if (sort_col == 2) {
                        c = inline_sig_spec_string(a).compare(inline_sig_spec_string(b));
                    } else {
                        uint8_t ab = a->has_bank ? a->bank : 0xffu;
                        uint8_t bb = b->has_bank ? b->bank : 0xffu;
                        c = ui_cmp_u32(((uint32_t)ab<<16)|(a->addr&0xffffu),
                                       ((uint32_t)bb<<16)|(b->addr&0xffffu));
                    }
                    return sort_asc ? c < 0 : c > 0;
                });
            }
        }

        clipper.Begin((int)rows.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const InlineSignature *sig = &p->inline_sigs.items[rows[row]];
                uint8_t bank = sig->has_bank ? sig->bank : 0xffu;
                uint32_t addr = sig->addr;
                std::string spec = inline_sig_spec_string(sig);
                std::string lbl  = label_at_address(d, s, bank, addr);

                size_t li = 0;
                bool found = apex_render_find_line_by_address(d, bank, addr, &li) != NULL;

                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x", bank, (unsigned)addr & 0xffffu);

                ImGui::PushID((int)rows[row]);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool sel = found && s->selected_line == li;
                if (ImGui::Selectable(addrstr, sel,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (found) {
                        select_line(s, li, ImGui::IsMouseDoubleClicked(0) ? 1 : 0);
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(lbl.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(spec.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void render_entries_list(ApexProject *p, const ApexRenderedDocument **document_ptr, UiState *s)
{
    const ApexRenderedDocument *d = *document_ptr;
    static char filter[128] = "";
    /* Kind filter: 0 = all, 1 = necessary only ("entry"), 2 = redundant only
       ("entry~", i.e. already reached by code flow). */
    static int kind_filter = 0;
    ImGui::InputText("Filter##entlist", filter, sizeof(filter));
    ImGui::RadioButton("All", &kind_filter, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Necessary", &kind_filter, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Redundant", &kind_filter, 2);

    /* Collect all redundant entries (reached by flow ⇒ the explicit [entries]
       line is unnecessary) so we can offer a one-click bulk cleanup. */
    std::vector<std::pair<uint8_t, uint32_t>> redundant;
    for (size_t i = 0; i < p->config_entries.count; i++) {
        const ConfigEntry *e = &p->config_entries.items[i];
        uint8_t bank = e->has_bank ? e->bank : 0xffu;
        const Label *el = find_explicit_entry_label(p, bank, e->addr);
        if (el && el->reached_by_flow)
            redundant.emplace_back(bank, e->addr);
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(redundant.empty());
    if (ImGui::Button("Delete redundant"))
        ImGui::OpenPopup("del_redundant");
    ImGui::EndDisabled();

    if (ImGui::BeginPopupModal("del_redundant", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %zu redundant entr%s?", redundant.size(),
                    redundant.size() == 1 ? "y" : "ies");
        ImGui::TextDisabled("These are still reached by code-flow analysis,\n"
                            "so the disassembly is unchanged.");
        ImGui::Separator();
        if (ImGui::Button("Delete")) {
            /* Preserve the current selection (deleting redundant entries leaves the
               disassembly unchanged) so the view doesn't jump. */
            uint8_t keep_bank = 0; uint32_t keep_addr = 0;
            selected_address(d, s, &keep_bank, &keep_addr);
            apex_project_begin_edit_group(p, "delete redundant");
            for (auto &r : redundant)
                apex_project_clear_kind(p, r.first != 0xffu, r.first, r.second);
            apex_project_end_edit_group(p);
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            /* clear_kind frees and rebuilds the document on the next render; do it
               now and bail so the rest of this frame doesn't touch the stale `d`. */
            rerender_and_reselect(p, document_ptr, s, keep_bank, keep_addr);
            set_status(s, "deleted redundant entries");
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    /* Collect the visible rows (text + kind filter) before drawing the table so
       the match count can sit next to the filter controls. */
    std::vector<size_t> rows;
    for (size_t i = 0; i < p->config_entries.count; i++) {
        const ConfigEntry *e = &p->config_entries.items[i];
        uint8_t bank = e->has_bank ? e->bank : 0xffu;
        uint32_t addr = e->addr;
        if (kind_filter != 0) {
            const Label *el = find_explicit_entry_label(p, bank, addr);
            bool redundant = el && el->reached_by_flow;
            if (kind_filter == 1 && redundant) continue;   /* want necessary only */
            if (kind_filter == 2 && !redundant) continue;  /* want redundant only */
        }
        if (filter[0]) {
            char addrstr[32];
            snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x", bank, (unsigned)addr & 0xffffu);
            std::string lbl = label_at_address(d, s, bank, addr);
            bool match = str_icontains(addrstr, filter) ||
                         (!lbl.empty() && str_icontains(lbl.c_str(), filter));
            if (!match) continue;
        }
        rows.push_back(i);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu / %zu)", rows.size(), p->config_entries.count);

    if (ImGui::BeginTable("entries", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 110.0f, 0);
        ImGui::TableSetupColumn("Kind",    ImGuiTableColumnFlags_WidthFixed,  55.0f, 1);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                                0.0f, 2);
        ImGui::TableHeadersRow();
        {
            int sort_col; bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc)) {
                std::stable_sort(rows.begin(), rows.end(), [&](size_t ia, size_t ib) {
                    const ConfigEntry *a = &p->config_entries.items[ia];
                    const ConfigEntry *b = &p->config_entries.items[ib];
                    uint8_t ab = a->has_bank ? a->bank : 0xffu;
                    uint8_t bb = b->has_bank ? b->bank : 0xffu;
                    int c;
                    if (sort_col == 1) {
                        const Label *la = find_explicit_entry_label(p, ab, a->addr);
                        const Label *lb = find_explicit_entry_label(p, bb, b->addr);
                        int ra = (la && la->reached_by_flow) ? 1 : 0;
                        int rb = (lb && lb->reached_by_flow) ? 1 : 0;
                        c = ui_cmp_int(ra, rb);
                    } else {
                        c = ui_cmp_u32(((uint32_t)ab<<16)|(a->addr&0xffffu),
                                       ((uint32_t)bb<<16)|(b->addr&0xffffu));
                    }
                    return sort_asc ? c < 0 : c > 0;
                });
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)rows.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const ConfigEntry *e = &p->config_entries.items[rows[row]];
                uint8_t bank = e->has_bank ? e->bank : 0xffu;
                uint32_t addr = e->addr;
                std::string lbl = label_at_address(d, s, bank, addr);

                size_t li = 0;
                bool found = apex_render_find_line_by_address(d, bank, addr, &li) != NULL;

                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x", bank, (unsigned)addr & 0xffffu);

                ImGui::PushID((int)rows[row]);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool sel = found && s->selected_line == li;
                if (ImGui::Selectable(addrstr, sel,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    if (found) {
                        select_line(s, li, 1);
                    }
                }
                /* Kind: "entry" (green) when this explicit entry is necessary, or
                   "entry~" (grey) when code flow already reaches it, so the entry
                   is redundant — same distinction the disassembly draws. */
                ImGui::TableSetColumnIndex(1);
                const Label *el = find_explicit_entry_label(p, bank, addr);
                if (el && el->reached_by_flow)
                    ImGui::TextDisabled("entry~");
                else if (el)
                    ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.35f, 1.0f), "entry");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(lbl.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

/* Classify a rendered directive line as a string and return its type name, or
   NULL if it is not a STRING/STRING_FIXED directive. */
static const char *string_directive_type(const ApexRenderedLine *l)
{
    /* STRING* directives render as indented lines, which classify_line() tags as
       APEX_RENDER_LINE_INSTRUCTION (DIRECTIVE is reserved for unindented lines
       like .ORG); accept either and rely on the text prefix below. */
    if (l->kind != APEX_RENDER_LINE_INSTRUCTION &&
        l->kind != APEX_RENDER_LINE_DIRECTIVE)
        return NULL;
    size_t i = 0;
    while (i < l->length && (l->text[i] == ' ' || l->text[i] == '\t'))
        i++;
    const char *p = l->text + i;
    size_t rem = l->length - i;
    #define STR_STARTS(kw) (rem >= sizeof(kw) - 1 && memcmp(p, kw, sizeof(kw) - 1) == 0)
    if (STR_STARTS("STRING_FIXED")) return "string_fixed";
    if (STR_STARTS("STRING"))       return "string";
    #undef STR_STARTS
    return NULL;
}

/* Extract the quoted content of a STRING directive line (escapes kept as-is). */
static std::string string_directive_content(const ApexRenderedLine *l)
{
    size_t a = 0;
    while (a < l->length && l->text[a] != '"')
        a++;
    if (a >= l->length)
        return std::string();
    size_t b = l->length;
    while (b > a + 1 && l->text[b - 1] != '"')
        b--;
    if (b <= a + 1)
        return std::string();
    return std::string(l->text + a + 1, (b - 1) - (a + 1));
}

void render_strings_list(ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    ImGui::InputText("Filter##strlist", s->strings_filter_input,
                     sizeof(s->strings_filter_input));
    const char *filter = s->strings_filter_input;

    /* Collect string directive lines, applying the case-insensitive filter to
       content / label / address. The O(line_count) scan is cached on
       (generation, filter); only the small matched set is materialised per frame. */
    struct StrRow { size_t line_idx; std::string content; const char *type; };
    if (s->cached_strings_gen != d->generation || s->cached_strings_filter != filter) {
        s->cached_strings_rows.clear();
        for (size_t i = 0; i < d->line_count; i++) {
            const ApexRenderedLine *l = &d->lines[i];
            const char *type = string_directive_type(l);
            if (!type || !l->has_location)
                continue;
            if (filter[0]) {
                std::string content = string_directive_content(l);
                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x", l->bank,
                         (unsigned)l->cpu_addr & 0xffffu);
                std::string lbl = label_at_address(d, s, l->bank, l->cpu_addr);
                bool match = str_icontains(content.c_str(), filter) ||
                             str_icontains(addrstr, filter) ||
                             (!lbl.empty() && str_icontains(lbl.c_str(), filter));
                if (!match)
                    continue;
            }
            s->cached_strings_rows.push_back(i);
        }
        s->cached_strings_gen = d->generation;
        s->cached_strings_filter = filter;
    }
    std::vector<StrRow> rows;
    rows.reserve(s->cached_strings_rows.size());
    for (size_t idx : s->cached_strings_rows) {
        const ApexRenderedLine *l = &d->lines[idx];
        rows.push_back({idx, string_directive_content(l), string_directive_type(l)});
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", rows.size());

    if (ImGui::BeginTable("strings", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 110.0f, 0);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                160.0f, 1);
        ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthFixed,  85.0f, 2);
        ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch, 0.0f, 3);
        ImGui::TableSetupColumn("Refs",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                60.0f, 4);
        ImGui::TableHeadersRow();

        int sort_col; bool sort_asc;
        if (ui_table_sort(&sort_col, &sort_asc)) {
            std::stable_sort(rows.begin(), rows.end(),
                [&](const StrRow &a, const StrRow &b) {
                    const ApexRenderedLine *la = &d->lines[a.line_idx];
                    const ApexRenderedLine *lb = &d->lines[b.line_idx];
                    int c = 0;
                    switch (sort_col) {
                    case 0: c = ui_cmp_u32(((uint32_t)la->bank<<16)|(la->cpu_addr&0xffffu),
                                           ((uint32_t)lb->bank<<16)|(lb->cpu_addr&0xffffu)); break;
                    case 2: c = strcmp(a.type, b.type); break;
                    case 3: c = a.content.compare(b.content); break;
                    }
                    return sort_asc ? c < 0 : c > 0;
                });
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)rows.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const StrRow &sr = rows[(size_t)row];
                const ApexRenderedLine *l = &d->lines[sr.line_idx];
                std::string lbl = label_at_address(d, s, l->bank, l->cpu_addr);
                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x", l->bank,
                         (unsigned)l->cpu_addr & 0xffffu);

                ImGui::PushID((int)sr.line_idx);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                /* Address: click jumps the disassembly to the string. */
                if (ImGui::Selectable(addrstr, s->selected_line == sr.line_idx,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    select_line(s, sr.line_idx, 1);

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(lbl.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", sr.type);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(sr.content.c_str());

                /* Refs: count of incoming references; button opens the xref popup
                   (which lists every reference and jumps to it). */
                ImGui::TableSetColumnIndex(4);
                auto refs = find_incoming_refs(p, d, s, l->bank, l->cpu_addr);
                if (!refs.empty()) {
                    char rbuf[24];
                    snprintf(rbuf, sizeof(rbuf), "%zu refs", refs.size());
                    if (ImGui::SmallButton(rbuf)) {
                        s->request_xref_popup = true;
                        s->xref_popup_bank    = l->bank;
                        s->xref_popup_addr    = l->cpu_addr;
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void render_rom_map(ApexProject *p, const ApexRenderedDocument **document_ptr, UiState *s)
{
    const ApexRenderedDocument *d = *document_ptr;
    if (!d || p->rom.size == 0) {
        ImGui::TextDisabled("No ROM loaded.");
        return;
    }

    static const ImVec4 kind_colors[] = {
        ImVec4(0.50f, 0.50f, 0.50f, 1.0f), /* [0]  UNKNOWN       — gray       */
        ImVec4(0.40f, 0.90f, 0.40f, 1.0f), /* [1]  CODE          — green      */
        ImVec4(0.45f, 0.70f, 1.00f, 1.0f), /* [2]  DATA (.DB)    — blue       */
        ImVec4(0.95f, 0.65f, 0.20f, 1.0f), /* [3]  TABLE         — orange     */
        ImVec4(0.65f, 0.65f, 0.65f, 1.0f), /* [4]  UNCLASSIFIED  — light gray */
        ImVec4(0.55f, 0.35f, 0.10f, 1.0f), /* [5]  FREE (0xFF)   — dark amber */
        ImVec4(0.47f, 0.86f, 1.00f, 1.0f), /* [6]  SPRITE        — sky blue   */
        ImVec4(0.90f, 0.55f, 0.90f, 1.0f), /* [7]  STRING        — purple     */
        ImVec4(0.30f, 0.90f, 0.90f, 1.0f), /* [8]  .DW           — cyan       */
        ImVec4(1.00f, 0.40f, 0.35f, 1.0f), /* [9]  FAR pointer   — red        */
        ImVec4(1.00f, 0.30f, 0.70f, 1.0f), /* [10] DMD fullframe — magenta    */
    };
    static const int kind_colors_count = (int)(sizeof(kind_colors) / sizeof(kind_colors[0]));

    static GLuint rom_map_tex = 0;
    static const ApexRenderedDocument *last_doc = nullptr;
    static int tex_w = 0, tex_h = 0;

    const int map_w = 512;

    /* Rebuild texture whenever the document changes. */
    if (d != last_doc) {
        last_doc = d;
        size_t rom_size = p->rom.size;
        int h = (int)((rom_size + (size_t)(map_w - 1)) / (size_t)map_w);
        tex_w = map_w;
        tex_h = h;

        /* Per-byte extended kind array, shared with the Hex view (cached on the
           document generation) — same classifier, one implementation. */
        ensure_hex_kind_map(p, d, s);
        const std::vector<uint8_t> &kinds = s->cached_hex_kinds;

        /* Convert kind array to RGBA pixels. */
        std::vector<uint8_t> pixels((size_t)tex_w * (size_t)tex_h * 4, 0);
        for (size_t i = 0; i < rom_size; i++) {
            int ki = kinds[i];
            if (ki < 0 || ki >= kind_colors_count) ki = 0;
            const ImVec4 &c = kind_colors[ki];
            size_t px = i * 4;
            pixels[px + 0] = (uint8_t)(c.x * 255.0f);
            pixels[px + 1] = (uint8_t)(c.y * 255.0f);
            pixels[px + 2] = (uint8_t)(c.z * 255.0f);
            pixels[px + 3] = 255;
        }

        if (rom_map_tex == 0)
            glGenTextures(1, &rom_map_tex);
        glBindTexture(GL_TEXTURE_2D, rom_map_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (rom_map_tex == 0 || tex_w == 0 || tex_h == 0) return;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float display_w = (float)map_w;
    float display_h = (float)tex_h * (display_w / (float)tex_w);
    if (display_h > avail.y) display_h = avail.y;
    ImVec2 img_size(display_w, display_h);
    ImVec2 img_origin = ImGui::GetCursorScreenPos();

    /* Helper: convert ROM offset → screen Y within the image. */
    auto offset_to_screen_y = [&](size_t off) -> float {
        int row = (int)(off / (size_t)map_w);
        return img_origin.y + ((float)row / (float)tex_h) * display_h;
    };
    /* Helper: convert ROM offset → screen X within the image. */
    auto offset_to_screen_x = [&](size_t off) -> float {
        int col = (int)(off % (size_t)map_w);
        return img_origin.x + ((float)col / (float)map_w) * display_w;
    };

    ImGui::Image((ImTextureID)(intptr_t)rom_map_tex, img_size);
    bool hovered = ImGui::IsItemHovered();

    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* --- Bank boundary lines --- */
    {
        size_t sys_start = p->rom.size > 32768u ? p->rom.size - 32768u : 0u;
        /* Thin white lines at each 16 KB paged bank boundary. */
        for (size_t off = 16384u; off < sys_start; off += 16384u) {
            float y = offset_to_screen_y(off);
            dl->AddLine(ImVec2(img_origin.x, y),
                        ImVec2(img_origin.x + display_w, y),
                        IM_COL32(255, 255, 255, 50), 1.0f);
        }
        /* Brighter line at the system bank boundary. */
        if (sys_start > 0 && sys_start < p->rom.size) {
            float y = offset_to_screen_y(sys_start);
            dl->AddLine(ImVec2(img_origin.x, y),
                        ImVec2(img_origin.x + display_w, y),
                        IM_COL32(255, 210, 80, 180), 1.5f);
        }
    }

    /* --- Crosshair: mirror current disasm/hex cursor position --- */
    {
        /* Prefer hex cursor when hex is active and has a valid selection. */
        size_t cursor_offset = SIZE_MAX;
        if (s->hex_active && s->hex_selected_offset < p->rom.size) {
            cursor_offset = s->hex_selected_offset;
        } else if (s->selected_line < d->line_count &&
                   d->lines[s->selected_line].has_location) {
            cursor_offset = d->lines[s->selected_line].rom_addr;
        }

        if (cursor_offset < p->rom.size) {
            float cy = offset_to_screen_y(cursor_offset);
            float cx = offset_to_screen_x(cursor_offset);
            /* Horizontal line across the full width. */
            dl->AddLine(ImVec2(img_origin.x,              cy),
                        ImVec2(img_origin.x + display_w,  cy),
                        IM_COL32(255, 255, 255, 210), 1.0f);
            /* Vertical line for column position. */
            dl->AddLine(ImVec2(cx, img_origin.y),
                        ImVec2(cx, img_origin.y + display_h),
                        IM_COL32(255, 255, 255, 100), 1.0f);
        }
    }

    /* --- Click to navigate --- */
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        float rx = (mouse.x - img_origin.x) / display_w;
        float ry = (mouse.y - img_origin.y) / display_h;
        size_t rom_offset = (size_t)(ry * (float)tex_h) * (size_t)map_w +
                            (size_t)(rx * (float)map_w);
        if (rom_offset < p->rom.size) {
            /* Find the nearest located line whose rom_addr <= clicked offset.
               This handles clicks inside blocks, not just at label entry points. */
            size_t best_li = SIZE_MAX;
            size_t best_addr = 0;
            for (size_t li = 0; li < d->line_count; li++) {
                const ApexRenderedLine *l = &d->lines[li];
                if (!l->has_location || l->rom_addr > rom_offset) continue;
                if (best_li == SIZE_MAX || l->rom_addr >= best_addr) {
                    best_li = li;
                    best_addr = l->rom_addr;
                }
            }
            if (best_li != SIZE_MAX) {
                select_line(s, best_li, 1);
                s->show_disasm = true;
            }
        }
    }

    /* --- Hover tooltip --- */
    if (hovered) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        float rx = (mouse.x - img_origin.x) / display_w;
        float ry = (mouse.y - img_origin.y) / display_h;
        size_t rom_offset = (size_t)(ry * (float)tex_h) * (size_t)map_w +
                            (size_t)(rx * (float)map_w);
        if (rom_offset < p->rom.size) {
            uint8_t bank; uint32_t cpu_addr;
            char tip[64];
            if (rom_offset_to_cpu_address(p, rom_offset, &bank, &cpu_addr)) {
                if (bank == 0xff)
                    snprintf(tip, sizeof(tip), "Bff_A%04x (0x%06lx)",
                             (unsigned)cpu_addr, (unsigned long)rom_offset);
                else
                    snprintf(tip, sizeof(tip), "B%02x_A%04x (0x%06lx)",
                             (unsigned)bank, (unsigned)cpu_addr, (unsigned long)rom_offset);
            } else {
                snprintf(tip, sizeof(tip), "0x%06lx", (unsigned long)rom_offset);
            }
            ImGui::SetTooltip("%s", tip);
        }
    }
}

