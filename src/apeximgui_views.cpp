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
    if (!start || !end || end <= start) return;
    if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
    ImGui::TextUnformatted(start, end);
    if (color) ImGui::PopStyleColor();
}

static void render_line_text(const ApexRenderedDocument *document, UiState *state, const ApexRenderedLine *line)
{
    static const ImVec4 label_color = ImVec4(0.95f, 0.82f, 0.45f, 1.0f);
    static const ImVec4 target_color = ImVec4(0.45f, 0.80f, 0.95f, 1.0f);
    if (line->kind == APEX_RENDER_LINE_LABEL) { render_text_chunk(line->text, line->text + line->length, &label_color); return; }
    auto targets = find_line_targets(document, state, line);
    if (targets.empty()) { render_text_chunk(line->text, line->text + line->length, NULL); return; }
    const char *cursor = line->text;
    for (auto &target : targets) {
        const char *m_start = line->text + target.match_pos, *m_end = m_start + target.name.size();
        if (m_start < cursor) continue;
        if (cursor < m_start) { render_text_chunk(cursor, m_start, NULL); ImGui::SameLine(0, 0); }
        render_text_chunk(m_start, m_end, &target_color); cursor = m_end;
        if (cursor < line->text + line->length) ImGui::SameLine(0, 0);
    }
    if (cursor < line->text + line->length) render_text_chunk(cursor, line->text + line->length, NULL);
}

static void render_dmd_preview(const DmdPreviewInfo &preview)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale = std::max(1.0f, std::min(6.0f, avail.x / (float)APEX_DMD_WIDTH));
    ImGui::TextUnformatted(preview.title);
    ImGui::Text("Address: B%02x_A%04x  ROM: 0x%06lx", preview.bank, (unsigned)preview.cpu_addr & 0xffffu, (unsigned long)preview.rom_offset);
    ImGui::Text("Decoder: 0x%02x  Consumed: %lu  Size: %ux%u", (unsigned)preview.decoder_type, (unsigned long)preview.consumed, (unsigned)APEX_DMD_WIDTH, (unsigned)APEX_DMD_HEIGHT);
    ImGui::Separator();
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("dmd_canvas", ImVec2(APEX_DMD_WIDTH * scale, APEX_DMD_HEIGHT * scale));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + APEX_DMD_WIDTH * scale, canvas_pos.y + APEX_DMD_HEIGHT * scale), IM_COL32(8, 8, 8, 255));
    for (size_t row = 0; row < APEX_DMD_HEIGHT; row++) {
        for (size_t col_byte = 0; col_byte < APEX_DMD_ROW_BYTES; col_byte++) {
            uint8_t bits = preview.plane[row * APEX_DMD_ROW_BYTES + col_byte];
            for (size_t bit = 0; bit < 8u; bit++) {
                bool lit = ((bits >> bit) & 1u) != 0u;
                ImVec2 p0(canvas_pos.x + (col_byte * 8 + bit) * scale, canvas_pos.y + row * scale);
                draw->AddRectFilled(p0, ImVec2(p0.x + scale - 1.0f, p0.y + scale - 1.0f), lit ? IM_COL32(255, 160, 40, 255) : IM_COL32(28, 18, 6, 255));
            }
        }
    }
}

// --- Public Window Rendering ---

void render_line_table(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;
    std::vector<size_t> visible; int selected_visible_row = -1;
    ensure_label_index(document, state);
    visible.reserve(document->line_count);
    for (size_t i = 0; i < document->line_count; i++) {
        if (line_matches_filter(&document->lines[i], state->filter_input)) {
            if (state->selected_line == i) selected_visible_row = (int)visible.size();
            visible.push_back(i);
        }
    }
    if (ImGui::BeginTable("disasm", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(2, 1);
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin((int)visible.size());
        if (state->request_scroll_to_selection && selected_visible_row >= 0) clipper.IncludeItemByIndex(selected_visible_row);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                size_t line_idx = visible[(size_t)row]; const auto *line = &document->lines[line_idx];
                size_t sel_min = std::min(state->selected_line, state->selection_end), sel_max = std::max(state->selected_line, state->selection_end);
                bool in_range = (line_idx >= sel_min && line_idx <= sel_max);
                ImGui::PushID((int)line_idx); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                char addr_buf[32]; const char *addr_text = "##line";
                if (line->has_location) { snprintf(addr_buf, 32, "B%02x_A%04x", line->bank, (unsigned)line->cpu_addr & 0xffffu); addr_text = addr_buf; }
                if (ImGui::Selectable(addr_text, in_range, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) handle_line_selection(state, line_idx, ImGui::GetIO().KeyShift);
                bool row_double_clicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0), row_right_clicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(1);
                if (row_right_clicked) { if (!in_range) select_line(state, line_idx, 0); ImGui::OpenPopup("row_context_menu"); }
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(block_name(line->block_kind));
                ImGui::TableSetColumnIndex(2); if (line->transition_kind != APEX_RENDER_TRANSITION_NONE) { ImGui::TextDisabled("%s", transition_name(line->transition_kind)); ImGui::SameLine(); }
                ImGui::BeginGroup(); render_line_text(document, state, line); ImGui::EndGroup();

                uint8_t t_bank; uint32_t t_addr; int t_far; bool has_pointer = resolve_pointer_target(project, line, &t_bank, &t_addr, &t_far);
                if (ImGui::IsItemHovered()) {
                    row_double_clicked |= ImGui::IsMouseDoubleClicked(0);
                    ImGui::BeginTooltip();
                    if (line->kind == APEX_RENDER_LINE_INSTRUCTION) {
                        char mn[16]; size_t k = 0; const char *p = line->text; while (k < line->length && isspace((unsigned char)*p)) p++;
                        while (k < 15 && (p+k) < (line->text+line->length) && !isspace((unsigned char)p[k])) { mn[k] = p[k]; k++; } mn[k] = 0;
                        const auto *h = lookup_cpu_help(mn); if (h) { ImGui::Text("Instruction: %s", h->mnemonic); ImGui::Separator(); ImGui::Text("%s", h->desc); ImGui::Text("Flags: %s", h->flags); ImGui::Text("Cycles: %s", h->cycles); }
                    }
                    auto hw = find_hardware_in_text(line->text, line->length); if (!hw.empty()) { ImGui::Separator(); ImGui::TextUnformatted("Hardware:"); for (auto h : hw) ImGui::BulletText("%s ($%04X): %s", h->name, h->addr, h->desc); }
                    if (has_pointer) { ImGui::Separator(); std::string l = label_at_address(document, state, t_bank, t_addr); if (!l.empty()) ImGui::Text("Jump: %s (B%02x_A%04x)", l.c_str(), t_bank, (unsigned)t_addr & 0xffffu); else ImGui::Text("Jump: B%02x_A%04x", t_bank, (unsigned)t_addr & 0xffffu); }
                    if (line->has_location) { auto in_refs = find_incoming_refs(project, document, state, line->bank, line->cpu_addr); if (!in_refs.empty()) { ImGui::Separator(); ImGui::Text("Called by (%lu):", (unsigned long)in_refs.size()); for (size_t ri = 0; ri < in_refs.size() && ri < 5; ri++) ImGui::BulletText("%s %s", in_refs[ri].label.empty() ? "-" : in_refs[ri].label.c_str(), in_refs[ri].kind.c_str()); } }
                    ImGui::EndTooltip();
                }
                if (ImGui::BeginPopup("row_context_menu")) {
                    if (has_pointer && ImGui::MenuItem("Jump to target", "F / Enter")) { size_t tl; if (apex_render_find_line_by_address(document, t_bank, t_addr, &tl)) select_line(state, tl, 1); }
                    if (line->has_location && ImGui::MenuItem("Show incoming references", "X")) { state->request_xref_popup = true; state->xref_popup_bank = line->bank; state->xref_popup_addr = line->cpu_addr; }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Copy selection", "Ctrl+C")) { copy_selection_to_clipboard(document, state); set_status(state, "copied"); }
                    if (ImGui::MenuItem("Mark as Code", "C")) apply_code_at_selection(project, document_ptr, state);
                    if (ImGui::MenuItem("Mark as Data", "D")) apply_data_at_selection(project, document_ptr, state, state->edit_spec_input[0] ? state->edit_spec_input : "bytes[1]");
                    if (ImGui::MenuItem("Mark as String", "S")) apply_string_at_selection(project, document_ptr, state);
                    if (ImGui::MenuItem("Mark as Table", "T")) apply_table_at_selection(project, document_ptr, state, state->edit_spec_input[0] ? state->edit_spec_input : "counted(ptr16_data)");
                    if (ImGui::MenuItem("Clear Classification", "X")) clear_kind_at_selection(project, document_ptr, state);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Edit Label", "L")) state->request_focus_label = 1;
                    if (ImGui::MenuItem("Edit Comment", "Shift+D")) state->request_focus_doc = 1;
                    ImGui::Separator();
                    if (line->has_location && ImGui::MenuItem("Add Bookmark", "B")) { char n[64]; snprintf(n, 64, "Bookmark @ B%02x_%04x", line->bank, line->cpu_addr); state->bookmarks.push_back({line->bank, line->cpu_addr, n}); state->request_focus_new_bookmark = 1; set_status(state, "bookmark added"); }
                    ImGui::EndPopup();
                }
                if (row_double_clicked) { size_t tl; if (has_pointer && apex_render_find_line_by_address(document, t_bank, t_addr, &tl)) select_line(state, tl, 1); else jump_to_first_line_target(document, state, line); }
                if (line_idx == state->selected_line && state->request_scroll_to_selection) { ImGui::SetScrollHereY(0.35f); state->request_scroll_to_selection = 0; }
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
    std::vector<size_t> visible; for (size_t i = 0; i < state->cached_labels.size(); i++) if (label_entry_matches_filter(state->cached_labels[i], state->label_filter_input)) visible.push_back(i);
    if (ImGui::BeginTable("labels", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 120.0f); ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed, 60.0f); ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch); ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin((int)visible.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const auto &e = state->cached_labels[visible[row]]; char a[32]; snprintf(a, 32, "B%02x_A%04x", e.bank, (unsigned)e.cpu_addr & 0xffffu);
                ImGui::PushID((int)e.line_index); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(a, state->selected_line == e.line_index, ImGuiSelectableFlags_SpanAllColumns)) select_line(state, e.line_index, 1);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(block_name(e.block_kind));
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(e.name.c_str()); ImGui::PopID();
            }
        } ImGui::EndTable();
    }
}

void render_bank_list(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    uint8_t cur_b = 0; bool has_sel = false; if (s->selected_line < d->line_count && d->lines[s->selected_line].has_location) { cur_b = d->lines[s->selected_line].bank; has_sel = true; }
    if (ImGui::BeginTable("banks", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Bank ID", ImGuiTableColumnFlags_WidthStretch); ImGui::TableHeadersRow();
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        if (ImGui::Selectable("System (Prime)", has_sel && cur_b == 0xffu)) { size_t li; if (find_first_line_in_bank(d, 0xffu, &li)) select_line(s, li, 0); }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { size_t li; if (find_first_line_in_bank(d, 0xffu, &li)) select_line(s, li, 1); }
        for (size_t i = 0; i < p->banks; i++) {
            uint8_t bid = bank_id_for_index(p->rom.data, (int)i); char lbl[64]; snprintf(lbl, 64, "Bank 0x%02x", bid);
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(lbl, has_sel && cur_b == bid)) { size_t li; if (find_first_line_in_bank(d, bid, &li)) select_line(s, li, 0); }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { size_t li; if (find_first_line_in_bank(d, bid, &li)) select_line(s, li, 1); }
        } ImGui::EndTable();
    }
}

void render_transition_list(const ApexRenderedDocument *d, UiState *s)
{
    for (size_t i = 0; i < d->line_count; i++) {
        const auto *l = &d->lines[i]; if (l->transition_kind == APEX_RENDER_TRANSITION_NONE || !l->has_location || !line_matches_filter(l, s->filter_input)) continue;
        char lbl[128]; snprintf(lbl, 128, "%s @ B%02x_A%04x", transition_name(l->transition_kind), l->bank, (unsigned)l->cpu_addr & 0xffffu);
        if (ImGui::Selectable(lbl, s->selected_line == i)) select_line(s, i, 1);
    }
}

void render_xref_popup(ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    if (s->request_xref_popup) { ImGui::OpenPopup("XRefs"); s->request_xref_popup = false; }
    if (ImGui::BeginPopup("XRefs")) {
        auto in = find_incoming_refs(p, d, s, s->xref_popup_bank, s->xref_popup_addr);
        ImGui::Text("Refs to B%02x_A%04x", s->xref_popup_bank, s->xref_popup_addr); ImGui::Separator();
        if (in.empty()) ImGui::TextDisabled("None.");
        else if (ImGui::BeginTable("xref_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(400, 300))) {
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f); ImGui::TableHeadersRow();
            for (auto &r : in) {
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); char lbl[128];
                if (!r.label.empty()) snprintf(lbl, 128, "%s (B%02x_A%04x)", r.label.c_str(), r.bank, (unsigned)r.cpu_addr & 0xffffu);
                else snprintf(lbl, 128, "B%02x_A%04x", r.bank, (unsigned)r.cpu_addr & 0xffffu);
                if (ImGui::Selectable(lbl, false, ImGuiSelectableFlags_SpanAllColumns)) { select_line(s, r.line_index, 1); ImGui::CloseCurrentPopup(); }
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r.kind.c_str());
            } ImGui::EndTable();
        }
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void render_bookmark_list(const ApexRenderedDocument *d, UiState *s)
{
    if (ImGui::Button("Add Bookmark")) { uint8_t b; uint32_t a; if (selected_address(d, s, &b, &a)) { char n[64]; snprintf(n, 64, "Bookmark @ B%02x_%04x", b, a); s->bookmarks.push_back({b, a, n}); s->request_focus_new_bookmark = 1; } }
    if (ImGui::BeginTable("bookmarks", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 100.0f); ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 60.0f); ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin((int)s->bookmarks.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                auto &bm = s->bookmarks[row]; ImGui::PushID(row); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                char a[32]; snprintf(a, 32, "B%02x_A%04x", bm.bank, bm.addr);
                if (ImGui::Selectable(a, false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) { size_t li; if (apex_render_find_line_by_address(d, bm.bank, bm.addr, &li)) select_line(s, li, 1); }
                ImGui::TableSetColumnIndex(1); char nb[256]; strcpy(nb, bm.name.c_str());
                if ((size_t)row == s->bookmarks.size() - 1 && s->request_focus_new_bookmark) { ImGui::SetKeyboardFocusHere(); s->request_focus_new_bookmark = 0; }
                if (ImGui::InputText("##name", nb, 256)) bm.name = nb;
                ImGui::TableSetColumnIndex(2); if (ImGui::SmallButton("Del")) s->bookmarks.erase(s->bookmarks.begin() + row); ImGui::PopID();
            }
        } ImGui::EndTable();
    }
}

void render_global_search(const ApexRenderedDocument *d, UiState *s)
{
    if (s->request_focus_global_search) { ImGui::SetKeyboardFocusHere(); s->request_focus_global_search = 0; }
    if (ImGui::InputText("Query", s->global_search_input, 128)) { s->search_results.clear(); if (s->global_search_input[0]) for (size_t i = 0; i < d->line_count; i++) if (strcasestr(d->lines[i].text, s->global_search_input)) s->search_results.push_back(i); }
    if (ImGui::BeginTable("search_results", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 100.0f); ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed, 60.0f); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch); ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin((int)s->search_results.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                size_t li = s->search_results[row]; const auto *l = &d->lines[li]; ImGui::PushID((int)li); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                char a[32]; if (l->has_location) snprintf(a, 32, "B%02x_A%04x", l->bank, (unsigned)l->cpu_addr & 0xffffu); else strcpy(a, "-");
                if (ImGui::Selectable(a, s->selected_line == li, ImGuiSelectableFlags_SpanAllColumns)) select_line(s, li, 1);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(block_name(l->block_kind));
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(l->text, l->text + l->length); ImGui::PopID();
            }
        } ImGui::EndTable();
    }
}

void render_hex_view(const ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    const ApexRenderedDocument *d = *dp;
    auto sp = selected_line_span(p, d, s); 
    size_t focus = sp.valid ? sp.start : (s->hex_active ? s->hex_selected_offset : 0);
    size_t st = (focus >= 64) ? focus - 64 : 0;
    size_t en = std::min(p->rom.size, st + 256);
    
    float inspector_width = 300.0f;
    ImGui::Columns(2, "hex_layout", true);
    ImGui::SetColumnWidth(1, inspector_width);

    ImGui::BeginChild("hexscroll");
    for (size_t row = st; row < en; row += 16) {
        char a[32]; snprintf(a, 32, "%06lx:", (unsigned long)row); ImGui::TextUnformatted(a); ImGui::SameLine(90);
        for (size_t col = 0; col < 16 && row + col < en; col++) {
            size_t o = row + col; 
            bool line_sel = sp.valid && o >= sp.start && o < sp.end; 
            bool hex_sel = s->hex_active && o == s->hex_selected_offset; 
            uint8_t v = p->rom.data[o];
            
            if (col > 0) ImGui::SameLine(); 
            if (hex_sel) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
            else if (line_sel) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 120, 255));
            
            ImGui::Text("%02x", v); 
            
            if (hex_sel || line_sel) ImGui::PopStyleColor();
            
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip(); 
                ImGui::Text("ROM: 0x%06lx", (unsigned long)o); ImGui::Separator();
                ImGui::Text("Hex: $%02X  Dec: %u", v, v); 
                char bin[10]; for (int b = 0; b < 8; b++) bin[7-b] = ((v >> b) & 1) ? '1' : '0'; bin[8] = 0;
                ImGui::Text("Bin: %%%s  Char: '%c'", bin, (v >= 32 && v <= 126) ? (char)v : '.');
                if (o+1 < p->rom.size) ImGui::Text("Word: $%04X", ((uint16_t)v << 8) | p->rom.data[o+1]); 
                ImGui::EndTooltip();
            }
            if (ImGui::IsItemClicked()) { 
                s->hex_selected_offset = o; s->hex_active = true;
                size_t li; if (find_line_by_rom_offset(d, o, &li)) select_line(s, li, 0); 
            }
        }
        ImGui::SameLine(520);
        for (size_t col = 0; col < 16 && row + col < en; col++) {
            size_t o = row + col; 
            uint8_t v = p->rom.data[o]; 
            bool line_sel = sp.valid && o >= sp.start && o < sp.end; 
            bool hex_sel = s->hex_active && o == s->hex_selected_offset;
            
            if (col > 0) ImGui::SameLine(); 
            if (hex_sel) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
            else if (line_sel) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 120, 255));
            
            char ch[2] = {(char)((v >= 32 && v <= 126) ? v : '.'), 0}; 
            ImGui::TextUnformatted(ch); 
            
            if (hex_sel || line_sel) ImGui::PopStyleColor();
            
            if (ImGui::IsItemClicked()) { 
                s->hex_selected_offset = o; s->hex_active = true;
                size_t li; if (find_line_by_rom_offset(d, o, &li)) select_line(s, li, 0); 
            }
        }
    } ImGui::EndChild();

    ImGui::NextColumn();
    ImGui::BeginChild("inspector");
    if (s->hex_active) {
        size_t o = s->hex_selected_offset; uint8_t v = p->rom.data[o];
        uint8_t bank; uint32_t cpu_addr;
        ImGui::TextUnformatted("Hex Inspector"); ImGui::Separator();
        ImGui::Text("ROM Offset: 0x%06lx", (unsigned long)o);
        if (rom_offset_to_cpu_address(p, o, &bank, &cpu_addr)) ImGui::Text("CPU Address: B%02x:A%04x", bank, cpu_addr);
        else ImGui::Text("CPU Address: -");
        ImGui::Separator();
        
        ImGui::Text("Value (Dec): %u", v);
        ImGui::Text("Value (Hex): $%02X", v);

        ImGui::Text("Binary:");
        for (int i = 7; i >= 0; i--) {
            bool bit = (v >> i) & 1;
            ImGui::Text("%d:%s", i, bit ? "1" : "0");
            if (i > 0) ImGui::SameLine();
        }
        
        ImGui::Separator();
        if (o + 1 < p->rom.size) {
            uint16_t word = ((uint16_t)v << 8) | p->rom.data[o+1];
            ImGui::Text("Word (BE): 0x%04X (%u)", word, word);
        }
        
        char ash[2] = {(char)((v >= 32 && v <= 126) ? v : '.'), 0};
        ImGui::Text("ASCII: '%s'", ash);
        
        ImGui::Separator();
        if (ImGui::Button("Close Inspector")) s->hex_active = false;
    } else {
        ImGui::TextDisabled("Select a byte in Hex View to inspect.");
    }
    ImGui::EndChild();
    ImGui::Columns(1);
}

void render_call_graph(ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    if (s->graph_needs_rebuild) rebuild_call_graph(p, d, s);
    ImGui::SliderInt("In Depth", &s->graph_depth_in, 1, 4); ImGui::SameLine(); ImGui::SliderInt("Out Depth", &s->graph_depth_out, 1, 4);
    if (ImGui::Button("Rebuild Graph")) s->graph_needs_rebuild = true;
    ImVec2 cp0 = ImGui::GetCursorScreenPos(), csz = ImGui::GetContentRegionAvail(); if (csz.x < 50) csz.x = 50; if (csz.y < 50) csz.y = 50;
    ImGui::InvisibleButton("canvas", csz); if (s->graph_nodes.empty()) return;
    std::map<int, std::vector<size_t>> lys; int min_l = 0, max_l = 0;
    for (size_t i = 0; i < s->graph_nodes.size(); i++) { int l = s->graph_nodes[i].layer; lys[l].push_back(i); if (l < min_l) min_l = l; if (l > max_l) max_l = l; }
    float lc = (float)(max_l - min_l + 1), lw = csz.x / std::max(1.0f, lc);
    for (auto const& e : lys) { float x = cp0.x + (e.first - min_l + 0.5f) * lw, ys = csz.y / std::max((size_t)1, e.second.size()); for (size_t i = 0; i < e.second.size(); i++) { auto &n = s->graph_nodes[e.second[i]]; n.pos = ImVec2(x, cp0.y + (i + 0.5f) * ys); n.size = ImVec2(120, 30); } }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (auto &n : s->graph_nodes) for (auto ci : n.callee_indices) { auto &c = s->graph_nodes[ci]; ImVec2 p1(n.pos.x + n.size.x*0.5f, n.pos.y), p2(c.pos.x - c.size.x*0.5f, c.pos.y); dl->AddBezierCubic(p1, ImVec2(p1.x+50, p1.y), ImVec2(p2.x-50, p2.y), p2, IM_COL32(200, 200, 200, 150), 2.0f); }
    for (size_t i = 0; i < s->graph_nodes.size(); i++) {
        auto &n = s->graph_nodes[i]; ImVec2 pmin(n.pos.x - n.size.x*0.5f, n.pos.y - n.size.y*0.5f), pmax(n.pos.x + n.size.x*0.5f, n.pos.y + n.size.y*0.5f);
        bool hov = ImGui::IsMouseHoveringRect(pmin, pmax); dl->AddRectFilled(pmin, pmax, hov ? IM_COL32(100, 100, 150, 255) : IM_COL32(50, 50, 80, 255), 5.0f);
        dl->AddRect(pmin, pmax, (int)i == s->graph_root_idx ? IM_COL32(255, 255, 0, 255) : IM_COL32(200, 200, 200, 255), 5.0f, 0, 2.0f);
        ImVec2 tsz = ImGui::CalcTextSize(n.name.c_str()); dl->AddText(ImVec2(n.pos.x - std::min(tsz.x, n.size.x-10)*0.5f, n.pos.y - tsz.y*0.5f), IM_COL32(255, 255, 255, 255), n.name.c_str());
        if (hov && ImGui::IsMouseDoubleClicked(0)) { size_t li; if (apex_render_find_line_by_address(d, n.bank, n.addr, &li)) { select_line(s, li, 1); s->graph_needs_rebuild = true; } }
    }
}

void render_editor(ApexProject *p, const ApexRenderedDocument **dp, const OriginalSnapshot *sn, UiState *s)
{
    static const char *dm[] = {"custom", "bytes[n]", "string", "far_string", "far_data", "far_table", "far_code"}, *tm[] = {"custom", "counted(...)", "rows[n](...)"}, *sm[] = {"custom", "ptr16_data", "ptr16_code", "ptr16_string", "far_data", "far_code", "far_string"}, *im[] = {"custom", "byte", "ptr16_data", "ptr16_code", "ptr16_string", "far_data", "far_code", "far_string"}, *dom[] = {"routine_docs", "table_docs"};
    uint8_t b; uint32_t a; if (!selected_address(*dp, s, &b, &a)) { ImGui::TextUnformatted("No addressable line selected."); return; }
    ImGui::Text("Address: B%02x_A%04x", b, (unsigned)a & 0xffffu);
    if (s->request_focus_label) { ImGui::SetKeyboardFocusHere(); s->request_focus_label = 0; }
    ImGui::InputText("Label", s->edit_label_input, 128);
    if (ImGui::Button("Apply Label")) { if (s->edit_label_input[0] == 0) set_status(s, "empty"); else if (apex_project_set_label(p, 1, b, a, s->edit_label_input) == 0) rerender_and_reselect(p, dp, s, b, a); }
    ImGui::SameLine(); if (ImGui::Button("Clear Label")) { if (apex_project_clear_label(p, 1, b, a) == 0) rerender_and_reselect(p, dp, s, b, a); }
    ImGui::InputText("Spec", s->edit_spec_input, 128); ImGui::Separator(); ImGui::TextUnformatted("Inline");
    ImGui::InputText("Inline Spec", s->edit_inline_input, 128); ImGui::Combo("Inline Mode", &s->edit_inline_mode, im, 8); ImGui::InputText("Inline Name", s->edit_inline_name_input, 64);
    if (ImGui::Button("Use Inline Preset")) apply_inline_preset(s);
    ImGui::SameLine(); if (ImGui::Button("Apply Inline")) { if (s->edit_inline_input[0]) { if (apex_project_set_inline(p, 1, b, a, s->edit_inline_input) == 0) rerender_and_reselect(p, dp, s, b, a); } }
    ImGui::SameLine(); if (ImGui::Button("Clear Inline")) { if (apex_project_clear_inline(p, 1, b, a) == 0) rerender_and_reselect(p, dp, s, b, a); }
    ImGui::Separator(); if (ImGui::Combo("Doc Type", &s->edit_doc_mode, dom, 2)) load_doc_editor_buffer(p, s, b, a);
    if (s->request_focus_doc) { ImGui::SetKeyboardFocusHere(); s->request_focus_doc = 0; }
    ImGui::InputTextMultiline("Doc", s->edit_doc_input, 1024, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()*6));
    if (ImGui::Button("Apply Doc")) { if (s->edit_doc_input[0]) { if (apex_project_set_doc(p, s->edit_doc_mode == EDIT_DOC_TABLE, 1, b, a, s->edit_doc_input) == 0) rerender_and_reselect(p, dp, s, b, a); } }
    ImGui::SameLine(); if (ImGui::Button("Clear Doc")) { if (apex_project_clear_doc(p, s->edit_doc_mode == EDIT_DOC_TABLE, 1, b, a) == 0) rerender_and_reselect(p, dp, s, b, a); }
    ImGui::Separator(); ImGui::TextUnformatted("Presets"); ImGui::Combo("Data Mode", &s->edit_data_mode, dm, 7); if (s->edit_data_mode == EDIT_DATA_BYTES) ImGui::InputInt("Bytes", &s->edit_data_length);
    if (ImGui::Button("Use Data Preset")) apply_data_preset(s);
    ImGui::Separator();
    ImGui::Combo("Table Mode", &s->edit_table_mode, tm, 3); if (s->edit_table_mode == EDIT_TABLE_ROWS) ImGui::InputInt("Rows", &s->edit_table_rows);
    ImGui::Combo("Schema", &s->edit_table_schema_mode, sm, 7); if (s->edit_table_schema_mode == EDIT_SCHEMA_CUSTOM) ImGui::InputText("Schema Text", s->edit_table_schema_input, 128);
    if (ImGui::Button("Use Table Preset")) apply_table_preset(s);
    ImGui::Separator();
    if (ImGui::Button("Code")) apply_code_at_selection(p, dp, s);
    ImGui::SameLine(); if (ImGui::Button("Data")) apply_data_at_selection(p, dp, s, s->edit_spec_input[0] ? s->edit_spec_input : "bytes[1]");
    ImGui::SameLine(); if (ImGui::Button("String")) apply_string_at_selection(p, dp, s);
    ImGui::SameLine(); if (ImGui::Button("Table")) apply_table_at_selection(p, dp, s, s->edit_spec_input[0] ? s->edit_spec_input : "counted(ptr16_data)");
    ImGui::SameLine(); if (ImGui::Button("Clear Kind")) clear_kind_at_selection(p, dp, s);
    ImGui::Separator(); ImGui::InputText("Overlay Path", s->save_path_input, 512);
    if (ImGui::Button("Save Overlay") || s->request_save_overlay) { s->request_save_overlay = 0; std::string st; int rc = write_delta_overlay(p, sn, s->save_path_input, &st); if (rc > 0) set_status(s, st.c_str()); else if (rc == 0) { if (apex_project_save_overlay(p, s->save_path_input) == 0) set_status(s, "saved full"); } else set_status(s, "failed"); }
    if (ImGui::Button("Clear Session")) { clear_session(); set_status(s, "cleared"); }
    if (s->status_message[0]) ImGui::TextWrapped("%s", s->status_message);
}

void render_dmd_view(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    auto pr = find_dmd_preview(p, d, s); ImGui::PushItemWidth(-1); ImGui::SliderInt("##scrub", &s->dmd_scrub_offset, -2048, 2048, "Scrub: %d"); ImGui::PopItemWidth();
    if (ImGui::Button("Reset")) s->dmd_scrub_offset = 0;
    ImGui::SameLine();
    if (pr.valid && s->dmd_scrub_offset != 0 && ImGui::Button("Mark DMD")) { uint8_t b; uint32_t a; if (selected_address(d, s, &b, &a)) { uint32_t ta = (uint32_t)((int64_t)a + s->dmd_scrub_offset); if (apex_project_set_kind((ApexProject*)p, 1, b, ta, APEX_KIND_DATA, "dmd_fullframe") == 0) { rerender_and_reselect((ApexProject*)p, (const ApexRenderedDocument**)&d, s, b, ta); s->dmd_scrub_offset = 0; } } }
    ImGui::Separator(); if (pr.valid) render_dmd_preview(pr); else ImGui::TextDisabled("None.");
}

void render_hardware_window(ApexProject *project, const ApexRenderedDocument *document, UiState *state)
{
    static std::vector<HardwareAccess> accesses;
    static bool needs_refresh = true;
    
    if (ImGui::Button("Refresh Scan") || needs_refresh) {
        accesses = find_hardware_accesses(project, document);
        needs_refresh = false;
    }
    
    ImGui::Separator();
    
    if (ImGui::BeginTable("hardware_mapping", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (auto &acc : accesses) {
            ImGui::PushID(acc.reg->addr);
            ImGui::TableNextRow();
            
            // Address
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("$%04X", acc.reg->addr);
            
            // Name
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(acc.reg->name);
            
            // Description
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(acc.reg->desc);
            
            // Usage
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
                        if (line->has_location) snprintf(loc_lbl, 64, "B%02x:A%04x", line->bank, line->cpu_addr);
                        else snprintf(loc_lbl, 64, "Line %lu", (unsigned long)lidx);
                        
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
    if (ImGui::Button("Search Tables (Auto)")) auto_search_tables(p, dp, s);
    ImGui::Separator();
    if (ImGui::BeginTable("tables_list", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Setup", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < p->tables.count; i++) {
            const auto *t = &p->tables.items[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            
            // 1. Address
            ImGui::TableSetColumnIndex(0);
            char a[32]; snprintf(a, 32, "B%02x_A%04x", t->bank, t->addr);
            if (ImGui::Selectable(a, false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) {
                size_t li; if (apex_render_find_line_by_address(*dp, t->bank, t->addr, &li)) select_line(s, li, 1);
            }

            // 2. Setup (Spec)
            ImGui::TableSetColumnIndex(1);
            char spec_buf[128];
            strncpy(spec_buf, table_def_spec_string(t).c_str(), 127); spec_buf[127] = '\0';
            ImGui::PushItemWidth(-FLT_MIN);
            if (ImGui::InputText("##spec", spec_buf, 128, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (apex_project_set_kind(p, 1, t->bank, t->addr, APEX_KIND_TABLE, spec_buf) == 0) {
                    rerender_and_reselect(p, dp, s, t->bank, t->addr);
                    set_status(s, "Table setup updated");
                }
            }
            ImGui::PopItemWidth();

            // 3. Comment
            ImGui::TableSetColumnIndex(2);
            const char *existing_doc = config_doc_at(&p->table_docs, t->bank, t->addr);
            char doc_buf[512] = "";
            if (existing_doc) { strncpy(doc_buf, existing_doc, 511); doc_buf[511] = '\0'; }
            ImGui::PushItemWidth(-FLT_MIN);
            if (ImGui::InputText("##doc", doc_buf, 512, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (apex_project_set_doc(p, 1, 1, t->bank, t->addr, doc_buf) == 0) {
                    rerender_and_reselect(p, dp, s, t->bank, t->addr);
                    set_status(s, "Table comment updated");
                }
            }
            ImGui::PopItemWidth();

            // 4. Actions
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
