#include "apeximgui_core.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <cctype>
#include <cfloat>
#include <strings.h>

// --- UI Rendering Helpers (Internal) ---

static bool str_icontains(const char *hay, const char *needle)
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
    static const ImVec4 label_color  = ImVec4(0.95f, 0.82f, 0.45f, 1.0f);
    static const ImVec4 target_color = ImVec4(0.45f, 0.80f, 0.95f, 1.0f);
    if (line->kind == APEX_RENDER_LINE_LABEL) {
        render_text_chunk(line->text, line->text + line->length, &label_color);
        return;
    }
    auto targets = find_line_targets(document, state, line);
    if (targets.empty()) {
        render_text_chunk(line->text, line->text + line->length, NULL);
        return;
    }
    const char *cursor = line->text;
    for (auto &target : targets) {
        const char *m_start = line->text + target.match_pos;
        const char *m_end   = m_start + target.name.size();
        if (m_start < cursor) {
            continue;
        }
        if (cursor < m_start) {
            render_text_chunk(cursor, m_start, NULL);
            ImGui::SameLine(0, 0);
        }
        render_text_chunk(m_start, m_end, &target_color);
        cursor = m_end;
        if (cursor < line->text + line->length) {
            ImGui::SameLine(0, 0);
        }
    }
    if (cursor < line->text + line->length) {
        render_text_chunk(cursor, line->text + line->length, NULL);
    }
}

static void render_dmd_preview(const DmdPreviewInfo &preview, float max_scale = 6.0f)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale = std::max(1.0f, std::min(max_scale, avail.x / (float)APEX_DMD_WIDTH));
    ImGui::TextUnformatted(preview.title);
    ImGui::Text("Address: B%02x_A%04x  ROM: 0x%06lx",
                preview.bank, (unsigned)preview.cpu_addr & 0xffffu,
                (unsigned long)preview.rom_offset);
    ImGui::Text("Decoder: 0x%02x  Consumed: %lu  Size: %ux%u",
                (unsigned)preview.decoder_type, (unsigned long)preview.consumed,
                (unsigned)APEX_DMD_WIDTH, (unsigned)APEX_DMD_HEIGHT);
    ImGui::Separator();
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("dmd_canvas", ImVec2(APEX_DMD_WIDTH * scale, APEX_DMD_HEIGHT * scale));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(canvas_pos,
                        ImVec2(canvas_pos.x + APEX_DMD_WIDTH * scale,
                               canvas_pos.y + APEX_DMD_HEIGHT * scale),
                        IM_COL32(8, 8, 8, 255));
    for (size_t row = 0; row < APEX_DMD_HEIGHT; row++) {
        for (size_t col_byte = 0; col_byte < APEX_DMD_ROW_BYTES; col_byte++) {
            uint8_t bits = preview.plane[row * APEX_DMD_ROW_BYTES + col_byte];
            for (size_t bit = 0; bit < 8u; bit++) {
                bool lit = ((bits >> bit) & 1u) != 0u;
                ImVec2 p0(canvas_pos.x + (col_byte * 8 + bit) * scale,
                          canvas_pos.y + row * scale);
                draw->AddRectFilled(p0,
                                    ImVec2(p0.x + scale - 1.0f, p0.y + scale - 1.0f),
                                    lit ? IM_COL32(255, 160, 40, 255) : IM_COL32(28, 18, 6, 255));
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

static bool is_dmd_fullframe_addr(const ApexProject *p, uint8_t bank, uint32_t addr)
{
    size_t i;
    for (i = 0; i < p->data_ranges.count; i++) {
        const DataRange *dr = &p->data_ranges.items[i];
        if (dr->kind == DATA_DMD_FULLFRAME &&
            dr->bank == bank &&
            addr >= dr->addr &&
            addr < dr->addr + APEX_DMD_PAGE_BYTES)
            return true;
    }
    return false;
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

static void render_sprite_preview(const SpritePreviewInfo &pr, float max_scale = 6.0f)
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
    for (uint8_t row = 0; row < pr.height; row++) {
        for (uint8_t col_byte = 0; col_byte < row_bytes; col_byte++) {
            uint8_t bits = pr.pixels[row * row_bytes + col_byte];
            for (size_t bit = 0; bit < 8u; bit++) {
                int px = (int)(col_byte * 8u + bit);
                if (px >= (int)pr.width) break;
                bool lit = ((bits >> bit) & 1u) != 0u; /* LSB = leftmost pixel, same as DMD renderer */
                ImVec2 p0(canvas_pos.x + px * scale, canvas_pos.y + row * scale);
                draw->AddRectFilled(p0,
                                    ImVec2(p0.x + scale - 1.0f, p0.y + scale - 1.0f),
                                    lit ? IM_COL32(120, 220, 255, 255) : IM_COL32(6, 18, 28, 255));
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

void render_line_table(ApexProject *project, const ApexRenderedDocument **document_ptr,
                       UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;
    std::vector<size_t> visible;
    int selected_visible_row = -1;
    ensure_label_index(document, state);
    visible.reserve(document->line_count);
    for (size_t i = 0; i < document->line_count; i++) {
        if (line_matches_filter(&document->lines[i], state->filter_input)) {
            if (state->selected_line == i) {
                selected_visible_row = (int)visible.size();
            }
            visible.push_back(i);
        }
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
        ImGui::TableSetupColumn("Text",  ImGuiTableColumnFlags_WidthStretch);
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
                const auto *line = &document->lines[line_idx];
                size_t sel_min = std::min(state->selected_line, state->selection_end);
                size_t sel_max = std::max(state->selected_line, state->selection_end);
                bool in_range = (line_idx >= sel_min && line_idx <= sel_max);
                ImGui::PushID((int)line_idx);
                ImGui::TableNextRow();
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
                /* Collect branch target for flow arrows */
                if (state->show_flow_arrows &&
                    line->kind == APEX_RENDER_LINE_INSTRUCTION && line->has_location &&
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
                if (line->transition_kind != APEX_RENDER_TRANSITION_NONE) {
                    ImGui::TextDisabled("%s", transition_name(line->transition_kind));
                    ImGui::SameLine();
                }
                ImGui::BeginGroup();
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
                /* Inline xref annotations for label lines */
                if (line->kind == APEX_RENDER_LINE_LABEL && line->has_location) {
                    auto in_refs = find_incoming_refs(project, document, state,
                                                     line->bank, line->cpu_addr);
                    static const int kMaxInline = 8;
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
                        size_t tl;
                        if (apex_render_find_line_by_address(document, t_bank, t_addr, &tl)) {
                            const ApexRenderedLine *tline = &document->lines[tl];
                            if (tline->block_kind == APEX_RENDER_BLOCK_CODE &&
                                tline->has_location &&
                                tline->rom_addr < project->rom.size) {
                                render_disasm_preview(project, tline->rom_addr, t_bank, t_addr);
                            }
                        }
                    }
                    /* Instruction: follow branch/call target, show code preview */
                    if (line->kind == APEX_RENDER_LINE_INSTRUCTION && line->has_location) {
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
                                size_t tl;
                                if (apex_render_find_line_by_address(document, tgt_bank, tgt_addr, &tl)) {
                                    const ApexRenderedLine *tline = &document->lines[tl];
                                    if (tline->block_kind == APEX_RENDER_BLOCK_CODE &&
                                        tline->has_location &&
                                        tline->rom_addr < project->rom.size) {
                                        render_disasm_preview(project, tline->rom_addr,
                                                              tgt_bank, tgt_addr);
                                    }
                                }
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
                    if (line->has_location &&
                        (line->block_kind == APEX_RENDER_BLOCK_DATA ||
                         line->block_kind == APEX_RENDER_BLOCK_SPRITE ||
                         line->block_kind == APEX_RENDER_BLOCK_TABLE ||
                         line->block_kind == APEX_RENDER_BLOCK_UNCLASSIFIED)) {
                        render_disasm_preview(project, line->rom_addr, line->bank, line->cpu_addr);
                    }
                    if (line->has_location) {
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
                        }
                        if (!found_dmd) {
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
                            /* Fallback: for ptr16_sprite FDB rows whose target isn't labeled
                               yet (unclassified), read the 2-byte pointer from ROM directly. */
                            if (!found_spr &&
                                line->has_location &&
                                line->block_kind == APEX_RENDER_BLOCK_TABLE &&
                                line->kind == APEX_RENDER_LINE_DIRECTIVE) {
                                const uint8_t *fdb_src; size_t fdb_rem;
                                if (project_locate_rom_bytes(project, line->bank,
                                                             line->cpu_addr, &fdb_src,
                                                             &fdb_rem, NULL) && fdb_rem >= 2u) {
                                    uint32_t tgt_addr = ((uint32_t)fdb_src[0] << 8) | fdb_src[1];
                                    uint8_t tgt_bank = (tgt_addr >= 0x8000u) ? 0xFFu : line->bank;
                                    if (decode_sprite_preview_at(project, tgt_bank,
                                                                 tgt_addr, &spr_pr)) {
                                        snprintf(spr_pr.title, sizeof(spr_pr.title), "Sprite Target");
                                        found_spr = true;
                                    }
                                }
                            }
                            if (found_spr) {
                                ImGui::Separator();
                                render_sprite_preview(spr_pr, 4.0f);
                            }
                        }
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
                uint8_t  pop_excl_bank = 0;
                uint32_t pop_excl_addr = 0;
                const bool pop_has_excl =
                    pop_has_loc && line_excluded_ref(project, line, &pop_excl_bank, &pop_excl_addr);

                if (ImGui::BeginPopup("row_context_menu")) {
                    if (has_pointer && ImGui::MenuItem("Jump to target", "F / Enter")) {
                        size_t tl;
                        if (apex_render_find_line_by_address(document, t_bank, t_addr, &tl)) {
                            select_line(state, tl, 1);
                        }
                    }
                    if (pop_has_loc && ImGui::MenuItem("Show incoming references", "X")) {
                        state->request_xref_popup = true;
                        state->xref_popup_bank = pop_bank;
                        state->xref_popup_addr = pop_cpu_addr;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy selection", "Ctrl+C")) {
                        copy_selection_to_clipboard(document, state);
                        set_status(state, "copied");
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
                    if (ImGui::MenuItem("Mark as String LP")) {
                        apply_string_lp_at_selection(project, document_ptr, state);
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
                    if (has_pointer && apex_render_find_line_by_address(document, t_bank, t_addr, &tl)) {
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
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,  120.0f);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed,   60.0f);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
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
            uint8_t bid = bank_id_for_index(p->rom.data, (int)i);
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
    for (size_t i = 0; i < d->line_count; i++) {
        const auto *l = &d->lines[i];
        if (l->transition_kind == APEX_RENDER_TRANSITION_NONE || !l->has_location ||
            !line_matches_filter(l, s->filter_input)) {
            continue;
        }
        char lbl[128];
        snprintf(lbl, 128, "%s @ B%02x_A%04x", transition_name(l->transition_kind),
                 l->bank, (unsigned)l->cpu_addr & 0xffffu);
        if (ImGui::Selectable(lbl, s->selected_line == i)) {
            select_line(s, i, 1);
        }
    }
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
                if (ImGui::Selectable(lbl, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    select_line(s, r.line_index, 1);
                    ImGui::CloseCurrentPopup();
                }
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
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Addr",    ImGuiTableColumnFlags_WidthFixed,  100.0f);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,   60.0f);
        ImGui::TableHeadersRow();
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
    if (ImGui::InputText("Query", s->global_search_input, 128)) {
        s->search_results.clear();
        if (s->global_search_input[0]) {
            for (size_t i = 0; i < d->line_count; i++) {
                if (line_matches_filter(&d->lines[i], s->global_search_input)) {
                    s->search_results.push_back(i);
                }
            }
        }
    }
    if (ImGui::BeginTable("search_results", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,  100.0f);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed,   60.0f);
        ImGui::TableSetupColumn("Text",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
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

/* Search forward from cur_off+1; wraps around to 0 if not found before end.
   Returns SIZE_MAX if the needle is not present anywhere. Sets *wrapped=true on wrap. */
static size_t hex_search_forward(const uint8_t *rom, size_t rom_size,
                                 size_t cur_off, const uint8_t *needle, size_t nlen,
                                 bool *wrapped)
{
    *wrapped = false;
    if (nlen == 0 || nlen > rom_size) return SIZE_MAX;
    size_t limit = rom_size - nlen;
    /* primary pass: cur_off+1 .. end */
    for (size_t i = cur_off + 1; i <= limit; i++) {
        if (memcmp(rom + i, needle, nlen) == 0) return i;
    }
    /* wrap: 0 .. cur_off */
    size_t wrap_end = cur_off < limit ? cur_off : limit;
    for (size_t i = 0; i <= wrap_end; i++) {
        if (memcmp(rom + i, needle, nlen) == 0) { *wrapped = true; return i; }
    }
    return SIZE_MAX;
}

/* Search backward from cur_off-1; wraps around to end if not found. */
static size_t hex_search_backward(const uint8_t *rom, size_t rom_size,
                                  size_t cur_off, const uint8_t *needle, size_t nlen,
                                  bool *wrapped)
{
    *wrapped = false;
    if (nlen == 0 || nlen > rom_size) return SIZE_MAX;
    size_t limit = rom_size - nlen;
    /* primary pass: cur_off-1 .. 0 */
    if (cur_off > 0) {
        size_t start = (cur_off - 1) < limit ? (cur_off - 1) : limit;
        for (size_t i = start + 1; i-- > 0; ) {
            if (memcmp(rom + i, needle, nlen) == 0) return i;
        }
    }
    /* wrap: end .. cur_off */
    for (size_t i = limit + 1; i-- > cur_off; ) {
        if (memcmp(rom + i, needle, nlen) == 0) { *wrapped = true; return i; }
    }
    return SIZE_MAX;
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
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (btn_w + ImGui::GetStyle().ItemSpacing.x) * 2.0f - 2.0f);
        bool enter_next = ImGui::InputText("##hexsearch", s->hex_search_input,
                                           sizeof(s->hex_search_input),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        bool do_next = enter_next || ImGui::Button("Next##hexsrch");
        ImGui::SameLine();
        bool do_prev = ImGui::Button("Prev##hexsrch");

        if ((do_next || do_prev) && s->hex_search_input[0]) {
            uint8_t needle[32];
            int nlen = parse_hex_search_bytes(s->hex_search_input, needle, (int)sizeof(needle));
            if (nlen > 0) {
                bool wrapped = false;
                size_t cur = s->hex_active ? s->hex_selected_offset : 0;
                size_t found = do_next
                    ? hex_search_forward (p->rom.data, p->rom.size, cur, needle, (size_t)nlen, &wrapped)
                    : hex_search_backward(p->rom.data, p->rom.size, cur, needle, (size_t)nlen, &wrapped);
                if (found != SIZE_MAX) {
                    s->hex_selected_offset = found;
                    s->hex_active          = true;
                    s->hex_request_follow  = 1;
                    size_t li;
                    if (find_line_by_rom_offset(*dp, found, &li)) {
                        select_line(s, li, 1);
                        s->hex_prev_selected_line = s->selected_line;
                    }
                    set_status(s, wrapped ? "search: found (wrapped)" : "search: found");
                } else {
                    set_status(s, "search: not found");
                }
            } else {
                set_status(s, "search: invalid hex");
            }
        }
    }

    ImGui::BeginChild("hex_grid", ImVec2(0.0f, -inspector_h), false);
    s->hex_window_focused = ImGui::IsWindowFocused();

    /* Keyboard cursor navigation (arrow keys / PgUp / PgDn) when hex view is focused. */
    if (s->hex_window_focused && s->hex_active && p->rom.size > 0) {
        auto hex_move = [&](int64_t delta) {
            int64_t next = (int64_t)s->hex_selected_offset + delta;
            if (next < 0) next = 0;
            if (next >= (int64_t)p->rom.size) next = (int64_t)p->rom.size - 1;
            s->hex_anchor_offset   = (size_t)next;
            s->hex_selected_offset = (size_t)next;
            s->hex_has_range       = false;
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
        std::vector<uint8_t> kinds(vis_count, (uint8_t)APEX_RENDER_BLOCK_UNKNOWN);
        {
            uint8_t cur_kind = (uint8_t)APEX_RENDER_BLOCK_UNKNOWN;
            size_t fill = vis_start;
            for (size_t li = 0; li < d->line_count && fill < vis_end; li++) {
                const ApexRenderedLine *l = &d->lines[li];
                if (!l->has_location) continue;
                /* Resolve extended kind: DATA lines with distinct pseudo-ops get own colors.
                   SPRITE block kind (=6) maps directly; DATA sub-types use indices 7-10. */
                uint8_t lk = (uint8_t)l->block_kind;
                if (l->block_kind == APEX_RENDER_BLOCK_DATA && l->text && l->length >= 3) {
                    const char *p2 = l->text;
                    size_t rem = l->length;
                    while (rem > 0 && (*p2 == ' ' || *p2 == '\t')) { p2++; rem--; }
                    if ((rem >= 12 && memcmp(p2, "STRING_FIXED", 12) == 0 &&
                         (rem == 12 || p2[12] == ' ' || p2[12] == '\t')) ||
                        (rem >= 9 && memcmp(p2, "STRING_LP", 9) == 0 &&
                         (rem == 9 || p2[9] == ' ' || p2[9] == '\t')) ||
                        (rem >= 6 && memcmp(p2, "STRING", 6) == 0 &&
                         (rem == 6 || p2[6] == ' ' || p2[6] == '\t')))
                        lk = 7; /* STRING / STRING_LP / STRING_FIXED — purple */
                    else if (rem >= 3 && memcmp(p2, ".DW", 3) == 0 &&
                             (rem == 3 || p2[3] == ' ' || p2[3] == '\t'))
                        lk = 8; /* .DW — cyan */
                    else if (rem >= 4 && memcmp(p2, "FAR_", 4) == 0)
                        lk = 9; /* FAR pointer — red */
                    else {
                        size_t ri;
                        for (ri = 0; ri < p->data_ranges.count; ri++) {
                            const DataRange *dr = &p->data_ranges.items[ri];
                            if (dr->kind == DATA_DMD_FULLFRAME &&
                                dr->bank == l->bank &&
                                l->cpu_addr >= dr->addr &&
                                l->cpu_addr < dr->addr + APEX_DMD_PAGE_BYTES) {
                                lk = 10; /* DMD fullframe — magenta */
                                break;
                            }
                        }
                    }
                }
                if (l->rom_addr <= vis_start) {
                    cur_kind = lk;
                } else {
                    size_t boundary = std::min(vis_end, l->rom_addr);
                    while (fill < boundary)
                        kinds[fill++ - vis_start] = cur_kind;
                    cur_kind = lk;
                }
            }
            while (fill < vis_end)
                kinds[fill++ - vis_start] = cur_kind;
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
                ? std::min(s->hex_anchor_offset, s->hex_selected_offset) : 0;
            size_t rng_hi = s->hex_has_range
                ? std::max(s->hex_anchor_offset, s->hex_selected_offset) : 0;
            for (int col = 0; col < bytes_per_row; col++) {
                size_t o = row_start + (size_t)col;
                if (o >= p->rom.size) {
                    break;
                }
                uint8_t v        = p->rom.data[o];
                bool is_cur      = s->hex_active && o == s->hex_selected_offset;
                bool is_in_range = s->hex_active && s->hex_has_range
                                   && o >= rng_lo && o <= rng_hi;
                uint8_t ki  = (o >= vis_start && o < vis_end) ? kinds[o - vis_start] : 0u;
                const ImVec4 &tc = kind_colors[ki < kind_colors_count ? ki : 0u];

                ImGui::SameLine(hex_x0 + (float)col * char_w * 3.0f);
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

                if (ImGui::IsItemHovered()) {
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
                        bool is_data = !find_line_by_rom_offset(d, o, &li) ||
                                       d->lines[li].block_kind == APEX_RENDER_BLOCK_DATA ||
                                       d->lines[li].block_kind == APEX_RENDER_BLOCK_TABLE ||
                                       d->lines[li].block_kind == APEX_RENDER_BLOCK_UNCLASSIFIED;
                        if (is_data) render_disasm_preview(p, o, bank, cpu_addr);
                    }
                    ImGui::EndTooltip();
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    bool shift = ImGui::GetIO().KeyShift;
                    if (shift && s->hex_active) {
                        s->hex_selected_offset = o;
                        s->hex_has_range = (s->hex_anchor_offset != o);
                    } else {
                        s->hex_anchor_offset   = o;
                        s->hex_selected_offset = o;
                        s->hex_has_range       = false;
                    }
                    s->hex_active = true;
                    size_t li;
                    if (find_line_by_rom_offset(d, o, &li)) {
                        select_line(s, li, 1);
                        s->hex_prev_selected_line = s->selected_line;
                    }
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    s->hex_selected_offset = o;
                    s->hex_active = true;
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
                uint8_t ki  = (o >= vis_start && o < vis_end) ? kinds[o - vis_start] : 0u;
                const ImVec4 &tc = kind_colors[ki < kind_colors_count ? ki : 0u];

                ImGui::SameLine(asc_x + (float)col * char_w);
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

                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    bool shift = ImGui::GetIO().KeyShift;
                    if (shift && s->hex_active) {
                        s->hex_selected_offset = o;
                        s->hex_has_range = (s->hex_anchor_offset != o);
                    } else {
                        s->hex_anchor_offset   = o;
                        s->hex_selected_offset = o;
                        s->hex_has_range       = false;
                    }
                    s->hex_active = true;
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
        if (ImGui::MenuItem("Copy selection", "Ctrl+C")) {
            copy_selection_to_clipboard(*dp, s);
            set_status(s, "copied");
        }
        if (ImGui::MenuItem("Mark as Code",   "C")) { apply_code_at_selection(p, dp, s); }
        if (s->hex_has_range) {
            size_t rlo = std::min(s->hex_anchor_offset, s->hex_selected_offset);
            size_t rhi = std::max(s->hex_anchor_offset, s->hex_selected_offset);
            size_t rn  = rhi - rlo + 1;
            char blabel[48], slabel[48];
            snprintf(blabel, sizeof(blabel), "Assign bytes[%zu]", rn);
            snprintf(slabel, sizeof(slabel), "Assign string[%zu]", rn);
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
        } else {
            if (ImGui::MenuItem("Mark as Data", "D")) {
                char spec[32];
                snprintf(spec, sizeof(spec), "bytes[%d]",
                         s->edit_data_length > 0 ? s->edit_data_length : 1);
                apply_data_at_selection(p, dp, s, spec);
            }
        }
        if (ImGui::MenuItem("Mark as String", "S")) { apply_string_at_selection(p, dp, s); }
        if (ImGui::MenuItem("Mark as String LP"))   { apply_string_lp_at_selection(p, dp, s); }
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
                                 int add_count)
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
                        ImGui::Text("0x%02x = %s", ct->values[vi].value, ct->values[vi].name);
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
        char chip[48];
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
            };
            kname = "?";
            for (int k = 0; k < (int)(sizeof(kN)/sizeof(kN[0])); k++) {
                if (kN[k].kind == fields[i].kind) { kname = kN[k].name; break; }
            }
        }
        if (fields[i].count > 1) {
            snprintf(chip, sizeof(chip), "%s[%d]##chip%d", kname, fields[i].count, i);
        } else {
            snprintf(chip, sizeof(chip), "%s##chip%d", kname, i);
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
    static const char *dom[] = {"routine_docs", "table_docs"};

    uint8_t b;
    uint32_t a;
    if (!selected_address(*dp, s, &b, &a)) {
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
    if (ImGui::Button("String LP##cls")) { apply_string_lp_at_selection(p, dp, s); }
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
                         s->edit_field_add_count);
    render_field_chips(s->edit_inline_fields, &s->edit_inline_count);
    ImGui::PopID();
    if (ImGui::Button("Apply##inl")) {
        if (s->edit_inline_count > 0) {
            char spec[256];
            fields_to_spec(spec, sizeof(spec), s->edit_inline_fields, s->edit_inline_count);
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
                         s->edit_field_add_count);
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
    if (ImGui::Combo("##doctype", &s->edit_doc_mode, dom, 2)) {
        load_doc_editor_buffer(p, s, b, a);
    }
    if (s->request_focus_doc) {
        ImGui::SetKeyboardFocusHere();
        s->request_focus_doc = 0;
    }
    ImGui::InputTextMultiline("##doc", s->edit_doc_input, 1024,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5),
                              ImGuiInputTextFlags_WordWrap);
    if (ImGui::Button("Apply##doc")) {
        if (s->edit_doc_input[0]) {
            if (apex_project_set_doc(p, s->edit_doc_mode == EDIT_DOC_TABLE, 1, b, a,
                                     s->edit_doc_input) == 0) {
                rerender_and_reselect(p, dp, s, b, a);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##doc")) {
        if (apex_project_clear_doc(p, s->edit_doc_mode == EDIT_DOC_TABLE, 1, b, a) == 0) {
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

void render_tables_window(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    if (ImGui::Button("Search Tables (Auto)")) {
        auto_search_tables(p, dp, s);
    }
    ImGui::Separator();
    if (ImGui::BeginTable("tables_list", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Addr",    ImGuiTableColumnFlags_WidthFixed,  100.0f);
        ImGui::TableSetupColumn("Setup",   ImGuiTableColumnFlags_WidthFixed,  200.0f);
        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,   80.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < p->tables.count; i++) {
            const auto *t = &p->tables.items[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            char a[32];
            snprintf(a, 32, "B%02x_A%04x", t->bank, t->addr);
            if (ImGui::Selectable(a, false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(0)) {
                size_t li;
                if (apex_render_find_line_by_address(*dp, t->bank, t->addr, &li)) {
                    select_line(s, li, 1);
                }
            }

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

            ImGui::TableSetColumnIndex(2);
            const char *existing_doc = config_doc_at(&p->table_docs, t->bank, t->addr);
            char doc_buf[512] = "";
            if (existing_doc) {
                strncpy(doc_buf, existing_doc, 511);
                doc_buf[511] = '\0';
            }
            ImGui::PushItemWidth(-FLT_MIN);
            if (ImGui::InputText("##doc", doc_buf, 512, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (apex_project_set_doc(p, 1, 1, t->bank, t->addr, doc_buf) == 0) {
                    rerender_and_reselect(p, dp, s, t->bank, t->addr);
                    set_status(s, "Table comment updated");
                }
            }
            ImGui::PopItemWidth();

            ImGui::TableSetColumnIndex(3);
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
}

/* Build a values_str like "0x00:name, 0x01:other" from a ConfigType, excluding index `skip`.
   If skip < 0 append the new_val/new_name pair. */
static std::string build_values_str(const ConfigType *ct, int skip,
                                    uint32_t new_val, const char *new_name)
{
    std::string out;
    char tmp[64];
    for (size_t i = 0; i < ct->value_count; i++) {
        if ((int)i == skip) continue;
        if (!out.empty()) out += ", ";
        snprintf(tmp, sizeof(tmp), "0x%02x:%s", ct->values[i].value, ct->values[i].name);
        out += tmp;
    }
    if (new_name && *new_name) {
        if (!out.empty()) out += ", ";
        snprintf(tmp, sizeof(tmp), "0x%02x:%s", new_val, new_name);
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
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
            ImVec2(0, p->symbols.count > 0 ? 200.0f : 60.0f))) {
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();

        bool deleted = false;
        for (size_t i = 0; i < p->symbols.count && !deleted; i++) {
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

    /* ---- Usages of selected symbol ---- */
    if (s->sym_selected >= 0 && (size_t)s->sym_selected < p->symbols.count && document) {
        const char *selname = p->symbols.items[s->sym_selected].name;
        ImGui::SeparatorText("Usages in disassembly");
        auto usages = find_symbol_usages(document, selname);
        if (usages.empty()) {
            ImGui::TextDisabled("none found");
        } else {
            float list_h = std::min((float)usages.size() * ImGui::GetFrameHeightWithSpacing() + 4.0f,
                                    120.0f);
            ImGui::BeginChild("sym_usages", ImVec2(0, list_h), false);
            for (size_t i = 0; i < usages.size(); i++) {
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
                ImGui::Text("  0x%02x = %s", ct->values[vi].value, ct->values[vi].name);
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
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Inline",  ImGuiTableColumnFlags_WidthStretch);
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

void render_entries_list(ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    static char filter[128] = "";
    ImGui::InputText("Filter##entlist", filter, sizeof(filter));
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", p->config_entries.count);

    if (ImGui::BeginTable("entries", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::vector<size_t> rows;
        for (size_t i = 0; i < p->config_entries.count; i++) {
            const ConfigEntry *e = &p->config_entries.items[i];
            uint8_t bank = e->has_bank ? e->bank : 0xffu;
            uint32_t addr = e->addr;
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
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (found) {
                        select_line(s, li, ImGui::IsMouseDoubleClicked(0) ? 1 : 0);
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(lbl.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void render_pattern_search(ApexProject *project, const ApexRenderedDocument **document_ptr,
                           UiState *state)
{
    if (state->request_focus_pattern_search) {
        ImGui::SetKeyboardFocusHere();
        state->request_focus_pattern_search = 0;
    }
    ImGui::TextUnformatted("Pattern (hex bytes, ?? = wildcard):");
    ImGui::SetNextItemWidth(-80.0f);
    bool run = ImGui::InputText("##pat", state->pattern_search_input, 128,
                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    run |= ImGui::Button("Search");
    if (run && state->pattern_search_input[0]) {
        state->pattern_search_results = search_hex_pattern(project, state->pattern_search_input);
        char msg[64];
        snprintf(msg, 64, "%lu match(es)%s",
                 (unsigned long)state->pattern_search_results.size(),
                 state->pattern_search_results.size() >= 500 ? " (capped at 500)" : "");
        set_status(state, msg);
    }
    ImGui::Text("%lu result(s)", (unsigned long)state->pattern_search_results.size());
    if (ImGui::BeginTable("pat_results", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("ROM Offset", ImGuiTableColumnFlags_WidthFixed,  90.0f);
        ImGui::TableSetupColumn("CPU Addr",   ImGuiTableColumnFlags_WidthFixed,  90.0f);
        ImGui::TableSetupColumn("Bytes",      ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int)state->pattern_search_results.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                size_t offset = state->pattern_search_results[(size_t)row];
                uint8_t bank;
                uint32_t cpu_addr;
                size_t line_idx = 0;
                bool has_addr = rom_offset_to_cpu_address(project, offset, &bank, &cpu_addr) != 0;
                bool has_line = find_line_by_rom_offset(*document_ptr, offset, &line_idx);
                ImGui::PushID((int)row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char off_buf[24];
                snprintf(off_buf, sizeof(off_buf), "0x%06lx", (unsigned long)offset);
                if (ImGui::Selectable(off_buf,
                        has_line && state->selected_line == line_idx,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    if (has_line) {
                        select_line(state, line_idx, 1);
                    }
                }
                ImGui::TableSetColumnIndex(1);
                if (has_addr) {
                    ImGui::Text("B%02x_A%04x", bank, (unsigned)cpu_addr & 0xffff);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(2);
                char bytes_buf[40] = "";
                size_t show = 8;
                if (offset + show > project->rom.size) {
                    show = project->rom.size - offset;
                }
                for (size_t k = 0; k < show; k++) {
                    char hex[4];
                    snprintf(hex, sizeof(hex), k ? " %02X" : "%02X",
                             (unsigned)project->rom.data[offset + k]);
                    strncat(bytes_buf, hex, sizeof(bytes_buf) - strlen(bytes_buf) - 1);
                }
                ImGui::TextUnformatted(bytes_buf);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void render_ram_refs(const ApexProject *project, const ApexRenderedDocument *document,
                     UiState *state)
{
    (void)project;
    if (state->request_focus_ram_refs) {
        ImGui::SetKeyboardFocusHere();
        state->request_focus_ram_refs = 0;
    }
    ImGui::TextUnformatted("RAM address ($XX, $XXXX, or hex):");
    ImGui::SetNextItemWidth(-80.0f);
    bool run = ImGui::InputText("##ramaddr", state->ram_ref_input, 32,
                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    run |= ImGui::Button("Search");
    if (run && state->ram_ref_input[0]) {
        state->ram_ref_results = find_ram_refs(document, state->ram_ref_input);
        char msg[64];
        snprintf(msg, 64, "%lu RAM ref(s) found", (unsigned long)state->ram_ref_results.size());
        set_status(state, msg);
    }
    ImGui::Text("%lu result(s)", (unsigned long)state->ram_ref_results.size());
    if (ImGui::BeginTable("ram_results", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,  90.0f);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed,  50.0f);
        ImGui::TableSetupColumn("Text",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int)state->ram_ref_results.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                size_t li = state->ram_ref_results[(size_t)row];
                if (li >= document->line_count) {
                    continue;
                }
                const auto *line = &document->lines[li];
                ImGui::PushID((int)row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char addr_buf[32];
                if (line->has_location) {
                    snprintf(addr_buf, sizeof(addr_buf), "B%02x_A%04x",
                             line->bank, (unsigned)line->cpu_addr & 0xffff);
                } else {
                    strcpy(addr_buf, "-");
                }
                if (ImGui::Selectable(addr_buf, state->selected_line == li,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    select_line(state, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(block_name(line->block_kind));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(line->text, line->text + line->length);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void render_code_candidates(ApexProject *project,
                            const ApexRenderedDocument **document_ptr,
                            UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;

    /* ---- Header row ---- */
    bool want_scan = ImGui::Button("Scan");
    ImGui::SameLine();
    if (state->code_candidates_stale)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "stale – rescan recommended");
    else if (state->code_candidates.count == 0 && !state->code_candidates_stale)
        ImGui::TextDisabled("no candidates (run scan)");
    else
        ImGui::Text("%zu candidates", state->code_candidates.count);

    ImGui::SameLine();
    ImGui::TextDisabled("| Tier 1 = far-ptr  Tier 2 = probe");

    if (want_scan) {
        apex_free_code_candidates(&state->code_candidates);
        apex_scan_code_candidates(project, &state->code_candidates);
        state->code_candidates_stale = false;
    }

    if (state->code_candidates.count == 0) return;

    ImGui::Separator();

    /* ---- Table ---- */
    if (!ImGui::BeginTable("cand_tbl", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
            ImVec2(0, 0)))
        return;

    ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed,  48.0f);
    ImGui::TableSetupColumn("T",     ImGuiTableColumnFlags_WidthFixed,  18.0f);
    ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,  90.0f);
    ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableHeadersRow();

    /* iterate a copy index so "Accept" can remove items without iterator UB */
    size_t i = 0;
    while (i < state->code_candidates.count) {
        ApexCodeCandidate *c = &state->code_candidates.items[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();

        /* score column — colour-coded */
        ImGui::TableSetColumnIndex(0);
        ImVec4 score_col = c->score >= 80
            ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f)
            : c->score >= 60
                ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                : ImVec4(0.9f, 0.55f, 0.2f, 1.0f);
        ImGui::TextColored(score_col, "%d", c->score);

        /* tier column */
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%d", c->tier);

        /* address column */
        ImGui::TableSetColumnIndex(2);
        char addr_buf[24];
        snprintf(addr_buf, sizeof(addr_buf), "B%02x_A%04x",
                 c->bank, (unsigned)c->addr & 0xffffu);
        ImGui::TextUnformatted(addr_buf);

        /* preview column */
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(c->preview);

        /* action column */
        ImGui::TableSetColumnIndex(4);
        if (ImGui::SmallButton("Go")) {
            size_t li;
            if (!apex_render_find_line_by_address(document, c->bank, c->addr, &li)) {
                /* Exact match missing (unclassified region): find the nearest
                   rendered line with matching bank and cpu_addr <= candidate. */
                li = (size_t)-1;
                for (size_t di = 0; di < document->line_count; di++) {
                    const ApexRenderedLine *dl = &document->lines[di];
                    if (dl->has_location && dl->bank == c->bank &&
                        dl->cpu_addr <= c->addr) {
                        if (li == (size_t)-1 ||
                            dl->cpu_addr > document->lines[li].cpu_addr)
                            li = di;
                    }
                }
            }
            if (li != (size_t)-1)
                select_line(state, li, 1);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Accept")) {
            /* classify as code and rerender */
            if (apex_project_set_kind(project, 1, c->bank, c->addr,
                                      APEX_KIND_CODE, NULL) == 0) {
                state->overlay_dirty = true;
                state->labels_valid  = false;
                state->code_candidates_stale = true;
                rerender_and_reselect(project, document_ptr, state, c->bank, c->addr);
            }
            document = *document_ptr;
            /* remove this candidate from the list */
            size_t rem = state->code_candidates.count - i - 1;
            if (rem > 0)
                memmove(&state->code_candidates.items[i],
                        &state->code_candidates.items[i + 1],
                        rem * sizeof(state->code_candidates.items[0]));
            state->code_candidates.count--;
            ImGui::PopID();
            continue;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) {
            size_t rem = state->code_candidates.count - i - 1;
            if (rem > 0)
                memmove(&state->code_candidates.items[i],
                        &state->code_candidates.items[i + 1],
                        rem * sizeof(state->code_candidates.items[0]));
            state->code_candidates.count--;
            ImGui::PopID();
            continue;
        }
        ImGui::PopID();
        i++;
    }
    ImGui::EndTable();
}

void render_inline_candidates(ApexProject *project,
                              const ApexRenderedDocument **document_ptr,
                              UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;

    bool want_scan = ImGui::Button("Scan");
    ImGui::SameLine();
    if (state->inline_candidates_stale)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "stale – rescan recommended");
    else if (state->inline_candidates.count == 0)
        ImGui::TextDisabled("no candidates (run scan)");
    else
        ImGui::Text("%zu candidates", state->inline_candidates.count);

    ImGui::SameLine();
    ImGui::TextDisabled("| score: green>=80 yellow>=60 orange<60");

    if (want_scan) {
        apex_free_inline_candidates(&state->inline_candidates);
        apex_scan_inline_candidates(project, &state->inline_candidates);
        state->inline_candidates_stale = false;
    }

    if (state->inline_candidates.count == 0) return;

    ImGui::Separator();

    if (!ImGui::BeginTable("icand_tbl", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
            ImVec2(0, 0)))
        return;

    ImGui::TableSetupColumn("Score",     ImGuiTableColumnFlags_WidthFixed,  48.0f);
    ImGui::TableSetupColumn("Addr",      ImGuiTableColumnFlags_WidthFixed,  90.0f);
    ImGui::TableSetupColumn("Spec",      ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Callsites", ImGuiTableColumnFlags_WidthFixed,  70.0f);
    ImGui::TableSetupColumn("##act",     ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableHeadersRow();

    size_t i = 0;
    while (i < state->inline_candidates.count) {
        ApexInlineCandidate *c = &state->inline_candidates.items[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImVec4 score_col = c->score >= 80
            ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f)
            : c->score >= 60
                ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                : ImVec4(0.9f, 0.55f, 0.2f, 1.0f);
        ImGui::TextColored(score_col, "%d", c->score);

        ImGui::TableSetColumnIndex(1);
        char addr_buf[24];
        snprintf(addr_buf, sizeof(addr_buf), "B%02x_A%04x",
                 c->bank, (unsigned)c->addr & 0xffffu);
        ImGui::TextUnformatted(addr_buf);

        ImGui::TableSetColumnIndex(2);
        /* Editable spec field so user can correct before accepting */
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##spec", c->spec, sizeof(c->spec));

        ImGui::TableSetColumnIndex(3);
        if (c->callsite_count > 0)
            ImGui::Text("%d/%d", c->callsite_valid, c->callsite_count);
        else
            ImGui::TextDisabled("?");

        ImGui::TableSetColumnIndex(4);
        if (ImGui::SmallButton("Go")) {
            size_t li;
            if (!apex_render_find_line_by_address(document, c->bank, c->addr, &li)) {
                li = (size_t)-1;
                for (size_t di = 0; di < document->line_count; di++) {
                    const ApexRenderedLine *dl = &document->lines[di];
                    if (dl->has_location && dl->bank == c->bank &&
                        dl->cpu_addr <= c->addr &&
                        (li == (size_t)-1 ||
                         dl->cpu_addr > document->lines[li].cpu_addr))
                        li = di;
                }
            }
            if (li != (size_t)-1) select_line(state, li, 1);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Accept")) {
            if (apex_project_set_inline(project, 1, c->bank, c->addr, c->spec) == 0) {
                state->overlay_dirty = true;
                state->labels_valid  = false;
                state->inline_candidates_stale = true;
                rerender_and_reselect(project, document_ptr, state, c->bank, c->addr);
                document = *document_ptr;
            }
            size_t rem = state->inline_candidates.count - i - 1;
            if (rem > 0)
                memmove(&state->inline_candidates.items[i],
                        &state->inline_candidates.items[i + 1],
                        rem * sizeof(state->inline_candidates.items[0]));
            state->inline_candidates.count--;
            ImGui::PopID();
            continue;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) {
            size_t rem = state->inline_candidates.count - i - 1;
            if (rem > 0)
                memmove(&state->inline_candidates.items[i],
                        &state->inline_candidates.items[i + 1],
                        rem * sizeof(state->inline_candidates.items[0]));
            state->inline_candidates.count--;
            ImGui::PopID();
            continue;
        }
        ImGui::PopID();
        i++;
    }
    ImGui::EndTable();
}

void render_ref_exclusions(ApexProject *project, const ApexRenderedDocument **document_ptr,
                           UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;

    ImGui::TextDisabled("Addresses excluded from false-positive ref detection");
    ImGui::Separator();

    if (project->ref_exclusions.count == 0) {
        ImGui::TextDisabled("(no exclusions)");
        return;
    }

    if (!ImGui::BeginTable("excl_table", 3,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, 0))) {
        return;
    }
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100.f);
    ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##act",   ImGuiTableColumnFlags_WidthFixed, 120.f);
    ImGui::TableHeadersRow();

    int to_remove_idx = -1;
    for (size_t i = 0; i < project->ref_exclusions.count; i++) {
        const ConfigEntry *e = &project->ref_exclusions.items[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        char addr_buf[32];
        if (e->has_bank) {
            snprintf(addr_buf, sizeof(addr_buf), "B%02x_A%04x", e->bank,
                     (unsigned)e->addr & 0xffff);
        } else {
            snprintf(addr_buf, sizeof(addr_buf), "0x%04x", (unsigned)e->addr & 0xffff);
        }
        ImGui::TextUnformatted(addr_buf);

        ImGui::TableSetColumnIndex(1);
        std::string lbl = label_at_address(document, state, e->bank, e->addr);
        if (!lbl.empty()) {
            ImGui::TextUnformatted(lbl.c_str());
        } else {
            ImGui::TextDisabled("-");
        }

        ImGui::TableSetColumnIndex(2);
        if (ImGui::Button("Navigate")) {
            size_t li;
            if (apex_render_find_line_by_address(document, e->bank, e->addr, &li)) {
                select_line(state, li, 1);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            to_remove_idx = (int)i;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();

    if (to_remove_idx >= 0) {
        const ConfigEntry *e = &project->ref_exclusions.items[to_remove_idx];
        uint8_t bank = e->bank;
        uint32_t addr = e->addr;
        int has_bank = e->has_bank;
        uint8_t sel_bank = 0;
        uint32_t sel_addr = 0;
        selected_address(document, state, &sel_bank, &sel_addr);
        apex_project_remove_ref_exclusion(project, has_bank, bank, addr);
        const ApexRenderedDocument *new_doc = apex_project_render(project, 1, 0);
        if (new_doc) {
            *document_ptr = new_doc;
        }
        state->labels_valid = false;
        state->overlay_dirty = true;
        set_status(state, "ref exclusion removed");
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

        /* Build per-byte kind array (same logic as hex view). */
        std::vector<uint8_t> kinds(rom_size, (uint8_t)APEX_RENDER_BLOCK_UNKNOWN);
        {
            uint8_t cur_kind = (uint8_t)APEX_RENDER_BLOCK_UNKNOWN;
            size_t fill = 0;
            for (size_t li = 0; li < d->line_count; li++) {
                const ApexRenderedLine *l = &d->lines[li];
                if (!l->has_location) continue;
                uint8_t lk = (uint8_t)l->block_kind;
                if (l->block_kind == APEX_RENDER_BLOCK_DATA && l->text && l->length >= 3) {
                    const char *tp = l->text;
                    size_t rem = l->length;
                    while (rem > 0 && (*tp == ' ' || *tp == '\t')) { tp++; rem--; }
                    if ((rem >= 12 && memcmp(tp, "STRING_FIXED", 12) == 0 &&
                         (rem == 12 || tp[12] == ' ' || tp[12] == '\t')) ||
                        (rem >= 9 && memcmp(tp, "STRING_LP", 9) == 0 &&
                         (rem == 9 || tp[9] == ' ' || tp[9] == '\t')) ||
                        (rem >= 6 && memcmp(tp, "STRING", 6) == 0 &&
                         (rem == 6 || tp[6] == ' ' || tp[6] == '\t')))
                        lk = 7;
                    else if (rem >= 3 && memcmp(tp, ".DW", 3) == 0 &&
                             (rem == 3 || tp[3] == ' ' || tp[3] == '\t'))
                        lk = 8;
                    else if (rem >= 4 && memcmp(tp, "FAR_", 4) == 0)
                        lk = 9;
                    else {
                        for (size_t ri = 0; ri < p->data_ranges.count; ri++) {
                            const DataRange *dr = &p->data_ranges.items[ri];
                            if (dr->kind == DATA_DMD_FULLFRAME &&
                                dr->bank == l->bank &&
                                l->cpu_addr >= dr->addr &&
                                l->cpu_addr < dr->addr + APEX_DMD_PAGE_BYTES) {
                                lk = 10;
                                break;
                            }
                        }
                    }
                }
                if (l->rom_addr <= fill) {
                    cur_kind = lk;
                } else {
                    size_t boundary = std::min(rom_size, l->rom_addr);
                    while (fill < boundary)
                        kinds[fill++] = cur_kind;
                    cur_kind = lk;
                }
            }
            while (fill < rom_size)
                kinds[fill++] = cur_kind;
        }

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

void render_dmd_list_window(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    size_t dmd_count = 0;
    for (size_t i = 0; i < p->data_ranges.count; i++)
        if (p->data_ranges.items[i].kind == DATA_DMD_FULLFRAME)
            dmd_count++;
    ImGui::TextDisabled("(%zu frames)", dmd_count);

    if (ImGui::BeginTable("dmd_list", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,  110.0f);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        int row_id = 0;
        for (size_t i = 0; i < p->data_ranges.count; i++) {
            const DataRange *dr = &p->data_ranges.items[i];
            if (dr->kind != DATA_DMD_FULLFRAME) continue;

            char addrstr[32];
            snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x",
                     (unsigned)dr->bank, (unsigned)dr->addr & 0xffffu);
            std::string lbl = label_at_address(d, s, dr->bank, dr->addr);
            size_t li = 0;
            bool found = apex_render_find_line_by_address(d, dr->bank, dr->addr, &li) != NULL;
            bool sel = found && s->selected_line == li;

            ImGui::PushID(row_id++);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(addrstr, sel,
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap)) {
                if (found) select_line(s, li, 1);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem)) {
                DmdPreviewInfo pr = {};
                if (decode_dmd_preview_at(p, dr->bank, dr->addr, &pr)) {
                    ImGui::BeginTooltip();
                    render_dmd_preview(pr, 4.0f);
                    ImGui::EndTooltip();
                }
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(lbl.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

/* Resolve a WPC far pointer {cpu, bank} to a ROM offset.
   Returns true on success.  bank == 0xFF means the system bank. */
static bool vsi_resolve_far(const ApexProject *p,
                             uint32_t cpu, uint8_t bank, size_t *out)
{
    if (bank == 0xFFu) {
        if (cpu < 0x8000u || cpu >= 0x10000u) return false;
        size_t o = p->paged_size + (cpu - 0x8000u);
        if (o >= p->rom.size) return false;
        *out = o;
        return true;
    }
    if (cpu < 0x4000u || cpu >= 0x8000u) return false;
    int bi = bank_index_for_far_ref(p->rom.data, p->banks, bank);
    if (bi < 0) return false;
    size_t o = (size_t)bi * 0x4000u + (cpu - 0x4000u);
    if (o >= p->rom.size) return false;
    *out = o;
    return true;
}

/* Resolve a 16-bit near pointer from a ptr16_sprite table entry.
   If cpu >= 0x8000 the target is in the system bank regardless of tbl_bank. */
static bool ptr16_sprite_resolve(const ApexProject *p, uint32_t cpu, uint8_t tbl_bank,
                                  size_t *out, uint8_t *out_bank)
{
    uint8_t effective_bank = (cpu >= 0x8000u) ? 0xFFu : tbl_bank;
    if (!vsi_resolve_far(p, cpu, effective_bank, out)) return false;
    *out_bank = effective_bank;
    return true;
}

/* Scan the ROM for the WPC font-table code signature, walk the master VSI
   table, and populate s->vsi_table_entries with every discoverable image. */
static void scan_vsi_table_candidates(const ApexProject *p, UiState *s)
{
    s->vsi_table_entries.clear();
    s->vsi_sub_tables.clear();
    s->vsi_table_scan_done = true; /* set early so early-returns leave it true */

    if (!p->rom.data || p->rom.size == 0 || p->paged_size >= p->rom.size)
        return;

    /* --- Step 1: find the LDX / ABX / ASLB code signature in system bank ---
       Pattern (from WPCEdit initTableAddrs):
         BE xx xx 3A 58 3A D6 ?? 34 04 (F6|BD) ?? ?? (BD|F6) ??
       where xx xx is the system-bank CPU address of the 3-byte far pointer
       that points to the font master table. */
    const uint8_t *sys = p->rom.data + p->paged_size;
    size_t sys_len = p->rom.size - p->paged_size;
    size_t sig = SIZE_MAX;
    for (size_t i = 0; i + 16u <= sys_len; i++) {
        if (sys[i]    == 0xBEu &&
            sys[i+3]  == 0x3Au && sys[i+4] == 0x58u &&
            sys[i+5]  == 0x3Au && sys[i+6] == 0xD6u &&
            sys[i+8]  == 0x34u && sys[i+9] == 0x04u &&
            (sys[i+10] == 0xF6u || sys[i+10] == 0xBDu) &&
            (sys[i+13] == 0xBDu || sys[i+13] == 0xF6u)) {
            sig = i;
            break;
        }
    }
    if (sig == SIZE_MAX) return;

    /* --- Step 2: extract the CPU address of the far-pointer cell ---
       The LDX operand (bytes 1-2 of the pattern) is a system-bank address
       that holds the 3-byte far pointer to the master table. */
    uint32_t ptr_cpu = ((uint32_t)sys[sig+1] << 8) | sys[sig+2];
    if (ptr_cpu < 0x8000u) return;
    size_t ptr_off = p->paged_size + (ptr_cpu - 0x8000u);
    if (ptr_off + 3u > p->rom.size) return;

    /* --- Step 3: read the 3-byte far pointer → master table address ---
       Force bank to 0xFF when cpu address is in system range (WPCEdit does
       the same when the page byte is not a valid paged bank indicator). */
    uint32_t master_cpu  = ((uint32_t)p->rom.data[ptr_off] << 8)
                           | p->rom.data[ptr_off+1];
    uint8_t  master_bank = p->rom.data[ptr_off+2];
    if (master_cpu >= 0x8000u) master_bank = 0xFFu;

    size_t master_off;
    if (!vsi_resolve_far(p, master_cpu, master_bank, &master_off)) return;

    /* --- Step 4: walk master table (array of 3-byte far pointers) ---
       Stop at the first entry that cannot be parsed as a valid VSI sub-table
       (matches WPCEdit's preAnalyzeVariableSizedImageTable behaviour). */
    uint8_t tmp[APEX_SPRITE_MAX_BYTES];

    for (int tidx = 0; tidx < 512; tidx++) {
        size_t eoff = master_off + (size_t)tidx * 3u;
        if (eoff + 3u > p->rom.size) break;

        uint32_t e_cpu  = ((uint32_t)p->rom.data[eoff] << 8) | p->rom.data[eoff+1];
        uint8_t  e_bank = p->rom.data[eoff+2];
        if (e_cpu >= 0x8000u) e_bank = 0xFFu;

        /* First hop: resolve the far pointer from the master table entry. */
        size_t inter_off;
        if (!vsi_resolve_far(p, e_cpu, e_bank, &inter_off)) break;

        /* Near-pointer fixup (some ROMs store a 2-byte near pointer at the
           first hop instead of the sub-table directly).  If the 2-byte word
           at the first hop is itself in the paged range, follow it (keeping
           the same bank byte). */
        if (inter_off + 2u <= p->rom.size) {
            uint32_t tmp_cpu = ((uint32_t)p->rom.data[inter_off] << 8)
                               | p->rom.data[inter_off+1];
            if (tmp_cpu >= 0x4000u && tmp_cpu < 0x8000u)
                e_cpu = tmp_cpu;
        }

        size_t sub_off;
        if (!vsi_resolve_far(p, e_cpu, e_bank, &sub_off)) break;
        if (sub_off + 4u > p->rom.size) break;

        /* --- Step 5: parse the sub-table header ---
           Layout: [ImgIndexMin][ImgIndexMax]... 0x00 [TableHeight] [TableSpacing]
                   followed by total_images × 2-byte BE near pointers. */
        const uint8_t *sub  = p->rom.data + sub_off;
        size_t         slen = p->rom.size - sub_off;

        size_t pos = 0;
        int total_imgs = 0;
        bool valid = true;
        while (pos + 1u < slen && sub[pos] != 0x00u) {
            uint8_t imin = sub[pos], imax = sub[pos+1];
            if (imin > imax || total_imgs + (imax - imin + 1) > 512) {
                valid = false; break;
            }
            total_imgs += (int)(imax - imin + 1);
            pos += 2;
        }
        if (!valid || total_imgs == 0) break;
        if (pos >= slen || sub[pos] != 0x00u) break;
        pos++; /* skip terminator */

        if (pos + 2u > slen) break;
        uint8_t tbl_h = sub[pos++];
        pos++; /* spacing byte */
        if (tbl_h == 0 || tbl_h > 32) break;
        if (pos + (size_t)total_imgs * 2u > slen) break;

        /* Record sub-table metadata (pos = header length: pairs + terminator + H + spacing) */
        {
            uint8_t sb; uint32_t sa;
            if (rom_offset_to_cpu_address(p, sub_off, &sb, &sa)) {
                UiState::VsiSubTableInfo st;
                st.table_idx   = tidx;
                st.bank        = sb;
                st.cpu_addr    = sa;
                st.header_len  = pos;
                st.num_images  = total_imgs;
                st.table_height= tbl_h;
                s->vsi_sub_tables.push_back(st);
            }
        }

        /* --- Step 6: enumerate images in this sub-table --- */
        for (int iidx = 0; iidx < total_imgs; iidx++) {
            size_t ppos = pos + (size_t)iidx * 2u;
            uint32_t img_cpu = ((uint32_t)sub[ppos] << 8) | sub[ppos+1];

            size_t img_off;
            uint8_t img_effective_bank;
            if (!ptr16_sprite_resolve(p, img_cpu, e_bank, &img_off, &img_effective_bank)) continue;

            const uint8_t *img = p->rom.data + img_off;
            size_t img_len = p->rom.size - img_off;

            uint8_t img_bank_out;
            uint32_t img_cpu_out;
            if (!rom_offset_to_cpu_address(p, img_off, &img_bank_out, &img_cpu_out))
                continue;

            uint8_t b0 = img_len > 0 ? img[0] : 0;
            bool is_nh = false;
            uint8_t w = 0, h = 0;
            bool decoded = false;

            if (b0 == 0x00u || b0 == 0xFDu || b0 == 0xFEu || b0 == 0xFFu) {
                uint8_t ht, vb, hb, enc; size_t con;
                if (apexsprite_decode(img, img_len, tmp,
                                      &ht, &vb, &hb, &w, &h, &enc, &con))
                    decoded = true;
            } else if (b0 >= 1u && b0 <= 128u) {
                uint8_t pw; size_t con;
                if (apexsprite_decode_noheader(img, img_len, tmp,
                                               tbl_h, &pw, &con)) {
                    w = pw; h = tbl_h; is_nh = true; decoded = true;
                }
            }
            if (!decoded) continue;

            bool classified = false;
            for (size_t ri = 0; ri < p->data_ranges.count; ri++) {
                const DataRange *dr = &p->data_ranges.items[ri];
                if ((dr->kind == DATA_SPRITE || dr->kind == DATA_SPRITE_NOHEADER) &&
                    dr->bank == img_bank_out && dr->addr == img_cpu_out) {
                    classified = true; break;
                }
            }

            UiState::VsiTableEntry e;
            e.table_idx   = tidx;
            e.image_idx   = iidx;
            e.table_height= tbl_h;
            e.bank        = img_bank_out;
            e.cpu_addr    = img_cpu_out;
            e.rom_offset  = img_off;
            e.is_noheader = is_nh;
            e.width       = w;
            e.height      = h;
            e.classified  = classified;
            s->vsi_table_entries.push_back(e);
        }
    }
}

/* Bulk-classify a complete VSI sub-table: header bytes + pointer-array table + all images. */
static void classify_vsi_table(ApexProject *p, const ApexRenderedDocument **dp,
                               UiState *s, int table_idx)
{
    const UiState::VsiSubTableInfo *st = nullptr;
    for (auto &t : s->vsi_sub_tables) {
        if (t.table_idx == table_idx) { st = &t; break; }
    }
    if (!st) return;

    /* Sub-table header bytes (min/max pairs + 0x00 + height + spacing) */
    if (st->header_len > 0) {
        char spec[32];
        snprintf(spec, sizeof(spec), "bytes[%zu]", st->header_len);
        apex_project_set_kind(p, 1, st->bank, st->cpu_addr, APEX_KIND_DATA, spec);
    }

    /* Pointer array: rows[N](ptr16_sprite), starting right after the header */
    if (st->num_images > 0) {
        uint32_t ptr_addr = st->cpu_addr + (uint32_t)st->header_len;
        char spec[64];
        snprintf(spec, sizeof(spec), "rows[%d](ptr16_sprite)", st->num_images);
        apex_project_set_kind(p, 1, st->bank, ptr_addr, APEX_KIND_TABLE, spec);
    }

    /* Individual images: walk the raw pointer array so we classify ALL entries,
       including those the scan skipped (null check handles 0x0000 / unmapped). */
    {
        size_t sub_off;
        if (vsi_resolve_far(p, st->cpu_addr, st->bank, &sub_off)) {
            size_t ptr_off = sub_off + st->header_len;
            uint8_t tmp[APEX_SPRITE_MAX_BYTES];
            for (int iidx = 0; iidx < st->num_images; iidx++) {
                size_t ppos = ptr_off + (size_t)iidx * 2u;
                if (ppos + 2u > p->rom.size) break;
                uint32_t img_cpu = ((uint32_t)p->rom.data[ppos] << 8) | p->rom.data[ppos+1];
                size_t img_off;
                uint8_t img_effective_bank;
                if (!ptr16_sprite_resolve(p, img_cpu, st->bank, &img_off, &img_effective_bank)) continue;
                uint8_t img_bank; uint32_t img_cpu_out;
                if (!rom_offset_to_cpu_address(p, img_off, &img_bank, &img_cpu_out)) continue;

                const uint8_t *img = p->rom.data + img_off;
                size_t img_len = p->rom.size - img_off;
                uint8_t b0 = img_len > 0 ? img[0] : 0;
                char spec[32];
                if (b0 >= 1u && b0 <= 128u) {
                    snprintf(spec, sizeof(spec), "sprite_noheader[%d]", (int)st->table_height);
                } else if (apexsprite_decode(img, img_len, tmp,
                                             nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, nullptr)) {
                    strcpy(spec, "sprite");
                } else {
                    /* Undecipherable format — classify as raw bytes so it gets a label */
                    strcpy(spec, "sprite");
                }
                apex_project_set_kind(p, 1, img_bank, img_cpu_out, APEX_KIND_DATA, spec);
            }
        }
    }

    /* Re-analyze, re-render, scroll to sub-table, then refresh the VSI scan. */
    rerender_and_reselect(p, dp, s, st->bank, st->cpu_addr);
    scan_vsi_table_candidates(p, s);
}

static void scan_sprite_candidates(const ApexProject *p, UiState *s)
{
    s->sprite_candidates.clear();
    if (!p->rom.data || p->rom.size == 0) {
        s->sprite_scan_done = true;
        return;
    }
    uint8_t tmp[APEX_SPRITE_MAX_BYTES];
    for (size_t off = 0; off < p->rom.size; off++) {
        uint8_t b0 = p->rom.data[off];
        if (b0 != 0x00u && b0 != 0xFDu && b0 != 0xFEu && b0 != 0xFFu)
            continue;
        const uint8_t *src = p->rom.data + off;
        size_t src_size = p->rom.size - off;
        uint8_t htype, voff_b, hoff_b, width, height, enc_type;
        size_t consumed;
        if (!apexsprite_decode(src, src_size, tmp, &htype, &voff_b, &hoff_b,
                               &width, &height, &enc_type, &consumed))
            continue;
        uint8_t bank;
        uint32_t cpu_addr;
        if (!rom_offset_to_cpu_address(p, off, &bank, &cpu_addr))
            continue;
        bool classified = false;
        for (size_t ri = 0; ri < p->data_ranges.count; ri++) {
            const DataRange *dr = &p->data_ranges.items[ri];
            if (dr->kind == DATA_SPRITE && dr->bank == bank && dr->addr == cpu_addr) {
                classified = true;
                break;
            }
        }
        UiState::SpriteScanEntry e;
        e.bank        = bank;
        e.cpu_addr    = cpu_addr;
        e.rom_offset  = off;
        e.header_type = htype;
        e.enc_type    = enc_type;
        e.width       = width;
        e.height      = height;
        e.consumed    = consumed;
        e.classified  = classified;
        s->sprite_candidates.push_back(e);
    }
    s->sprite_scan_done = true;
}

void render_sprite_list_window(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    const ApexRenderedDocument *d = *dp;
    size_t classified_count = 0;
    for (size_t i = 0; i < p->data_ranges.count; i++) {
        DataKind k = p->data_ranges.items[i].kind;
        if (k == DATA_SPRITE || k == DATA_SPRITE_NOHEADER)
            classified_count++;
    }

    if (ImGui::Button("Scan VSI Tables")) {
        scan_vsi_table_candidates(p, s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Scan ROM")) {
        scan_sprite_candidates(p, s);
    }
    ImGui::SameLine();
    {
        size_t vsi_count  = s->vsi_table_scan_done ? s->vsi_table_entries.size() : 0;
        size_t cand_count = 0;
        if (s->sprite_scan_done) {
            for (auto &e : s->sprite_candidates)
                if (!e.classified &&
                    e.width  >= s->sprite_filter_min_w && e.width  <= s->sprite_filter_max_w &&
                    e.height >= s->sprite_filter_min_h && e.height <= s->sprite_filter_max_h)
                    cand_count++;
        }
        ImGui::TextDisabled("(%zu classified, %zu vsi, %zu scan)",
                            classified_count, vsi_count, cand_count);
    }

    /* Filter controls */
    ImGui::SetNextItemWidth(60.0f); ImGui::InputInt("W min", &s->sprite_filter_min_w);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f); ImGui::InputInt("W max", &s->sprite_filter_max_w);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f); ImGui::InputInt("H min", &s->sprite_filter_min_h);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f); ImGui::InputInt("H max", &s->sprite_filter_max_h);
    s->sprite_filter_min_w = std::max(1, std::min(s->sprite_filter_min_w, 128));
    s->sprite_filter_max_w = std::max(s->sprite_filter_min_w, std::min(s->sprite_filter_max_w, 128));
    s->sprite_filter_min_h = std::max(1, std::min(s->sprite_filter_min_h, 32));
    s->sprite_filter_max_h = std::max(s->sprite_filter_min_h, std::min(s->sprite_filter_max_h, 32));

    /* --- VSI Sub-Table overview (shown when scan produced results) --- */
    if (s->vsi_table_scan_done && !s->vsi_sub_tables.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("VSI sub-tables — click Classify to apply all entries to config");

        if (ImGui::BeginTable("vsi_subtables", 6,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Tbl",   0, 30.0f);
            ImGui::TableSetupColumn("Address", 0, 110.0f);
            ImGui::TableSetupColumn("H",     0, 25.0f);
            ImGui::TableSetupColumn("Imgs",  0, 35.0f);
            ImGui::TableSetupColumn("Hdr",   0, 35.0f);
            ImGui::TableSetupColumn("",      0, 90.0f); /* Classify button */
            ImGui::TableHeadersRow();

            bool vsi_reclassified = false;
            for (auto &st : s->vsi_sub_tables) {
                int pass = 0, done = 0;
                for (auto &e : s->vsi_table_entries) {
                    if (e.table_idx != st.table_idx) continue;
                    if (e.width  < s->sprite_filter_min_w || e.width  > s->sprite_filter_max_w) continue;
                    if (e.height < s->sprite_filter_min_h || e.height > s->sprite_filter_max_h) continue;
                    pass++;
                    if (e.classified) done++;
                }

                ImGui::PushID(st.table_idx);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("T%d", st.table_idx);
                ImGui::TableSetColumnIndex(1);
                {
                    char addrstr[32];
                    snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x",
                             (unsigned)st.bank, (unsigned)st.cpu_addr & 0xffffu);
                    size_t li = 0;
                    bool found = apex_render_find_line_by_address(d, st.bank, st.cpu_addr, &li) != NULL;
                    if (ImGui::SmallButton(addrstr) && found)
                        select_line(s, li, 1);
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", (unsigned)st.table_height);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", st.num_images);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%zu", st.header_len);
                ImGui::TableSetColumnIndex(5);
                bool all_done = (done == pass && pass == st.num_images);
                if (all_done) {
                    ImGui::TextColored(ImVec4(0.47f, 0.86f, 1.0f, 1.0f), "classified");
                } else {
                    char btn[24];
                    snprintf(btn, sizeof(btn), "Classify##t%d", st.table_idx);
                    if (ImGui::SmallButton(btn)) {
                        int tidx = st.table_idx; /* copy before vsi_sub_tables is invalidated */
                        classify_vsi_table(p, dp, s, tidx);
                        d = *dp;
                        vsi_reclassified = true;
                    }
                    if (!vsi_reclassified && done > 0) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%d/%d", done, st.num_images);
                    }
                }
                ImGui::PopID();
                if (vsi_reclassified) break; /* vsi_sub_tables was rebuilt — must not iterate further */
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
    } else if (s->vsi_table_scan_done && s->vsi_sub_tables.empty()) {
        ImGui::TextDisabled("VSI table signature not found in this ROM.");
        ImGui::Separator();
    }

    if (ImGui::BeginTable("sprite_list", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,   110.0f);
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed,    60.0f);
        ImGui::TableSetupColumn("Kind",    ImGuiTableColumnFlags_WidthFixed,    90.0f);
        ImGui::TableSetupColumn("Tbl/Img", ImGuiTableColumnFlags_WidthFixed,    60.0f);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        int row_id = 0;

        /* --- VSI table entries (from "Scan VSI Tables") --- */
        if (s->vsi_table_scan_done) {
            for (auto &e : s->vsi_table_entries) {
                if (e.width  < s->sprite_filter_min_w || e.width  > s->sprite_filter_max_w) continue;
                if (e.height < s->sprite_filter_min_h || e.height > s->sprite_filter_max_h) continue;

                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x",
                         (unsigned)e.bank, (unsigned)e.cpu_addr & 0xffffu);
                std::string lbl = label_at_address(d, s, e.bank, e.cpu_addr);
                size_t li = 0;
                bool found = apex_render_find_line_by_address(d, e.bank, e.cpu_addr, &li) != NULL;
                bool sel = found && s->selected_line == li;

                ImGui::PushID(row_id++);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(addrstr, sel,
                        ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowOverlap)) {
                    if (found) select_line(s, li, 1);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem)) {
                    SpritePreviewInfo pr = {};
                    if (decode_sprite_preview_at(p, e.bank, e.cpu_addr, &pr)) {
                        ImGui::BeginTooltip();
                        render_sprite_preview(pr, 6.0f);
                        char info[64];
                        snprintf(info, sizeof(info),
                                 "T%d I%d  %ux%u  tbl_h=%u  %s",
                                 e.table_idx, e.image_idx,
                                 (unsigned)e.width, (unsigned)e.height,
                                 (unsigned)e.table_height,
                                 e.is_noheader ? "no-hdr" : "hdr");
                        ImGui::TextUnformatted(info);
                        ImGui::EndTooltip();
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%ux%u", (unsigned)e.width, (unsigned)e.height);
                ImGui::TableSetColumnIndex(2);
                if (e.classified)
                    ImGui::TextColored(ImVec4(0.47f, 0.86f, 1.00f, 1.0f),
                                       e.is_noheader ? "vsi_nh*" : "vsi*");
                else
                    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
                                       e.is_noheader ? "vsi_nh" : "vsi");
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("T%d I%d", e.table_idx, e.image_idx);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(lbl.c_str());
                ImGui::PopID();
            }
        } else if (s->vsi_table_entries.empty()) {
            /* no scan yet — show hint */
        }

        /* --- Classified sprites from data_ranges --- */
        for (size_t i = 0; i < p->data_ranges.count; i++) {
            const DataRange *dr = &p->data_ranges.items[i];
            if (dr->kind != DATA_SPRITE && dr->kind != DATA_SPRITE_NOHEADER) continue;

            char addrstr[32];
            snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x",
                     (unsigned)dr->bank, (unsigned)dr->addr & 0xffffu);
            std::string lbl = label_at_address(d, s, dr->bank, dr->addr);
            size_t li = 0;
            bool found = apex_render_find_line_by_address(d, dr->bank, dr->addr, &li) != NULL;
            bool sel = found && s->selected_line == li;

            SpritePreviewInfo pr = {};
            bool have_pr = decode_sprite_preview_at(p, dr->bank, dr->addr, &pr) != 0;

            ImGui::PushID(row_id++);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(addrstr, sel,
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap)) {
                if (found) select_line(s, li, 1);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem) && have_pr) {
                ImGui::BeginTooltip();
                render_sprite_preview(pr, 6.0f);
                ImGui::EndTooltip();
            }
            ImGui::TableSetColumnIndex(1);
            if (have_pr)
                ImGui::Text("%ux%u", (unsigned)pr.width, (unsigned)pr.height);
            else
                ImGui::TextDisabled("?");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(ImVec4(0.47f, 0.86f, 1.00f, 1.0f),
                               dr->kind == DATA_SPRITE_NOHEADER ? "spr_nh" : "sprite");
            ImGui::TableSetColumnIndex(3); /* Tbl/Img — not applicable */
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(lbl.c_str());
            ImGui::PopID();
        }

        /* --- ROM scan candidates (unclassified, dimension-filtered) --- */
        if (s->sprite_scan_done) {
            for (auto &e : s->sprite_candidates) {
                if (e.classified) continue;
                if (e.width  < s->sprite_filter_min_w || e.width  > s->sprite_filter_max_w) continue;
                if (e.height < s->sprite_filter_min_h || e.height > s->sprite_filter_max_h) continue;

                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x",
                         (unsigned)e.bank, (unsigned)e.cpu_addr & 0xffffu);
                std::string lbl = label_at_address(d, s, e.bank, e.cpu_addr);
                size_t li = 0;
                bool found = apex_render_find_line_by_address(d, e.bank, e.cpu_addr, &li) != NULL;
                bool sel = found && s->selected_line == li;

                ImGui::PushID(row_id++);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(addrstr, sel,
                        ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowOverlap)) {
                    if (found) select_line(s, li, 1);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem)) {
                    SpritePreviewInfo pr = {};
                    if (decode_sprite_preview_at(p, e.bank, e.cpu_addr, &pr)) {
                        ImGui::BeginTooltip();
                        render_sprite_preview(pr, 6.0f);
                        ImGui::EndTooltip();
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%ux%u", (unsigned)e.width, (unsigned)e.height);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("scan");
                ImGui::TableSetColumnIndex(3); /* Tbl/Img — not applicable */
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(lbl.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}
