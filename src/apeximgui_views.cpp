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

static void render_dmd_preview(const DmdPreviewInfo &preview)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale = std::max(1.0f, std::min(6.0f, avail.x / (float)APEX_DMD_WIDTH));
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

// --- Public Window Rendering ---

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
    if (ImGui::BeginTable("disasm", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(2, 1);
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,   120.0f);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed,    60.0f);
        ImGui::TableSetupColumn("Text",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int)visible.size());
        if (state->request_scroll_to_selection && selected_visible_row >= 0) {
            clipper.IncludeItemByIndex(selected_visible_row);
        }
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
                ImGui::TableSetColumnIndex(0);
                char addr_buf[32];
                const char *addr_text = "##line";
                if (line->has_location) {
                    snprintf(addr_buf, 32, "B%02x_A%04x", line->bank,
                             (unsigned)line->cpu_addr & 0xffffu);
                    addr_text = addr_buf;
                }
                if (ImGui::Selectable(addr_text, in_range,
                        ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowOverlap)) {
                    handle_line_selection(state, line_idx, ImGui::GetIO().KeyShift);
                }
                bool row_double_clicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
                bool row_right_clicked  = ImGui::IsItemHovered() && ImGui::IsMouseClicked(1);
                if (row_right_clicked) {
                    if (!in_range) {
                        select_line(state, line_idx, 0);
                    }
                    ImGui::OpenPopup("row_context_menu");
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(block_name(line->block_kind));
                ImGui::TableSetColumnIndex(2);
                if (line->transition_kind != APEX_RENDER_TRANSITION_NONE) {
                    ImGui::TextDisabled("%s", transition_name(line->transition_kind));
                    ImGui::SameLine();
                }
                ImGui::BeginGroup();
                render_line_text(document, state, line);
                ImGui::EndGroup();

                uint8_t t_bank;
                uint32_t t_addr;
                int t_far;
                bool has_pointer = resolve_pointer_target(project, line, &t_bank, &t_addr, &t_far);
                if (ImGui::IsItemHovered()) {
                    row_double_clicked |= ImGui::IsMouseDoubleClicked(0);
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
                    ImGui::EndTooltip();
                }
                if (ImGui::BeginPopup("row_context_menu")) {
                    if (has_pointer && ImGui::MenuItem("Jump to target", "F / Enter")) {
                        size_t tl;
                        if (apex_render_find_line_by_address(document, t_bank, t_addr, &tl)) {
                            select_line(state, tl, 1);
                        }
                    }
                    if (line->has_location && ImGui::MenuItem("Show incoming references", "X")) {
                        state->request_xref_popup = true;
                        state->xref_popup_bank = line->bank;
                        state->xref_popup_addr = line->cpu_addr;
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
                    if (ImGui::MenuItem("Clear Classification", "X")) {
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
                    if (line->has_location && ImGui::MenuItem("Add Bookmark", "B")) {
                        char n[64];
                        snprintf(n, 64, "Bookmark @ B%02x_%04x", line->bank, line->cpu_addr);
                        state->bookmarks.push_back({line->bank, line->cpu_addr, n});
                        state->request_focus_new_bookmark = 1;
                        set_status(state, "bookmark added");
                    }
                    ImGui::EndPopup();
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
        }
        ImGui::EndTable();
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
        ImVec4(0.50f, 0.50f, 0.50f, 1.0f), /* UNKNOWN  — gray    */
        ImVec4(0.40f, 0.90f, 0.40f, 1.0f), /* CODE     — green   */
        ImVec4(0.45f, 0.70f, 1.00f, 1.0f), /* DATA     — blue    */
        ImVec4(0.95f, 0.65f, 0.20f, 1.0f), /* TABLE    — orange  */
    };

    const int bytes_per_row = 16;
    const int total_rows = (int)((p->rom.size + (size_t)(bytes_per_row - 1)) / (size_t)bytes_per_row);
    const float row_h   = ImGui::GetTextLineHeightWithSpacing();
    const float char_w  = ImGui::CalcTextSize("0").x;
    /* Layout: "000000: " = 8 chars, then 16 × "xx " (3 chars each), 2-char gap, 16 ASCII chars */
    const float hex_x0  = char_w * 8.5f;
    const float gap_w   = char_w * 2.0f;
    const float asc_x   = hex_x0 + (float)bytes_per_row * char_w * 3.0f + gap_w;

    /* Inspector strip at bottom: one line with address/value info. */
    const float inspector_h = row_h + ImGui::GetStyle().ItemSpacing.y * 2.0f +
                              ImGui::GetStyle().SeparatorTextBorderSize;

    ImGui::BeginChild("hex_grid", ImVec2(0.0f, -inspector_h), false);

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

        /* Precompute block-kind for every byte in the visible range with a single
           forward pass through the (rom-address-ordered) document lines. */
        size_t vis_count = vis_end - vis_start;
        std::vector<uint8_t> kinds(vis_count, (uint8_t)APEX_RENDER_BLOCK_UNKNOWN);
        {
            /* Find the last document line whose rom_addr <= vis_start. */
            size_t li = 0;
            ApexRenderedBlockKind cur_kind = APEX_RENDER_BLOCK_UNKNOWN;
            while (li < d->line_count) {
                if (d->lines[li].has_location && d->lines[li].rom_addr <= vis_start) {
                    cur_kind = d->lines[li].block_kind;
                }
                if (d->lines[li].has_location && d->lines[li].rom_addr > vis_start) {
                    break;
                }
                li++;
            }
            /* Fill kinds: walk forward, filling spans between consecutive located lines.
               Non-located lines (comments, section headers) carry no ROM bytes and must
               be skipped — otherwise they'd prematurely terminate the fill at vis_end. */
            size_t fill = vis_start;
            for (; li <= d->line_count; li++) {
                if (li < d->line_count && !d->lines[li].has_location) {
                    continue;
                }
                size_t boundary = vis_end;
                if (li < d->line_count) {
                    boundary = std::min(vis_end, d->lines[li].rom_addr);
                }
                for (size_t o = fill; o < boundary; o++) {
                    kinds[o - vis_start] = (uint8_t)cur_kind;
                }
                fill = boundary;
                if (fill >= vis_end) {
                    break;
                }
                if (li < d->line_count) {
                    cur_kind = d->lines[li].block_kind;
                }
            }
        }

        for (int row_idx = clipper.DisplayStart; row_idx < clipper.DisplayEnd; row_idx++) {
            size_t row_start = (size_t)row_idx * (size_t)bytes_per_row;

            /* Address label — buf needs 8 hex digits + ':' + '\0' = 10, use 24 for safety */
            char addr_buf[24];
            snprintf(addr_buf, sizeof(addr_buf), "%06lx:", (unsigned long)row_start);
            ImGui::TextUnformatted(addr_buf);

            /* Hex bytes */
            for (int col = 0; col < bytes_per_row; col++) {
                size_t o = row_start + (size_t)col;
                if (o >= p->rom.size) {
                    break;
                }
                uint8_t v   = p->rom.data[o];
                bool is_cur = s->hex_active && o == s->hex_selected_offset;
                uint8_t ki  = (o >= vis_start && o < vis_end) ? kinds[o - vis_start] : 0u;
                const ImVec4 &tc = kind_colors[ki < 4u ? ki : 0u];

                ImGui::SameLine(hex_x0 + (float)col * char_w * 3.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, tc);
                char hbuf[3];
                snprintf(hbuf, sizeof(hbuf), "%02x", v);
                ImGui::TextUnformatted(hbuf);
                ImGui::PopStyleColor();

                if (is_cur) {
                    ImVec2 rmin = ImGui::GetItemRectMin();
                    ImVec2 rmax = ImGui::GetItemRectMax();
                    rmax.x += char_w * 0.15f;
                    dl->AddRect(rmin, rmax, IM_COL32(255, 220, 0, 230), 0.0f, 0, 1.5f);
                }

                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    uint8_t bank;
                    uint32_t cpu_addr;
                    if (rom_offset_to_cpu_address(p, o, &bank, &cpu_addr)) {
                        ImGui::Text("B%02x:A%04x  ROM:0x%06lx", bank, cpu_addr, (unsigned long)o);
                    } else {
                        ImGui::Text("ROM:0x%06lx", (unsigned long)o);
                    }
                    ImGui::Text("$%02X  %u  '%c'", v, v, (v >= 32 && v <= 126) ? (char)v : '.');
                    if (o + 1 < p->rom.size) {
                        ImGui::Text("BE16:$%04X", ((uint16_t)v << 8) | p->rom.data[o + 1]);
                    }
                    ImGui::EndTooltip();
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    s->hex_selected_offset = o;
                    s->hex_active = true;
                    size_t li;
                    if (find_line_by_rom_offset(d, o, &li)) {
                        select_line(s, li, 1);
                    }
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    s->hex_selected_offset = o;
                    s->hex_active = true;
                    size_t li;
                    if (find_line_by_rom_offset(d, o, &li)) {
                        select_line(s, li, 1);
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
                uint8_t v   = p->rom.data[o];
                bool is_cur = s->hex_active && o == s->hex_selected_offset;
                uint8_t ki  = (o >= vis_start && o < vis_end) ? kinds[o - vis_start] : 0u;
                const ImVec4 &tc = kind_colors[ki < 4u ? ki : 0u];

                ImGui::SameLine(asc_x + (float)col * char_w);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    is_cur ? ImVec4(1.0f, 0.86f, 0.0f, 1.0f) : tc);
                char ch[2] = {(v >= 32 && v <= 126) ? (char)v : '.', '\0'};
                ImGui::TextUnformatted(ch);
                ImGui::PopStyleColor();

                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    s->hex_selected_offset = o;
                    s->hex_active = true;
                    size_t li;
                    if (find_line_by_rom_offset(d, o, &li)) {
                        select_line(s, li, 1);
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
        if (ImGui::MenuItem("Mark Code"))   { apply_code_at_selection(p, dp, s); }
        if (ImGui::MenuItem("Mark Data"))   { apply_data_at_selection(p, dp, s, "bytes[1]"); }
        if (ImGui::MenuItem("Mark String")) { apply_string_at_selection(p, dp, s); }
        if (ImGui::MenuItem("Mark Table"))  { apply_table_at_selection(p, dp, s, "counted(ptr16_data)"); }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear Kind"))  { clear_kind_at_selection(p, dp, s); }
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
        if (hov && ImGui::IsMouseDoubleClicked(0)) {
            size_t li;
            if (apex_render_find_line_by_address(d, n.bank, n.addr, &li)) {
                select_line(s, li, 1);
                s->graph_needs_rebuild = true;
            }
        }
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
        { TABLE_BYTE,         "byte"        },
        { TABLE_WORD,         "word"        },
        { TABLE_PTR16_STRING, "ptr16_string"},
        { TABLE_PTR16_DATA,   "ptr16_data"  },
        { TABLE_PTR16_CODE,   "ptr16_code"  },
    };
    /* Row 2: far pointer kinds */
    static const struct { int kind; const char *label; } kRow2[] = {
        { TABLE_FAR_STRING,   "far_string"  },
        { TABLE_FAR_DATA,     "far_data"    },
        { TABLE_FAR_TABLE,    "far_table"   },
        { TABLE_FAR_CODE,     "far_code"    },
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
    /* named types from config, appended to row 2 */
    if (p) {
        for (size_t ti = 0; ti < p->config_types.count; ti++) {
            ImGui::SameLine();
            const ConfigType *ct = &p->config_types.items[ti];
            if (ImGui::SmallButton(ct->name)) {
                if (*count < APEX_MAX_EDIT_FIELDS) {
                    ApexEditField f = {};
                    f.kind  = -1;
                    f.count = add_count > 0 ? add_count : 1;
                    snprintf(f.type_name, sizeof(f.type_name), "%s", ct->name);
                    fields[(*count)++] = f;
                    changed = true;
                }
            }
            if (ImGui::IsItemHovered() && ct->value_count > 0) {
                ImGui::BeginTooltip();
                for (size_t vi = 0; vi < ct->value_count; vi++) {
                    ImGui::Text("0x%02x = %s", ct->values[vi].value, ct->values[vi].name);
                }
                ImGui::EndTooltip();
            }
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
                { TABLE_FAR_STRING,        "far_string"    },
                { TABLE_FAR_DATA,          "far_data"      },
                { TABLE_FAR_TABLE,         "far_table"     },
                { TABLE_FAR_CODE,          "far_code"      },
                { TABLE_FAR_DMD_FULLFRAME, "far_dmd"       },
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

    /* ── Classification ─────────────────────────────────────────── */
    ImGui::SeparatorText("Classification");
    if (ImGui::Button("Code")) {
        apply_code_at_selection(p, dp, s);
    }
    ImGui::SameLine();
    if (ImGui::Button("String")) {
        apply_string_at_selection(p, dp, s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Kind")) {
        clear_kind_at_selection(p, dp, s);
    }

    /* ── Data ───────────────────────────────────────────────────── */
    ImGui::SeparatorText("Data");
    /* row 1: bytes[N] with N spinner */
    ImGui::SetNextItemWidth(50);
    if (ImGui::InputInt("##datalen", &s->edit_data_length, 1, 8)) {
        if (s->edit_data_length < 1) s->edit_data_length = 1;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("byte count for bytes[N]");
    ImGui::SameLine();
    if (ImGui::Button("bytes[N]##data")) {
        char spec[32];
        snprintf(spec, sizeof(spec), "bytes[%d]", s->edit_data_length);
        apply_data_at_selection(p, dp, s, spec);
    }
    /* row 2: far pointer shortcuts */
    if (ImGui::Button("far_code##data"))   { apply_data_at_selection(p, dp, s, "far_code");     }
    ImGui::SameLine();
    if (ImGui::Button("far_data##data"))   { apply_data_at_selection(p, dp, s, "far_data");     }
    ImGui::SameLine();
    if (ImGui::Button("far_string##data")) { apply_data_at_selection(p, dp, s, "far_string");   }
    ImGui::SameLine();
    if (ImGui::Button("far_table##data"))  { apply_data_at_selection(p, dp, s, "far_table");    }
    ImGui::SameLine();
    if (ImGui::Button("dmd##data"))        { apply_data_at_selection(p, dp, s, "dmd_fullframe"); }

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
        ImGui::SetNextItemWidth(60);
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
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5));
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
                apex_project_clear_kind(p, 1, t->bank, t->addr);
                rerender_and_reselect(p, dp, s, t->bank, t->addr);
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
                bool match = strcasestr(addrstr, filter) ||
                             strcasestr(spec.c_str(), filter) ||
                             (!lbl.empty() && strcasestr(lbl.c_str(), filter));
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
                bool match = strcasestr(addrstr, filter) ||
                             (!lbl.empty() && strcasestr(lbl.c_str(), filter));
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
