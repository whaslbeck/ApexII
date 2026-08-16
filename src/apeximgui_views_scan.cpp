#include "apeximgui_core.h"
#include "apeximgui_views_internal.h"
#include "ImGuiFileDialog.h"
#include "apex_nvram.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cfloat>

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
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,  90.0f, 0);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed,  50.0f, 1);
        ImGui::TableSetupColumn("Text",  ImGuiTableColumnFlags_WidthStretch, 0.0f, 2);
        ImGui::TableHeadersRow();
        {
            int sort_col; bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc)) {
                std::stable_sort(state->ram_ref_results.begin(), state->ram_ref_results.end(),
                    [&](size_t ia, size_t ib) {
                        if (ia >= document->line_count || ib >= document->line_count)
                            return ia < ib;
                        const ApexRenderedLine *a = &document->lines[ia];
                        const ApexRenderedLine *b = &document->lines[ib];
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

/* Immediate Loads: every LDX / LDD / LDU / LDY / LDS immediate (and the CMPx /
   ADDD / SUBD immediate forms).
   The 6809 renders such an immediate symbolically only once a label exists at the
   target address, and creating those labels by hand is tedious — this panel lists
   the instructions and offers, per row: jump to the instruction, jump to the value
   as a target (disassembly + hex), and create a label at that target. */
void render_imm_loads(ApexProject *project,
                      const ApexRenderedDocument **document_ptr,
                      UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;

    bool want_scan = ImGui::Button("Scan");
    ImGui::SameLine();
    if (!state->imm_loads_scanned)
        ImGui::TextDisabled("no scan yet");
    else
        ImGui::Text("%lu immediate load(s)", (unsigned long)state->imm_load_results.size());
    ImGui::SameLine();
    ImGui::Checkbox("ROM targets only", &state->imm_only_rom);
    ImGui::SameLine();
    ImGui::Checkbox("Unlabeled only", &state->imm_only_unlabeled);

    if (want_scan) {
        state->imm_load_results = find_imm_loads(project, document);
        state->imm_loads_scanned = true;
        char msg[64];
        snprintf(msg, sizeof(msg), "%lu immediate load(s)",
                 (unsigned long)state->imm_load_results.size());
        set_status(state, msg);
    }
    if (!state->imm_loads_scanned) {
        ImGui::TextDisabled("Lists LDX/LDD/LDU/… #imm16 so you can label the value's target.");
        return;
    }

    /* Apply the filters into a view of indices into imm_load_results. */
    std::vector<size_t> view;
    view.reserve(state->imm_load_results.size());
    for (size_t i = 0; i < state->imm_load_results.size(); i++) {
        const ImmLoadRef &r = state->imm_load_results[i];
        if (state->imm_only_rom && !r.tgt_in_rom) continue;
        if (state->imm_only_unlabeled && r.symbolic) continue;
        view.push_back(i);
    }
    ImGui::Text("%lu shown", (unsigned long)view.size());

    int label_idx = -1; /* index into imm_load_results; label action is deferred */

    if (ImGui::BeginTable("imm_loads", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Instr",  ImGuiTableColumnFlags_WidthFixed,   90.0f, 0);
        ImGui::TableSetupColumn("Op",     ImGuiTableColumnFlags_WidthFixed,   48.0f, 1);
        ImGui::TableSetupColumn("Imm",    ImGuiTableColumnFlags_WidthFixed,   72.0f, 2);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed,  110.0f, 3);
        ImGui::TableSetupColumn("Label",  ImGuiTableColumnFlags_WidthStretch,  0.0f, 4);
        ImGui::TableHeadersRow();

        {
            int sort_col; bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc)) {
                auto &res = state->imm_load_results;
                std::stable_sort(view.begin(), view.end(),
                    [&](size_t ia, size_t ib) {
                        const ImmLoadRef &a = res[ia];
                        const ImmLoadRef &b = res[ib];
                        int c = 0;
                        if (sort_col == 0)
                            c = ui_cmp_u32(((uint32_t)a.bank<<16)|(a.cpu_addr&0xffffu),
                                           ((uint32_t)b.bank<<16)|(b.cpu_addr&0xffffu));
                        else if (sort_col == 1)
                            c = strcmp(a.mnem, b.mnem);
                        else if (sort_col == 2)
                            c = ui_cmp_u32(a.imm, b.imm);
                        else if (sort_col == 3)
                            c = ui_cmp_u32(((uint32_t)a.tgt_bank<<16)|(a.imm&0xffffu),
                                           ((uint32_t)b.tgt_bank<<16)|(b.imm&0xffffu));
                        else
                            c = ui_cmp_int(a.symbolic, b.symbolic);
                        return sort_asc ? c < 0 : c > 0;
                    });
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)view.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const ImmLoadRef &r = state->imm_load_results[view[(size_t)row]];
                ImGui::PushID(row);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                char addr_buf[40];
                /* Plain column-0 selectable (no SpanAllColumns/AllowOverlap): the
                   Target and Label buttons live in their own cells, so nothing
                   overlaps the selectable — this avoids the ImGui duplicate-ID
                   warning that AllowOverlap produced when hovering those buttons.
                   "##i" still distinguishes it from the target button's id. */
                snprintf(addr_buf, sizeof(addr_buf), "B%02x_A%04x##i",
                         r.bank, (unsigned)r.cpu_addr & 0xffff);
                bool sel = (r.line_index == state->selected_line);
                if (ImGui::Selectable(addr_buf, sel)) {
                    /* Navigate by address, not the stored line index: creating a
                       label re-renders and shifts line indices, so the index can
                       go stale between scans. */
                    size_t li;
                    if (apex_render_find_line_by_address(document, r.bank, r.cpu_addr, &li))
                        select_line(state, li, 1);
                    else if (r.line_index < document->line_count)
                        select_line(state, r.line_index, 1);
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(r.mnem);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("#0x%04x", (unsigned)r.imm);

                ImGui::TableSetColumnIndex(3);
                if (r.tgt_in_rom) {
                    char tbuf[40];
                    snprintf(tbuf, sizeof(tbuf), "B%02x_A%04x##t", r.tgt_bank, (unsigned)r.imm);
                    if (ImGui::SmallButton(tbuf)) {
                        size_t tli;
                        if (apex_render_find_line_by_address(document, r.tgt_bank, r.imm, &tli))
                            select_line(state, tli, 1);
                        const uint8_t *tsrc; size_t tlen, tro = 0;
                        if (project_locate_rom_bytes(project, r.tgt_bank, r.imm,
                                                     &tsrc, &tlen, &tro)) {
                            state->hex_selected_offset = tro;
                            state->hex_active         = true;
                            state->hex_request_follow = 1;
                        }
                    }
                } else {
                    ImGui::TextDisabled("(ram $%04x)", (unsigned)r.imm);
                }

                ImGui::TableSetColumnIndex(4);
                if (r.symbolic) {
                    ImGui::TextDisabled("symbolic");
                } else if (r.tgt_in_rom) {
                    /* "##mk" keeps this button's id distinct from the "Label"
                       column header, which is also hashed from "Label". */
                    if (ImGui::SmallButton("Label##mk"))
                        label_idx = (int)view[(size_t)row];
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    /* Deferred label creation (mutates the project and triggers an async
       re-render, so it must run after the table is closed and the view indices
       are no longer being read). */
    if (label_idx >= 0) {
        ImmLoadRef &r = state->imm_load_results[(size_t)label_idx];
        int has_bank = (r.tgt_bank != 0xffu);
        /* Name the target by its block kind, matching the auto-label convention. */
        const char *prefix = has_bank ? "Loc_" : "Ram_";
        size_t tli;
        if (apex_render_find_line_by_address(document, r.tgt_bank, r.imm, &tli)) {
            switch (document->lines[tli].block_kind) {
            case APEX_RENDER_BLOCK_CODE:         prefix = "Sub_"; break;
            case APEX_RENDER_BLOCK_DATA:         prefix = "Dat_"; break;
            case APEX_RENDER_BLOCK_SPRITE:       prefix = "Spr_"; break;
            case APEX_RENDER_BLOCK_TABLE:        prefix = "Tab_"; break;
            case APEX_RENDER_BLOCK_UNCLASSIFIED: prefix = "Unc_"; break;
            default:                             prefix = "Loc_"; break;
            }
        }
        char name[32];
        snprintf(name, sizeof(name), "%s%04X", prefix, (unsigned)r.imm);
        if (apex_project_set_label(project, has_bank, r.tgt_bank, r.imm, name) == 0) {
            uint8_t nb = r.tgt_bank; uint32_t na = r.imm;
            uint8_t ib = r.bank; uint32_t ia = r.cpu_addr;
            /* Optimistically flag every immediate that points at this target as
               symbolic — the async re-render will show the new label; we can't
               re-scan against the freshly rendered document yet. */
            for (auto &e : state->imm_load_results)
                if (e.tgt_in_rom && e.tgt_bank == nb && e.imm == na) e.symbolic = 1;
            rerender_and_reselect(project, document_ptr, state, ib, ia);
            set_status(state, name);
        }
    }
}

/* Change Log: a session audit trail of every recorded edit (classify, label,
   clear, doc, …), newest first, fed by the ApexProject change listener.  Clicking
   an address jumps the disassembly there. */
void render_change_log(const ApexRenderedDocument *document, UiState *state)
{
    ImGui::Text("%lu change(s)", (unsigned long)state->change_log.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) state->change_log.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("(this session; not saved)");

    if (state->change_log.empty()) {
        ImGui::TextDisabled("No edits yet — classify, label or clear something.");
        return;
    }

    if (ImGui::BeginTable("change_log", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed,   54.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed,  120.0f);
        ImGui::TableSetupColumn("Where",  ImGuiTableColumnFlags_WidthStretch,  0.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)state->change_log.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                /* newest first */
                size_t idx = state->change_log.size() - 1u - (size_t)row;
                const ChangeLogEntry &e = state->change_log[idx];
                ImGui::PushID((int)idx);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%lu", e.seq);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(e.action.c_str());
                ImGui::TableSetColumnIndex(2);
                if (e.has_addr) {
                    char b[32];
                    snprintf(b, sizeof(b), "B%02x_A%04x", e.bank, (unsigned)e.addr & 0xffff);
                    if (ImGui::SmallButton(b)) {
                        size_t li;
                        if (apex_render_find_line_by_address(document, e.bank, e.addr, &li))
                            select_line(state, li, 1);
                    }
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

/* Pull the full coverage bitmap from xpinmamed and group reached bytes into
   contiguous runs, tagging each with whether ApexII currently classifies its
   start as CODE.  Non-code runs are dynamic evidence of missed code. */
static void pinmame_import_coverage(ApexProject *p, const ApexRenderedDocument *doc,
                                    PinmameState &pm)
{
    pm.cov_runs.clear();
    pm.cov_reached.clear();
    apex_pinmame_coverage_summary(pm.port, pm.cov_executed, pm.cov_addressable);

    /* Reached (bank, addr), collected per bank in ascending address order so
       contiguous runs are already adjacent. */
    std::vector<std::pair<uint8_t, uint32_t>> reached;
    for (size_t i = 0; i < p->banks; i++) {
        uint8_t bid = bank_id_for_index(p->banks, (int)i);
        std::vector<uint8_t> f;
        if (apex_pinmame_coverage_window(pm.port, bid, 0x4000u, 0x4000u, f)) {
            for (size_t o = 0; o < f.size(); o++) {
                if (f[o]) reached.push_back({bid, 0x4000u + (uint32_t)o});
            }
        }
    }
    std::vector<uint8_t> sysf;
    if (apex_pinmame_coverage_window(pm.port, -1, 0x8000u, 0x8000u, sysf)) {
        for (size_t o = 0; o < sysf.size(); o++) {
            if (sysf[o]) reached.push_back({0xffu, 0x8000u + (uint32_t)o});
        }
    }

    for (size_t i = 0; i < reached.size();) {
        uint8_t b = reached[i].first;
        uint32_t start = reached[i].second, prev = start;
        size_t j = i + 1;
        while (j < reached.size() && reached[j].first == b && reached[j].second == prev + 1) {
            prev = reached[j].second;
            j++;
        }
        PinmameRun r;
        r.bank = b;
        r.addr = start;
        r.len = prev - start + 1u;
        size_t li;
        r.is_code = (apex_render_find_line_by_address(doc, b, start, &li) &&
                     doc->lines[li].block_kind == APEX_RENDER_BLOCK_CODE) ? 1 : 0;
        pm.cov_runs.push_back(r);
        i = j;
    }
    /* Per-address reached set for the disassembly overlay (O(1) lookup). */
    for (const std::pair<uint8_t, uint32_t> &pr : reached) {
        pm.cov_reached.insert(((uint32_t)pr.first << 16) | (pr.second & 0xffff));
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "coverage: %ld executed, %zu run(s)",
             pm.cov_executed, pm.cov_runs.size());
    pm.status = msg;
}

/* Name of the routine containing `addr` — the nearest code label at or before it
   in the same bank.  Empty if none.  Shared by the RAM-xref, backtrace and
   call-stack features. */
static std::string pm_routine_name(const ApexRenderedDocument *doc, uint8_t bank, uint32_t addr)
{
    size_t li;
    if (!doc || !apex_render_find_line_by_address(doc, bank, addr, &li)) {
        return "";
    }
    for (size_t j = li + 1; j-- > 0;) {
        const ApexRenderedLine *l = &doc->lines[j];
        if (l->has_location && l->bank != bank) break; /* left the bank */
        if (l->kind == APEX_RENDER_LINE_LABEL && l->has_location && l->bank == bank) {
            std::string nm(l->text, l->length);
            size_t c = nm.find(':');
            if (c != std::string::npos) nm.erase(c);
            return nm;
        }
    }
    return "";
}

/* Bounded substring test within one rendered line (text is not NUL-terminated). */
static bool pm_line_has(const ApexRenderedLine *l, const char *needle)
{
    size_t nl = strlen(needle);
    if (l->length < nl) return false;
    for (size_t i = 0; i + nl <= l->length; i++) {
        if (memcmp(l->text + i, needle, nl) == 0) return true;
    }
    return false;
}

/* True if (bank,v) is a return address: an instruction boundary whose preceding
   instruction is a JSR/BSR call. */
static bool pm_is_return_addr(const ApexRenderedDocument *doc, uint8_t bank, uint32_t v)
{
    size_t li;
    if (!apex_render_find_line_by_address(doc, bank, v, &li)) return false;
    /* A label/comment line may sit at v before the instruction — find the instr. */
    bool is_instr = false;
    for (size_t j = li; j < doc->line_count &&
         doc->lines[j].bank == bank && doc->lines[j].cpu_addr == v; j++) {
        if (doc->lines[j].kind == APEX_RENDER_LINE_INSTRUCTION) { is_instr = true; break; }
    }
    if (!is_instr) return false;
    const ApexRenderedLine *prev = nullptr;
    for (size_t j = li; j-- > 0;) {
        const ApexRenderedLine *l = &doc->lines[j];
        if (l->has_location && l->bank != bank) break;
        if (l->kind == APEX_RENDER_LINE_INSTRUCTION && l->bank == bank) { prev = l; break; }
    }
    return prev && (pm_line_has(prev, "JSR") || pm_line_has(prev, "BSR"));
}

/* Heuristic call stack: scan the S stack for 16-bit values that are return
   addresses (see pm_is_return_addr).  Innermost first (closest to S).  A paged
   return (0x4000-0x7fff) has an ambiguous bank, so try the live bank first then
   every bank — the WPC task loop keeps stacks shallow, so this is best-effort. */
static void pm_build_callstack(const ApexProject *p, const ApexRenderedDocument *doc,
                               PinmameState &pm)
{
    pm.callstack.clear();
    unsigned sp = (unsigned)pm.cpu.sp & 0xffffu;
    if (!doc || sp == 0) return;
    std::vector<uint8_t> buf;
    if (!apex_pinmame_memory(pm.port, sp, 64, -1, buf)) return;
    int frames = 0;
    for (size_t off = 0; off + 1 < buf.size() && frames < 16; off++) {
        unsigned v = ((unsigned)buf[off] << 8) | buf[off + 1];
        if (v < 0x4000u) continue; /* RAM — not a code return address */
        uint8_t hit = 0;
        bool found = false;
        if (v >= 0x8000u) {
            found = pm_is_return_addr(doc, 0xffu, v);
            hit = 0xffu;
        } else {
            uint8_t cur = (uint8_t)pm.info.wpc_bank;
            if (pm_is_return_addr(doc, cur, v)) { found = true; hit = cur; }
            for (size_t bi = 0; !found && bi < p->banks; bi++) {
                uint8_t b = bank_id_for_index(p->banks, (int)bi);
                if (b == cur) continue;
                if (pm_is_return_addr(doc, b, v)) { found = true; hit = b; }
            }
        }
        if (!found) continue;
        pm.callstack.push_back({hit, v, 0, pm_routine_name(doc, hit, v)});
        frames++;
        off++; /* consumed a 2-byte return address */
    }
}

/* Add or bump an accessor/PC entry (bank,addr) in a PmHotEntry list. */
static void pm_bump(std::vector<PmHotEntry> &v, uint8_t bank, uint32_t addr,
                    const std::string &name)
{
    for (PmHotEntry &e : v) {
        if (e.bank == bank && e.addr == addr) { e.count++; return; }
    }
    v.push_back({bank, addr, 1, name});
}

/* Parse the switch-control mini-script (one op per line) into PmScriptOp[]. */
static void pm_parse_script(const char *src, std::vector<PmScriptOp> &ops)
{
    ops.clear();
    std::string s(src);
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        std::string line = s.substr(i, (e == std::string::npos) ? std::string::npos : e - i);
        i = (e == std::string::npos) ? s.size() : e + 1;
        char kw[16] = {0};
        int a = 0, b = 0;
        if (sscanf(line.c_str(), " %15s %d %d", kw, &a, &b) < 1) continue;
        for (char *c = kw; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
        }
        if (kw[0] == '#' || kw[0] == 0) continue;
        PmScriptOp op{};
        if      (!strcmp(kw, "press"))   { op.kind = PM_OP_PRESS;   op.a = a; }
        else if (!strcmp(kw, "release")) { op.kind = PM_OP_RELEASE; op.a = a; }
        else if (!strcmp(kw, "pulse"))   { op.kind = PM_OP_PULSE;   op.a = a; op.b = b; }
        else if (!strcmp(kw, "wait"))    { op.kind = PM_OP_WAIT;    op.b = a; }
        else if (!strcmp(kw, "resume"))  { op.kind = PM_OP_RESUME; }
        else if (!strcmp(kw, "pause"))   { op.kind = PM_OP_PAUSE; }
        else continue;
        ops.push_back(op);
    }
}

/* Optional PinMAME dynamic-analysis panel: configure & spawn a headless
   xpinmamed, watch live CPU/bank state, import execution coverage as a
   missed-code worklist, set bank-aware breakpoints/watchpoints, and drive
   switches (incl. a mini-script) to reach code paths. */
/* Per-frame PinMAME work, independent of which windows are open: process
   liveness, the SSE halt handler (incl. xref / jump-resolver collection), the
   ~2 Hz status poll, the DMD fetch and the switch-script executor.  Called once
   per frame from the main loop so the live views stay updated regardless of which
   PinMAME window (if any) is on a foreground dock tab. */
void pinmame_pump(ApexProject *project, const ApexRenderedDocument **document_ptr,
                  UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;
    PinmameState &pm = state->pinmame;

    pm.alive = apex_pinmame_is_alive(pm.pm);
    if (pm.alive) {
        if (pm.connected && !pm.pm.ev_running) apex_pinmame_events_start(pm.pm);
        ApexPinmameHalt h;
        if (apex_pinmame_get_halt(pm.pm, h) && h.seq != pm.last_halt_seq) {
            pm.last_halt = h;
            pm.last_halt_seq = h.seq;
            if (pm.xref_collecting && h.reason == "wp") {
                uint8_t b = (h.pc >= 0x8000) ? 0xffu : (uint8_t)(h.bank & 0xff);
                pm_bump(pm.xref_accessors, b, (uint32_t)h.pc,
                        pm_routine_name(document, b, (uint32_t)h.pc));
                apex_pinmame_control(pm.port, "resume");
            } else if (pm.jmp_collecting && h.reason == "bp" &&
                       (uint32_t)h.pc == pm.jmp_pc) {
                apex_pinmame_control(pm.port, "step");
                ApexPinmameCpu c;
                ApexPinmameInfo ni;
                if (apex_pinmame_get_cpu0(pm.port, c)) {
                    uint32_t tgt = (uint32_t)c.pc & 0xffff;
                    if (tgt != 0 && tgt != pm.jmp_pc) {
                        uint8_t tb = 0xffu;
                        if (tgt < 0x8000u && apex_pinmame_get_info(pm.port, ni))
                            tb = (uint8_t)ni.wpc_bank;
                        pm_bump(pm.jmp_targets, tb, tgt, pm_routine_name(document, tb, tgt));
                    }
                }
                apex_pinmame_control(pm.port, "resume");
            } else {
                pm.resumed = true;
                pm.next_poll = 0.0;
                if (h.reason == "wp") {
                    /* The API gives no watchpoint hit count — tally it ourselves,
                       keyed by the watchpoint whose range contains the accessed
                       address (fall back to the raw address). */
                    uint32_t key = (uint32_t)h.addr & 0xffffu;
                    for (const ApexPinmamePoint &pt : pm.points) {
                        if (pt.is_wp && (uint32_t)h.addr >= (uint32_t)pt.addr &&
                            (uint32_t)h.addr < (uint32_t)pt.addr + (uint32_t)(pt.len > 0 ? pt.len : 1)) {
                            key = (uint32_t)pt.addr & 0xffffu;
                            break;
                        }
                    }
                    pm.wp_hits[key]++;
                }
                if (pm.trace_enabled) {
                    pm.trace.clear();
                    apex_pinmame_exectrace(pm.port, pm.trace);
                }
            }
        }
        double now = ImGui::GetTime();
        if (now >= pm.next_poll) {
            pm.next_poll = now + 0.5;
            pm.connected = apex_pinmame_get_info(pm.port, pm.info);
            if (pm.connected) {
                apex_pinmame_get_cpu0(pm.port, pm.cpu);
                pm.points.clear();   apex_pinmame_points(pm.port, pm.points);
                pm.switches.clear(); apex_pinmame_switches(pm.port, pm.switches);
                for (PmWatch &w : pm.watches) {
                    w.val.clear();
                    apex_pinmame_memory(pm.port, w.addr, w.size, -1, w.val); /* RAM: no bank */
                }
                if (pm.info.paused) pm_build_callstack(project, document, pm);
                else pm.callstack.clear();
                if (pm.launching) { pm.launching = false; pm.status = "connected"; }
            }
        }
        if ((pm.show_dmd || pm.dmd_recording) && pm.connected) {
            double dnow = ImGui::GetTime();
            if (dnow >= pm.dmd_next) {
                pm.dmd_next = dnow + 0.12; /* ~8 Hz refresh */
                bool got = apex_pinmame_dmd(pm.port, pm.dmd_w, pm.dmd_h, pm.dmd_lum);
                /* Append to the GIF only when the frame actually changed; write the
                   PREVIOUS frame with the time it was on screen (one-frame delay). */
                if (got && pm.dmd_recording && pm.dmd_gif && !pm.dmd_lum.empty() &&
                    pm.dmd_lum != pm.dmd_pending) {
                    if (pm.dmd_has_pending) {
                        int delay = (int)((dnow - pm.dmd_pending_time) * 100.0 + 0.5);
                        if (delay < 2) delay = 2;
                        if (delay > 6000) delay = 6000;
                        apex_gif_add_frame(pm.dmd_gif, pm.dmd_pending.data(), delay);
                        pm.dmd_rec_frames++;
                    }
                    pm.dmd_pending = pm.dmd_lum;
                    pm.dmd_pending_time = dnow;
                    pm.dmd_has_pending = true;
                }
            }
        }
    } else {
        if (pm.launching) {
            std::string tail = apex_pinmame_log_tail(pm.pm);
            pm.status = "xpinmamed exited before responding" +
                        (tail.empty() ? std::string(" (no log output)")
                                      : (": " + tail));
            pm.launching = false;
        }
        pm.connected = false;
    }

    /* Frame-stepped switch script: one op per frame; `wait` schedules ahead. */
    if (pm.script_running) {
        if (!pm.alive || !pm.connected) {
            pm.script_running = false;
            pm.script_status = "stopped (disconnected)";
        } else {
            double now = ImGui::GetTime();
            if (now >= pm.script_next) {
                if (pm.script_ip >= pm.script_ops.size()) {
                    pm.script_running = false;
                    pm.script_status = "done";
                } else {
                    PmScriptOp op = pm.script_ops[pm.script_ip++];
                    switch (op.kind) {
                    case PM_OP_PRESS:   apex_pinmame_input(pm.port, op.a, 1, 0); break;
                    case PM_OP_RELEASE: apex_pinmame_input(pm.port, op.a, 0, 0); break;
                    case PM_OP_PULSE:   apex_pinmame_input(pm.port, op.a, 1, op.b > 0 ? op.b : 100); break;
                    case PM_OP_WAIT:    pm.script_next = now + (op.b > 0 ? op.b : 0) / 1000.0; break;
                    case PM_OP_RESUME:  apex_pinmame_control(pm.port, "resume"); pm.resumed = true; break;
                    case PM_OP_PAUSE:   apex_pinmame_control(pm.port, "pause"); break;
                    }
                    char s[48];
                    snprintf(s, sizeof(s), "op %zu/%zu", pm.script_ip, pm.script_ops.size());
                    pm.script_status = s;
                }
            }
        }
    }
}

/* PinMAME control hub: config, launch / exec control, live status + registers,
   the halt banner, and the breakpoint / inspection tools. */
void render_pinmame(ApexProject *project, const ApexRenderedDocument **document_ptr,
                    UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;
    PinmameState &pm = state->pinmame;
    bool alive = pm.alive;

    /* ---- Config ---- */
    if (ImGui::CollapsingHeader("Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("xpinmamed binary", pm.bin_path, sizeof(pm.bin_path));
        ImGui::InputText("PinMAME rom dir",  pm.rompath,  sizeof(pm.rompath));
        ImGui::InputText("Game (romset)",    pm.game,     sizeof(pm.game));
        ImGui::SameLine();
        if (ImGui::Button("Browse…")) {
            IGFD::FileDialogConfig cfg;
            cfg.path = pm.rompath[0] ? pm.rompath : ".";
            ImGuiFileDialog::Instance()->OpenDialog("PmRomZip", "Select PinMAME rom (.zip)",
                                                    ".zip", cfg);
        }
        ImGui::InputInt("HTTP port",         &pm.port);
        if (pm.port < 1 || pm.port > 65535) pm.port = 8080;
    }
    /* Rom-zip picker: derive rom dir + game name (basename without .zip). */
    if (ImGuiFileDialog::Instance()->Display("PmRomZip", ImGuiWindowFlags_NoCollapse,
                                             ImVec2(560, 360))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string dir  = ImGuiFileDialog::Instance()->GetCurrentPath();
            std::string name = ImGuiFileDialog::Instance()->GetCurrentFileName();
            snprintf(pm.rompath, sizeof(pm.rompath), "%s", dir.c_str());
            size_t dot = name.rfind(".zip");
            if (dot != std::string::npos) name.erase(dot);
            snprintf(pm.game, sizeof(pm.game), "%s", name.c_str());
        }
        ImGuiFileDialog::Instance()->Close();
    }

    ImGui::Separator();

    /* ---- Process control ---- */
    if (!pm.alive) {
        if (ImGui::Button("Launch")) {
            if (apex_pinmame_launch(pm.pm, pm.bin_path, pm.rompath, pm.game, pm.port)) {
                pm.status = "launched — waiting for debugger…";
                pm.next_poll = 0.0;
                pm.connected = false;
                pm.launching = true;
            } else {
                pm.status = "launch failed: " + pm.pm.last_error;
                pm.launching = false;
            }
        }
    } else {
        if (ImGui::Button("Stop")) {
            apex_pinmame_stop(pm.pm);
            pm.connected = false;
            pm.resumed = false;
            pm.status = "stopped";
        }
        ImGui::SameLine();
        if (ImGui::Button("Resume")) { apex_pinmame_control(pm.port, "resume"); pm.resumed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Pause"))  { apex_pinmame_control(pm.port, "pause");  pm.resumed = false; }
        ImGui::SameLine();
        if (ImGui::Button("Step"))   apex_pinmame_control(pm.port, "step");
    }

    if (pm.alive && pm.connected) {
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "connected: %s (%s)",
                           pm.info.game.c_str(), pm.info.description.c_str());
        ImGui::Text("state: %s   wpc_bank: 0x%02x",
                    pm.info.paused ? "paused" : "running", pm.info.wpc_bank & 0xff);
        /* Full 6809 register set (frozen & accurate while halted at a breakpoint). */
        const ApexPinmameCpu &c = pm.cpu;
        unsigned d = ((unsigned)(c.a & 0xff) << 8) | (unsigned)(c.b & 0xff);
        char cc[9];
        const char *bits = "EFHINZVC"; /* 6809 CC: E F H I N Z V C (bit 7..0) */
        for (int i = 0; i < 8; i++) cc[i] = (c.cc & (0x80 >> i)) ? bits[i] : '.';
        cc[8] = '\0';
        ImGui::TextUnformatted("registers:");
        ImGui::Text("  PC %04lx  S %04lx  U %04lx  X %04lx  Y %04lx",
                    (unsigned long)c.pc & 0xffff, (unsigned long)c.sp & 0xffff,
                    (unsigned long)c.u & 0xffff, (unsigned long)c.x & 0xffff,
                    (unsigned long)c.y & 0xffff);
        ImGui::Text("  A %02lx  B %02lx  D %04x  DP %02lx  CC %02lx [%s]",
                    (unsigned long)c.a & 0xff, (unsigned long)c.b & 0xff, d,
                    (unsigned long)c.dp & 0xff, (unsigned long)c.cc & 0xff, cc);
    } else if (alive) {
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "process up, connecting…");
    }
    if (!pm.status.empty()) {
        bool err = pm.status.find("fail") != std::string::npos ||
                   pm.status.find("exited") != std::string::npos ||
                   pm.status.find("in use") != std::string::npos ||
                   pm.status.find("not found") != std::string::npos;
        if (err) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            ImGui::TextWrapped("%s", pm.status.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("%s", pm.status.c_str());
        }
    }

    /* ---- Breakpoints & watchpoints (bank-aware, from the disassembly) ---- */
    ImGui::SeparatorText("Breakpoints & watchpoints");
    ImGui::BeginDisabled(!alive || !pm.connected);
    if (ImGui::Button("Break at selection")) {
        if (state->selected_line < document->line_count) {
            const ApexRenderedLine *l = &document->lines[state->selected_line];
            if (l->has_location) {
                int bank = (l->cpu_addr >= 0x8000u) ? -1 : (int)l->bank; /* system takes no bank */
                if (apex_pinmame_breakpoint(pm.port, "add", l->cpu_addr, bank,
                                            pm.bp_cond[0] ? pm.bp_cond : nullptr))
                    pm.status = "breakpoint added at selection";
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear BPs")) apex_pinmame_breakpoint(pm.port, "clear", 0, -1, nullptr);
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##bpcond", "condition e.g. A==7F", pm.bp_cond, sizeof(pm.bp_cond));
    ImGui::SameLine();
    ImGui::TextDisabled("(applies to new breakpoints; REG ==/!=/</> HEX)");
    {
        static char wp_addr[16] = "";
        const char *modes[] = {"read", "write", "rw"};
        int mi = pm.wp_mode - 1;
        if (mi < 0 || mi > 2) mi = 1;
        ImGui::SetNextItemWidth(90);
        ImGui::InputText("RAM addr (hex)", wp_addr, sizeof(wp_addr));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::Combo("##wpmode", &mi, modes, 3);
        pm.wp_mode = mi + 1;
        ImGui::SameLine();
        if (ImGui::Button("Add watch") && wp_addr[0]) {
            unsigned a = (unsigned)strtoul(wp_addr, nullptr, 16);
            apex_pinmame_watchpoint(pm.port, "add", a, -1, 1, pm.wp_mode);
            pm.wp_hits[a & 0xffff] = 0; /* fresh hit count */
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear WPs")) {
            apex_pinmame_watchpoint(pm.port, "clear", 0, -1, 1, 2);
            pm.wp_hits.clear();
        }
    }
    ImGui::EndDisabled();

    /* Halt banner: the SSE event tells us which breakpoint/watchpoint fired. */
    if (pm.connected && pm.info.paused && pm.resumed && !pm.xref_collecting &&
        !pm.jmp_collecting) {
        const ApexPinmameHalt &h = pm.last_halt;
        bool have = pm.last_halt_seq != 0;
        uint32_t pc = (uint32_t)(have ? h.pc : pm.cpu.pc);
        uint8_t pcbank = (pc >= 0x8000u) ? 0xffu
                       : (uint8_t)(have ? (h.bank & 0xff) : pm.info.wpc_bank);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
        if (have && h.reason == "wp") {
            ImGui::Text("HALTED — watchpoint hit: accessed 0x%04lx   PC B%02x_A%04x",
                        (unsigned long)h.addr & 0xffff, pcbank, (unsigned)pc & 0xffff);
        } else if (have && h.reason == "bp") {
            ImGui::Text("HALTED — breakpoint at B%02x_A%04x", pcbank, (unsigned)pc & 0xffff);
        } else {
            ImGui::Text("HALTED — PC B%02x_A%04x", pcbank, (unsigned)pc & 0xffff);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("jump PC")) {
            size_t li;
            if (apex_render_find_line_by_address(document, pcbank, pc, &li))
                select_line(state, li, 1);
        }
        /* For a watchpoint hit, offer to jump to the accessed address when it's
           in ROM (RAM accesses aren't in the disassembly). */
        if (have && h.reason == "wp" && h.addr >= 0x4000) {
            ImGui::SameLine();
            if (ImGui::SmallButton("jump accessed")) {
                uint8_t ab = (h.addr >= 0x8000) ? 0xffu : (uint8_t)(h.bank & 0xff);
                size_t li;
                if (apex_render_find_line_by_address(document, ab, (uint32_t)h.addr, &li))
                    select_line(state, li, 1);
            }
        }
    }

    if (!pm.points.empty() && ImGui::BeginTable("pm_points", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, 90))) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableHeadersRow();
        const char *wpmode[] = {"?", "read", "write", "rw"};
        int del_wp = -1, del_idx = -1;
        int prow = 0;
        for (const ApexPinmamePoint &pt : pm.points) {
            ImGui::PushID(prow++);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(pt.is_wp ? "WP" : "BP");
            ImGui::TableSetColumnIndex(1);
            uint8_t db = (pt.bank < 0) ? 0xffu : (uint8_t)pt.bank;
            ImGui::Text("B%02x_A%04x", db, (unsigned)pt.addr & 0xffff);
            ImGui::TableSetColumnIndex(2);
            long hits = pt.hits;
            if (pt.is_wp) {  /* watchpoints have no API hit count — use our SSE tally */
                auto it = pm.wp_hits.find((uint32_t)pt.addr & 0xffffu);
                hits = (it != pm.wp_hits.end()) ? it->second : 0;
            }
            ImGui::Text("%ld", hits);
            ImGui::TableSetColumnIndex(3);
            if (pt.is_wp) ImGui::TextDisabled("%s", wpmode[(pt.mode >= 1 && pt.mode <= 3) ? pt.mode : 0]);
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("del")) { del_wp = pt.is_wp; del_idx = pt.idx; }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (del_idx >= 0) {
            apex_pinmame_point_delete(pm.port, del_wp, del_idx);
            pm.next_poll = 0.0; /* refresh the list immediately */
        }
    }

    /* ---- Live RAM watch (values resolved to NVRAM symbols) ---- */
    ImGui::SeparatorText("RAM watch");
    ImGui::BeginDisabled(!alive || !pm.connected);
    ImGui::SetNextItemWidth(90);
    ImGui::InputText("addr (hex)##watch", pm.watch_addr, sizeof(pm.watch_addr));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##wsize", &pm.watch_size, "1 byte\0" "2 bytes\0");
    ImGui::SameLine();
    if (ImGui::Button("Watch##ram") && pm.watch_addr[0]) {
        PmWatch w;
        w.addr = (uint32_t)strtoul(pm.watch_addr, nullptr, 16);
        w.size = (pm.watch_size == 1) ? 2 : 1;
        const char *nm = symbol_name_at(w.addr, &project->symbols);
        w.name = nm ? nm : "";
        pm.watches.push_back(w);
    }
    ImGui::EndDisabled();
    if (!pm.watches.empty() && ImGui::BeginTable("pm_watch", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, 110))) {
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        const char *wml = (pm.wp_mode == 1) ? "r" : (pm.wp_mode == 3) ? "rw" : "w";
        int del = -1;
        for (size_t i = 0; i < pm.watches.size(); i++) {
            const PmWatch &w = pm.watches[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%04x", w.addr & 0xffff);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(w.name.c_str());
            ImGui::TableSetColumnIndex(2);
            if (w.val.size() >= (size_t)w.size) {
                unsigned v = w.val[0];
                if (w.size == 2) v = ((unsigned)w.val[0] << 8) | w.val[1]; /* big-endian */
                ImGui::Text(w.size == 2 ? "0x%04x (%u)" : "0x%02x (%u)", v, v);
            } else {
                ImGui::TextDisabled("…");
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::BeginDisabled(!pm.alive || !pm.connected);
            char wpl[16];
            snprintf(wpl, sizeof(wpl), "wp:%s", wml);  /* set a watchpoint (current mode) */
            if (ImGui::SmallButton(wpl)) {
                apex_pinmame_watchpoint(pm.port, "add", w.addr, -1, w.size, pm.wp_mode);
                pm.wp_hits[w.addr & 0xffff] = 0;
                pm.status = "watchpoint set on RAM";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) del = (int)i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (del >= 0) pm.watches.erase(pm.watches.begin() + del);
    }

    /* ---- Heuristic call stack (return addresses on the S stack) ---- */
    if (pm.connected && pm.info.paused && !pm.callstack.empty()) {
        ImGui::SeparatorText("Call stack (heuristic)");
        if (ImGui::BeginTable("pm_stack", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                ImVec2(0, 110))) {
            ImGui::TableSetupColumn("Return", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch, 0.0f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < pm.callstack.size(); i++) {
                const PmHotEntry &e = pm.callstack[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char rb[24];
                snprintf(rb, sizeof(rb), "B%02x_A%04x", e.bank, e.addr & 0xffff);
                if (ImGui::Selectable(rb, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    size_t li;
                    if (apex_render_find_line_by_address(document, e.bank, e.addr, &li))
                        select_line(state, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(e.name.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    /* ---- Execution backtrace (recent instructions before the halt) ---- */
    ImGui::SeparatorText("Backtrace");
    ImGui::BeginDisabled(!alive || !pm.connected);
    if (ImGui::Checkbox("record trace", &pm.trace_enabled)) {
        apex_pinmame_exectrace_cmd(pm.port, pm.trace_enabled ? "start" : "stop");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!pm.trace_enabled);
    if (ImGui::Button("Refresh")) {
        pm.trace.clear();
        apex_pinmame_exectrace(pm.port, pm.trace);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("newest first; captured at each halt");
    if (!pm.trace.empty() && ImGui::BeginTable("pm_bt", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
            ImVec2(0, 150))) {
        ImGui::TableSetupColumn("PC",   ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("A B X", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableHeadersRow();
        int shown = (int)pm.trace.size();
        if (shown > 200) shown = 200; /* the immediate lead-up is what matters */
        ImGuiListClipper clip;
        clip.Begin(shown);
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                /* newest last in the ring → reverse for newest-first display */
                const ApexPinmameTrace &t = pm.trace[pm.trace.size() - 1 - (size_t)row];
                uint8_t b = (t.pc >= 0x8000) ? 0xffu : (uint8_t)(t.bank & 0xff);
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char pb[24];
                snprintf(pb, sizeof(pb), "B%02x_A%04x", b, (unsigned)t.pc & 0xffff);
                if (ImGui::Selectable(pb, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    size_t li;
                    if (apex_render_find_line_by_address(document, b, (uint32_t)t.pc, &li))
                        select_line(state, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%02lx %02lx %04lx", t.a & 0xff, t.b & 0xff, t.x & 0xffff);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(pm_routine_name(document, b, (uint32_t)t.pc).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    /* ---- Dynamic RAM xref: which code accesses a variable ---- */
    ImGui::SeparatorText("RAM xref (who accesses X)");
    {
        const char *modes[] = {"read", "write", "rw"};
        int mi = pm.xref_mode - 1;
        if (mi < 0 || mi > 2) mi = 2;
        ImGui::BeginDisabled(!alive || !pm.connected || pm.xref_collecting);
        ImGui::SetNextItemWidth(90);
        ImGui::InputText("addr (hex)##xref", pm.xref_addr, sizeof(pm.xref_addr));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::Combo("##xrefmode", &mi, modes, 3);
        pm.xref_mode = mi + 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!pm.xref_collecting) {
            ImGui::BeginDisabled(!alive || !pm.connected || pm.xref_addr[0] == 0);
            if (ImGui::Button("Collect accessors")) {
                unsigned a = (unsigned)strtoul(pm.xref_addr, nullptr, 16);
                apex_pinmame_watchpoint(pm.port, "clear", 0, -1, 1, 2);
                apex_pinmame_watchpoint(pm.port, "add", a, -1, 1, pm.xref_mode);
                pm.xref_accessors.clear();
                pm.xref_collecting = true;
                apex_pinmame_control(pm.port, "resume");
            }
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Stop")) {
                pm.xref_collecting = false;
                apex_pinmame_control(pm.port, "pause");
                apex_pinmame_watchpoint(pm.port, "clear", 0, -1, 1, 2);
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "collecting… %zu site(s)",
                               pm.xref_accessors.size());
        }
        if (!pm.xref_accessors.empty() && ImGui::BeginTable("pm_xref", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                ImVec2(0, 110))) {
            ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("PC",   ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch, 0.0f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < pm.xref_accessors.size(); i++) {
                const PmHotEntry &e = pm.xref_accessors[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char cb[24];
                snprintf(cb, sizeof(cb), "%ld", e.count);
                if (ImGui::Selectable(cb, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    size_t li;
                    if (apex_render_find_line_by_address(document, e.bank, e.addr, &li))
                        select_line(state, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("B%02x_A%04x", e.bank, e.addr & 0xffff);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(e.name.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    /* ---- Computed-jump resolver: discover the targets of an indexed JMP/JSR ---- */
    ImGui::SeparatorText("Computed-jump resolver");
    ImGui::BeginDisabled(!alive || !pm.connected || pm.jmp_collecting);
    if (ImGui::Button("From selection##jmp") && state->selected_line < document->line_count) {
        const ApexRenderedLine *l = &document->lines[state->selected_line];
        if (l->has_location)
            snprintf(pm.jmp_addr, sizeof(pm.jmp_addr), "%04x", l->cpu_addr & 0xffff);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputText("jump addr (hex)", pm.jmp_addr, sizeof(pm.jmp_addr));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!pm.jmp_collecting) {
        ImGui::BeginDisabled(!alive || !pm.connected || pm.jmp_addr[0] == 0);
        if (ImGui::Button("Resolve targets")) {
            pm.jmp_pc = (uint32_t)strtoul(pm.jmp_addr, nullptr, 16);
            /* Bank the breakpoint like the disasm: system >=0x8000 → none. */
            uint8_t selb = 0xffu;
            if (state->selected_line < document->line_count &&
                document->lines[state->selected_line].cpu_addr == pm.jmp_pc)
                selb = document->lines[state->selected_line].bank;
            pm.jmp_bank = (pm.jmp_pc >= 0x8000u) ? -1 : (int)selb;
            apex_pinmame_breakpoint(pm.port, "clear", 0, -1, nullptr);
            apex_pinmame_breakpoint(pm.port, "add", pm.jmp_pc, pm.jmp_bank, nullptr);
            pm.jmp_targets.clear();
            pm.jmp_collecting = true;
            apex_pinmame_control(pm.port, "resume");
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Stop##jmp")) {
            pm.jmp_collecting = false;
            apex_pinmame_control(pm.port, "pause");
            apex_pinmame_breakpoint(pm.port, "clear", 0, -1, nullptr);
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "resolving… %zu target(s)",
                           pm.jmp_targets.size());
    }
    if (!pm.jmp_targets.empty() && ImGui::BeginTable("pm_jmp", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
            ImVec2(0, 110))) {
        ImGui::TableSetupColumn("Hits",   ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < pm.jmp_targets.size(); i++) {
            const PmHotEntry &e = pm.jmp_targets[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char cb[24];
            snprintf(cb, sizeof(cb), "%ld", e.count);
            if (ImGui::Selectable(cb, false, ImGuiSelectableFlags_SpanAllColumns)) {
                size_t li;
                if (apex_render_find_line_by_address(document, e.bank, e.addr, &li))
                    select_line(state, li, 1);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("B%02x_A%04x", e.bank, e.addr & 0xffff);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

}

/* The DMD amber palette: luminance byte -> RGB, matching the live view. */
static void pm_dmd_palette(uint8_t pal[256 * 3])
{
    for (int i = 0; i < 256; i++) {
        pal[i * 3 + 0] = (uint8_t)i;
        pal[i * 3 + 1] = (uint8_t)(i * 150 / 255);
        pal[i * 3 + 2] = (uint8_t)(i * 20 / 255);
    }
}

/* Finalise an in-progress GIF recording (writes the last pending frame). */
void pinmame_stop_gif(UiState *state)
{
    PinmameState &pm = state->pinmame;
    if (pm.dmd_gif) {
        if (pm.dmd_has_pending) {
            apex_gif_add_frame(pm.dmd_gif, pm.dmd_pending.data(), 50);
            pm.dmd_rec_frames++;
        }
        apex_gif_end(pm.dmd_gif);
        pm.dmd_gif = nullptr;
    }
    pm.dmd_recording = false;
    pm.dmd_has_pending = false;
    pm.dmd_pending.clear();
}

/* ---- PinMAME · DMD window ---- */
void render_pinmame_dmd(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state)
{
    (void)project; (void)document_ptr;
    PinmameState &pm = state->pinmame;
    ImGui::BeginDisabled(!pm.alive || !pm.connected);
    ImGui::Checkbox("live DMD", &pm.show_dmd);
    ImGui::EndDisabled();

    /* ---- export: single PNG + animated GIF ---- */
    bool have_frame = pm.dmd_w > 0 && (int)pm.dmd_lum.size() >= pm.dmd_w * pm.dmd_h;
    ImGui::SameLine();
    ImGui::BeginDisabled(!have_frame);
    if (ImGui::Button("Save PNG")) {
        IGFD::FileDialogConfig cfg;
        cfg.path = ".";
        cfg.fileName = "dmd.png";
        ImGuiFileDialog::Instance()->OpenDialog("PmDmdPng", "Save DMD frame (.png)", ".png", cfg);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!pm.dmd_recording) {
        ImGui::BeginDisabled(!pm.alive || !pm.connected);
        if (ImGui::Button("Record GIF")) {
            IGFD::FileDialogConfig cfg;
            cfg.path = ".";
            cfg.fileName = "dmd.gif";
            ImGuiFileDialog::Instance()->OpenDialog("PmDmdGif", "Record DMD to (.gif)", ".gif", cfg);
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Stop recording")) pinmame_stop_gif(state);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "REC  %ld frame(s)", pm.dmd_rec_frames);
    }
    /* Save-PNG dialog */
    if (ImGuiFileDialog::Instance()->Display("PmDmdPng", ImGuiWindowFlags_NoCollapse,
                                             ImVec2(560, 360))) {
        if (ImGuiFileDialog::Instance()->IsOk() && have_frame) {
            std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
            uint8_t pal[256 * 3];
            pm_dmd_palette(pal);
            std::vector<uint8_t> rgb((size_t)pm.dmd_w * pm.dmd_h * 3);
            for (size_t i = 0; i < (size_t)pm.dmd_w * pm.dmd_h; i++) {
                uint8_t l = pm.dmd_lum[i];
                rgb[i * 3 + 0] = pal[l * 3 + 0];
                rgb[i * 3 + 1] = pal[l * 3 + 1];
                rgb[i * 3 + 2] = pal[l * 3 + 2];
            }
            pm.status = apex_png_write_rgb(path.c_str(), pm.dmd_w, pm.dmd_h, rgb.data())
                            ? "DMD PNG saved" : "PNG save failed";
        }
        ImGuiFileDialog::Instance()->Close();
    }
    /* Record-GIF dialog */
    if (ImGuiFileDialog::Instance()->Display("PmDmdGif", ImGuiWindowFlags_NoCollapse,
                                             ImVec2(560, 360))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
            uint8_t pal[256 * 3];
            pm_dmd_palette(pal);
            int w = pm.dmd_w > 0 ? pm.dmd_w : 128;
            int h = pm.dmd_h > 0 ? pm.dmd_h : 32;
            pm.dmd_gif = apex_gif_begin(path.c_str(), w, h, pal);
            if (pm.dmd_gif) {
                pm.dmd_recording = true;
                pm.dmd_has_pending = false;
                pm.dmd_pending.clear();
                pm.dmd_rec_frames = 0;
                pm.show_dmd = true; /* ensure frames are fetched */
                pm.status = "recording DMD → GIF";
            } else {
                pm.status = "GIF open failed";
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (pm.show_dmd && pm.dmd_w > 0 && (int)pm.dmd_lum.size() >= pm.dmd_w * pm.dmd_h) {
        float availw = ImGui::GetContentRegionAvail().x;
        float scale = std::max(1.0f, std::min(8.0f, availw / (float)pm.dmd_w));
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 sz(pm.dmd_w * scale, pm.dmd_h * scale);
        ImGui::InvisibleButton("pm_dmd_canvas", sz);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), IM_COL32(14, 10, 2, 255));
        for (int y = 0; y < pm.dmd_h; y++) {
            for (int x = 0; x < pm.dmd_w; x++) {
                uint8_t l = pm.dmd_lum[(size_t)y * pm.dmd_w + x];
                if (!l) continue;
                float f = l / 255.0f;
                ImU32 col = IM_COL32((int)(255 * f), (int)(150 * f), (int)(20 * f), 255); /* amber */
                ImVec2 a(pos.x + x * scale, pos.y + y * scale);
                dl->AddRectFilled(a, ImVec2(a.x + scale - 0.4f, a.y + scale - 0.4f), col);
            }
        }
    } else {
        ImGui::TextDisabled("enable 'live DMD' while connected");
    }
}

/* ---- PinMAME · Switches window (matrix + mini-script) ---- */
void render_pinmame_switches(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state)
{
    (void)project; (void)document_ptr;
    PinmameState &pm = state->pinmame;
    ImGui::BeginDisabled(!pm.alive || !pm.connected);
    if (!pm.switches.empty() && ImGui::BeginTable("pm_sw", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
            ImVec2(0, 200))) {
        ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("Col/Row", ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)pm.switches.size());
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const ApexPinmameSwitch &sw = pm.switches[(size_t)row];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (sw.active) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%d", sw.num);
                else ImGui::Text("%d", sw.num);
                ImGui::TableSetColumnIndex(1);
                if (sw.col >= 0 && sw.row >= 0) ImGui::Text("%d/%d", sw.col, sw.row);
                else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(2);
                if (!sw.name.empty()) ImGui::TextUnformatted(sw.name.c_str());
                else ImGui::TextDisabled("(unnamed)");
                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton("pulse")) apex_pinmame_input(pm.port, sw.num, 1, 100);
                ImGui::SameLine();
                if (ImGui::SmallButton(sw.active ? "off" : "on"))
                    apex_pinmame_input(pm.port, sw.num, sw.active ? 0 : 1, 0);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("script: press/release/pulse <sw> [ms], wait <ms>, resume, pause  (# = comment)");
    ImGui::InputTextMultiline("##pmscript", pm.script, sizeof(pm.script), ImVec2(-1, 80));
    if (!pm.script_running) {
        if (ImGui::Button("Run script")) {
            pm_parse_script(pm.script, pm.script_ops);
            pm.script_ip = 0;
            pm.script_next = 0.0;
            pm.script_running = !pm.script_ops.empty();
            pm.script_status = pm.script_running ? "started" : "empty script";
        }
    } else if (ImGui::Button("Stop script")) {
        pm.script_running = false;
        pm.script_status = "stopped";
    }
    ImGui::EndDisabled();
    if (!pm.script_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", pm.script_status.c_str());
    }
}

/* ---- PinMAME · Coverage window (coverage import/overlay + call hotlist) ---- */
void render_pinmame_coverage(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;
    PinmameState &pm = state->pinmame;

    ImGui::SeparatorText("Code coverage");
    ImGui::BeginDisabled(!pm.alive || !pm.connected);
    if (ImGui::Button("Start"))  apex_pinmame_coverage_cmd(pm.port, "start");
    ImGui::SameLine();
    if (ImGui::Button("Clear"))  apex_pinmame_coverage_cmd(pm.port, "clear");
    ImGui::SameLine();
    if (ImGui::Button("Import")) pinmame_import_coverage(project, document, pm);
    ImGui::EndDisabled();

    if (pm.cov_addressable > 0) {
        ImGui::Text("executed %ld / %ld bytes (%.1f%%)", pm.cov_executed, pm.cov_addressable,
                    100.0 * (double)pm.cov_executed / (double)pm.cov_addressable);
    }
    ImGui::Checkbox("Only runs not classified as code (missed code)", &pm.cov_only_noncode);
    ImGui::SameLine();
    ImGui::Checkbox("overlay in disasm", &pm.cov_overlay);
    if (pm.cov_overlay && !pm.cov_reached.empty())
        ImGui::TextDisabled("disasm: green = executed, red = never executed (import first)");

    std::vector<size_t> view;
    for (size_t i = 0; i < pm.cov_runs.size(); i++) {
        if (pm.cov_only_noncode && pm.cov_runs[i].is_code) continue;
        view.push_back(i);
    }
    ImGui::Text("%zu run(s) shown", view.size());

    if (ImGui::BeginTable("pm_cov", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, 150))) {
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed,   90.0f);
        ImGui::TableSetupColumn("Len",   ImGuiTableColumnFlags_WidthFixed,   56.0f);
        ImGui::TableSetupColumn("Kind",  ImGuiTableColumnFlags_WidthFixed,   64.0f);
        ImGui::TableSetupColumn("Go",    ImGuiTableColumnFlags_WidthStretch,  0.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int)view.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const PinmameRun &r = pm.cov_runs[view[(size_t)row]];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("B%02x_A%04x", r.bank, (unsigned)r.addr & 0xffff);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", r.len);
                ImGui::TableSetColumnIndex(2);
                if (r.is_code) ImGui::TextDisabled("code");
                else ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "not code");
                ImGui::TableSetColumnIndex(3);
                char b[32];
                snprintf(b, sizeof(b), "jump##%d", row);
                if (ImGui::SmallButton(b)) {
                    size_t li;
                    if (apex_render_find_line_by_address(document, r.bank, r.addr, &li))
                        select_line(state, li, 1);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Call hotlist");
    ImGui::BeginDisabled(!pm.alive || !pm.connected);
    if (ImGui::Button("Instrument routines")) {
        apex_pinmame_instrument(pm.port, "clear", 0, -1);
        const int CAP = 2000;
        int n = 0;
        for (size_t i = 0; i < document->line_count && n < CAP; i++) {
            const ApexRenderedLine *l = &document->lines[i];
            if (l->kind == APEX_RENDER_LINE_LABEL &&
                l->block_kind == APEX_RENDER_BLOCK_CODE && l->has_location) {
                int bank = (l->cpu_addr >= 0x8000u) ? -1 : (int)l->bank;
                apex_pinmame_instrument(pm.port, "add", l->cpu_addr, bank);
                n++;
            }
        }
        pm.hot_instrumented = n;
        char m[64];
        snprintf(m, sizeof(m), "instrumented %d routine(s)%s", n, n >= CAP ? " (capped)" : "");
        pm.status = m;
    }
    ImGui::SameLine();
    if (ImGui::Button("Read hotlist")) {
        std::vector<ApexPinmameCount> cs;
        apex_pinmame_instrument_read(pm.port, cs);
        pm.hotlist.clear();
        for (const ApexPinmameCount &c : cs) {
            if (c.count <= 0) continue;
            PmHotEntry e;
            e.bank = (c.bank < 0) ? 0xffu : (uint8_t)c.bank;
            e.addr = (uint32_t)c.addr;
            e.count = c.count;
            size_t li;
            if (apex_render_find_line_by_address(document, e.bank, e.addr, &li)) {
                for (size_t j = li; j < document->line_count &&
                     document->lines[j].bank == e.bank &&
                     document->lines[j].cpu_addr == e.addr; j++) {
                    if (document->lines[j].kind == APEX_RENDER_LINE_LABEL) {
                        std::string nm(document->lines[j].text, document->lines[j].length);
                        size_t colon = nm.find(':');
                        if (colon != std::string::npos) nm.erase(colon);
                        e.name = nm;
                        break;
                    }
                }
            }
            pm.hotlist.push_back(e);
        }
        std::sort(pm.hotlist.begin(), pm.hotlist.end(),
                  [](const PmHotEntry &a, const PmHotEntry &b) { return a.count > b.count; });
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset counts")) apex_pinmame_instrument(pm.port, "clear", 0, -1);
    ImGui::EndDisabled();
    if (pm.hot_instrumented)
        ImGui::TextDisabled("%d routine(s) instrumented; run the game, then Read hotlist",
                            pm.hot_instrumented);

    if (!pm.hotlist.empty() && ImGui::BeginTable("pm_hot", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS, ImVec2(0, 200))) {
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 90.0f, 0);
        ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed, 90.0f, 1);
        ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch, 0.0f, 2);
        ImGui::TableHeadersRow();
        int sc; bool asc;
        if (ui_table_sort(&sc, &asc)) {
            std::stable_sort(pm.hotlist.begin(), pm.hotlist.end(),
                [&](const PmHotEntry &a, const PmHotEntry &b) {
                    int c = 0;
                    if (sc == 0) c = ui_cmp_int(a.count, b.count);
                    else if (sc == 1) c = ui_cmp_u32(((uint32_t)a.bank << 16) | (a.addr & 0xffff),
                                                     ((uint32_t)b.bank << 16) | (b.addr & 0xffff));
                    else c = a.name.compare(b.name);
                    return asc ? c < 0 : c > 0;
                });
        }
        ImGuiListClipper clip;
        clip.Begin((int)pm.hotlist.size());
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const PmHotEntry &e = pm.hotlist[(size_t)row];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char cb[24];
                snprintf(cb, sizeof(cb), "%ld", e.count);
                if (ImGui::Selectable(cb, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    size_t li;
                    if (apex_render_find_line_by_address(document, e.bank, e.addr, &li))
                        select_line(state, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("B%02x_A%04x", e.bank, e.addr & 0xffff);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(e.name.c_str());
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
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS,
            ImVec2(0, 0)))
        return;

    ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed,  48.0f, 0);
    ImGui::TableSetupColumn("T",     ImGuiTableColumnFlags_WidthFixed,  18.0f, 1);
    ImGui::TableSetupColumn("Addr",  ImGuiTableColumnFlags_WidthFixed,  90.0f, 2);
    ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.0f, 3);
    ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                            100.0f, 4);
    ImGui::TableHeadersRow();

    {
        int sort_col; bool sort_asc;
        if (ui_table_sort(&sort_col, &sort_asc) && state->code_candidates.count > 1) {
            std::stable_sort(state->code_candidates.items,
                state->code_candidates.items + state->code_candidates.count,
                [&](const ApexCodeCandidate &a, const ApexCodeCandidate &b) {
                    int c = 0;
                    switch (sort_col) {
                    case 0: c = ui_cmp_int(a.score, b.score); break;
                    case 1: c = ui_cmp_int(a.tier, b.tier); break;
                    case 2: c = ui_cmp_u32(((uint32_t)a.bank<<16)|(a.addr&0xffffu),
                                           ((uint32_t)b.bank<<16)|(b.addr&0xffffu)); break;
                    case 3: c = strcmp(a.preview, b.preview); break;
                    }
                    return sort_asc ? c < 0 : c > 0;
                });
        }
    }

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

/* Scrape "; WARNING[_ACK] <type> bank=0x.. cpu=0x.. rom=0x.. <detail>" comment
   lines from the rendered document into state->warnings for the Warnings panel.
   "; WARNING_ACK" lines are acknowledged warnings (shown green, un-ackable). */
static void rebuild_warnings(const ApexRenderedDocument *d, UiState *s)
{
    s->warnings.clear();
    s->warnings_stale = false;
    if (!d) return;
    static const char PREFIX[]     = "; WARNING ";
    static const char PREFIX_ACK[] = "; WARNING_ACK ";
    const size_t PLEN     = sizeof(PREFIX) - 1;
    const size_t PLEN_ACK = sizeof(PREFIX_ACK) - 1;
    for (size_t i = 0; i < d->line_count; i++) {
        const ApexRenderedLine *l = &d->lines[i];
        /* "; WARNING " and "; WARNING_ACK " diverge at the space/underscore, so
           they are mutually exclusive; test the acked form first. */
        bool acked = (l->length >= PLEN_ACK && memcmp(l->text, PREFIX_ACK, PLEN_ACK) == 0);
        if (!acked && (l->length < PLEN || memcmp(l->text, PREFIX, PLEN) != 0)) continue;
        size_t plen = acked ? PLEN_ACK : PLEN;

        std::string line(l->text, l->length); /* null-terminated working copy */
        const char *p = line.c_str() + plen;

        WarningEntry w;
        w.line_index = i;
        w.has_location = false;
        w.acked = acked;
        w.bank = 0;
        w.cpu_addr = 0;

        const char *sp = strchr(p, ' ');
        w.type.assign(p, sp ? (size_t)(sp - p) : strlen(p));

        unsigned bank = 0, cpu = 0;
        const char *bptr = strstr(p, "bank=0x");
        const char *cptr = strstr(p, "cpu=0x");
        if (bptr && cptr && sscanf(bptr, "bank=0x%x", &bank) == 1 &&
            sscanf(cptr, "cpu=0x%x", &cpu) == 1) {
            w.bank = (uint8_t)bank;
            w.cpu_addr = (uint32_t)cpu;
            w.has_location = true;
        }

        /* detail = text after the last location field (rom=, else cpu=), else
           after the type token */
        const char *anchor = strstr(p, "rom=0x");
        if (!anchor) anchor = cptr;
        const char *detail = NULL;
        if (anchor && (detail = strchr(anchor, ' ')) != NULL) {
            while (*detail == ' ') detail++;
        } else if (sp) {
            detail = sp;
            while (*detail == ' ') detail++;
        }
        if (detail && *detail) w.detail.assign(detail);

        s->warnings.push_back(std::move(w));
    }
}

void render_warnings_view(ApexProject *project,
                          const ApexRenderedDocument **document_ptr,
                          UiState *state)
{
    (void)project;
    const ApexRenderedDocument *document = *document_ptr;

    if (state->warnings_stale) rebuild_warnings(document, state);

    size_t acked_count = 0;
    for (auto &w : state->warnings) if (w.acked) acked_count++;
    size_t active_count = state->warnings.size() - acked_count;

    ImGui::Text("%zu active", active_count);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "%zu acked", acked_count);
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) rebuild_warnings(document, state);
    ImGui::SameLine();
    ImGui::TextDisabled("| click a row to jump");

    if (state->warnings.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("No warnings.");
        return;
    }
    ImGui::Separator();

    if (!ImGui::BeginTable("warn_tbl", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0)))
        return;
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 190.0f);
    ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##ack", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableHeadersRow();

    /* Deferred ack toggle: applied after the loop so we never rerender (which
       rebuilds state->warnings) while iterating it. */
    int      do_toggle = 0;   /* +1 = ack, -1 = un-ack */
    uint8_t  tog_bank = 0;
    uint32_t tog_addr = 0;
    const ImVec4 kAckGreen(0.40f, 0.85f, 0.45f, 1.0f);

    for (size_t i = 0; i < state->warnings.size(); i++) {
        const WarningEntry &w = state->warnings[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (w.acked) ImGui::PushStyleColor(ImGuiCol_Text, kAckGreen);
        /* AllowOverlap so the Ack button (submitted later, in the last column)
           receives clicks instead of this row-spanning Selectable swallowing them. */
        bool clicked = ImGui::Selectable(w.type.c_str(), false,
                                         ImGuiSelectableFlags_SpanAllColumns |
                                         ImGuiSelectableFlags_AllowOverlap);
        if (w.acked) ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(1);
        if (w.has_location) {
            char addr[32];
            snprintf(addr, sizeof(addr), "B%02x_A%04x", w.bank, w.cpu_addr);
            if (w.acked) ImGui::TextColored(kAckGreen, "%s", addr);
            else         ImGui::TextUnformatted(addr);
        } else {
            ImGui::TextDisabled("-");
        }

        ImGui::TableSetColumnIndex(2);
        if (w.acked) ImGui::TextColored(kAckGreen, "%s", w.detail.c_str());
        else         ImGui::TextUnformatted(w.detail.c_str());

        ImGui::TableSetColumnIndex(3);
        if (w.has_location) {
            if (ImGui::SmallButton(w.acked ? "Un-ack" : "Ack")) {
                do_toggle = w.acked ? -1 : 1;
                tog_bank  = w.bank;
                tog_addr  = w.cpu_addr;
            }
        }

        if (clicked) {
            size_t li;
            if (w.has_location &&
                apex_render_find_line_by_address(document, w.bank, w.cpu_addr, &li)) {
                select_line(state, li, 1);
            } else {
                select_line(state, w.line_index, 1);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();

    if (do_toggle > 0) {
        apex_project_add_ack(project, 1, tog_bank, tog_addr);
        state->overlay_dirty = true;
        rerender_and_reselect(project, document_ptr, state, tog_bank, tog_addr);
    } else if (do_toggle < 0) {
        apex_project_remove_ack(project, 1, tog_bank, tog_addr);
        state->overlay_dirty = true;
        rerender_and_reselect(project, document_ptr, state, tog_bank, tog_addr);
    }
}

/* ---- Flow Breaks: places where code flow runs off into non-code ---- */

static const char *block_kind_short(ApexRenderedBlockKind k)
{
    switch (k) {
    case APEX_RENDER_BLOCK_DATA:         return "data";
    case APEX_RENDER_BLOCK_TABLE:        return "table";
    case APEX_RENDER_BLOCK_UNCLASSIFIED: return "unclassified";
    case APEX_RENDER_BLOCK_FREE:         return "fill (0xFF)";
    case APEX_RENDER_BLOCK_SPRITE:       return "sprite";
    default:                             return "non-code";
    }
}

/* Scan the whole document for code blocks whose last instruction does not
   transfer control (no flow-stop, and not a call into a flow-stop tail-call
   helper) yet is immediately followed by a non-code block. */
static void rebuild_flow_breaks(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    s->flow_breaks.clear();
    s->flow_breaks_stale = false;
    if (!d) return;

    const ApexRenderedLine *last_code = nullptr;
    size_t last_code_li = 0;
    for (size_t li = 0; li < d->line_count; li++) {
        const ApexRenderedLine *l = &d->lines[li];
        if (!l->has_location) continue;

        if (l->block_kind == APEX_RENDER_BLOCK_CODE) {
            /* Track the last real instruction, skipping inline payload lines
               (INLINE_*) so `last_code` is the call/branch itself. */
            if (l->kind == APEX_RENDER_LINE_INSTRUCTION && l->length >= 1) {
                const char *t = l->text;
                size_t n = l->length;
                while (n && (*t == ' ' || *t == '\t')) { t++; n--; }
                if (!(n >= 7 && memcmp(t, "INLINE_", 7) == 0)) {
                    last_code = l;
                    last_code_li = li;
                }
            }
            continue;
        }

        /* code -> non-code transition: judge the last code instruction */
        if (last_code && last_code->rom_addr < p->rom.size) {
            const uint8_t *isrc; size_t irem;
            bool clean = false;
            if (project_locate_rom_bytes(p, last_code->bank, last_code->cpu_addr,
                                         &isrc, &irem, NULL)) {
                char mn[32];
                Cpu6809InstrInfo info = cpu6809_disassemble_info(
                    isrc, irem < 8u ? irem : 8u, last_code->cpu_addr, mn, sizeof(mn));
                if (info.flags & CPU6809_FLOW_STOP) {
                    clean = true;
                } else if (info.has_target && (info.flags & CPU6809_CALL)) {
                    /* a call into a flow-stop tail-call helper ends the block */
                    const InlineSignature *sig =
                        inline_signature_for(&p->inline_sigs, last_code->bank, info.target);
                    if (sig && sig->flow_stop) clean = true;
                }
            }
            if (!clean) {
                FlowBreakEntry e;
                e.line_index = last_code_li;
                e.bank = last_code->bank;
                e.cpu_addr = last_code->cpu_addr;
                e.insn.assign(last_code->text, last_code->length);
                /* trim leading indent for display */
                size_t b = e.insn.find_first_not_of(" \t");
                if (b != std::string::npos && b) e.insn.erase(0, b);
                e.next = block_kind_short(l->block_kind);
                s->flow_breaks.push_back(std::move(e));
            }
        }
        last_code = nullptr;
    }
}

void render_flow_breaks_view(ApexProject *project,
                             const ApexRenderedDocument **document_ptr, UiState *state)
{
    const ApexRenderedDocument *document = *document_ptr;
    if (state->flow_breaks_stale) rebuild_flow_breaks(project, document, state);

    ImGui::Text("%zu flow break%s", state->flow_breaks.size(),
                state->flow_breaks.size() == 1 ? "" : "s");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) rebuild_flow_breaks(project, document, state);
    ImGui::SameLine();
    ImGui::TextDisabled("| code that runs into non-code (mis-classification / wrong inline sig)");

    if (state->flow_breaks.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("No flow breaks.");
        return;
    }
    ImGui::Separator();

    if (!ImGui::BeginTable("flowbreaks", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0)))
        return;
    ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Last instruction", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Runs into", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < state->flow_breaks.size(); i++) {
        const FlowBreakEntry &e = state->flow_breaks[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char ab[24]; snprintf(ab, sizeof(ab), "B%02x_A%04x", e.bank, e.cpu_addr);
        if (ImGui::Selectable(ab, false, ImGuiSelectableFlags_SpanAllColumns)) {
            size_t li;
            if (apex_render_find_line_by_address(document, e.bank, e.cpu_addr, &li))
                select_line(state, li, 1);
            else
                select_line(state, e.line_index, 1);
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(e.insn.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "%s", e.next.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/* ---- RAM-map import / export (PinMAME nvram-maps JSON) ---- */

static const ConfigSymbol *nvram_symbol_at(const ApexProject *p, uint32_t addr)
{
    for (size_t i = 0; i < p->symbols.count; i++)
        if (p->symbols.items[i].value == addr) return &p->symbols.items[i];
    return nullptr;
}

static const ConfigSymbol *nvram_symbol_named(const ApexProject *p, const char *name)
{
    for (size_t i = 0; i < p->symbols.count; i++)
        if (p->symbols.items[i].name && strcmp(p->symbols.items[i].name, name) == 0)
            return &p->symbols.items[i];
    return nullptr;
}

static const ConfigDoc *nvram_doc_at(const ApexProject *p, uint32_t addr)
{
    for (size_t i = 0; i < p->docs.count; i++)
        if (p->docs.items[i].addr == addr) return &p->docs.items[i];
    return nullptr;
}

int nvram_prepare_import(ApexProject *project, UiState *state, const char *json_path,
                        std::string *err)
{
    FILE *f = fopen(json_path, "rb");
    if (!f) { if (err) *err = std::string("cannot read ") + json_path; return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); if (err) *err = "cannot size file"; return 1; }
    std::string text((size_t)sz, '\0');
    size_t got = fread(&text[0], 1, (size_t)sz, f);
    fclose(f);
    text.resize(got);

    ApexNvramLocs locs;
    char emsg[128] = {0};
    if (apex_nvram_parse_json(text.data(), text.size(), 0, &locs, emsg, sizeof(emsg)) != 0) {
        if (err) *err = emsg;
        return 1;
    }
    state->nvram_import_rows.clear();
    for (size_t i = 0; i < locs.count; i++) {
        NvramImportRow r;
        r.name = locs.items[i].name ? locs.items[i].name : "";
        r.addr = locs.items[i].addr;
        r.doc  = locs.items[i].doc ? locs.items[i].doc : "";
        r.selected = true;
        r.overwrites = false;
        const ConfigSymbol *sa = nvram_symbol_at(project, r.addr);
        const ConfigSymbol *sn = nvram_symbol_named(project, r.name.c_str());
        const ConfigDoc *da = nvram_doc_at(project, r.addr);
        if (sa && sa->name && r.name != sa->name) {
            r.overwrites = true;
            r.conflict = std::string("addr has symbol ") + sa->name;
        } else if (sn && sn->value != r.addr) {
            r.overwrites = true;
            char b[48]; snprintf(b, sizeof(b), "name used at 0x%04x", sn->value & 0xffffu);
            r.conflict = b;
        } else if (da && da->text && r.doc != da->text) {
            r.overwrites = true;
            r.conflict = "addr has a different doc";
        }
        state->nvram_import_rows.push_back(std::move(r));
    }
    apex_nvram_locs_free(&locs);
    state->show_nvram_import = true;
    return 0;
}

void render_nvram_import_window(ApexProject *project,
                                const ApexRenderedDocument **document_ptr, UiState *state)
{
    auto &rows = state->nvram_import_rows;
    size_t sel = 0, conflicts = 0;
    for (auto &r : rows) { if (r.selected) sel++; if (r.overwrites) conflicts++; }

    ImGui::Text("%zu location%s", rows.size(), rows.size() == 1 ? "" : "s");
    ImGui::SameLine();
    if (conflicts) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                      "| %zu would overwrite existing entries", conflicts);
    if (ImGui::SmallButton("Select all"))  for (auto &r : rows) r.selected = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Select none")) for (auto &r : rows) r.selected = false;
    ImGui::SameLine();
    if (ImGui::SmallButton("Only non-conflicting"))
        for (auto &r : rows) r.selected = !r.overwrites;
    ImGui::Separator();

    if (ImGui::BeginTable("nvimp", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
            ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4.0f))) {
        ImGui::TableSetupColumn("##sel", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Doc / conflict", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rows.size(); i++) {
            NvramImportRow &r = rows[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            if (r.overwrites) {
                ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.85f, 0.45f, 0.05f, 0.20f));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg);
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##s", &r.selected);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%04x", r.addr & 0xffffu);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(r.doc.c_str());
            if (r.overwrites) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "  [!] %s",
                                   r.conflict.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("Import selected")) {
        int applied = 0;
        for (auto &r : rows) {
            if (!r.selected) continue;
            apex_project_set_symbol(project, r.name.c_str(), r.addr, 1u);
            if (!r.doc.empty())
                apex_project_set_doc(project, 0, 0, r.addr, r.doc.c_str());
            applied++;
        }
        if (applied) {
            state->overlay_dirty = true;
            state->labels_valid = false;
            rerender_and_reselect(project, document_ptr, state, 0xffu, 0u);
        }
        set_status(state, (std::to_string(applied) + " location(s) imported").c_str());
        state->show_nvram_import = false;
        rows.clear();
    }
    ImGui::SameLine();
    ImGui::Text("(%zu selected)", sel);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        state->show_nvram_import = false;
        rows.clear();
    }
}

int nvram_export(const ApexProject *project, const char *json_path,
                 const char *template_path, std::string *err)
{
    std::vector<ApexNvramLoc> arr;
    std::vector<std::string> gen_names; /* backing store for generated names */
    for (size_t i = 0; i < project->symbols.count; i++) {
        if (project->symbols.items[i].value >= APEX_RAM_LIMIT) continue;
        ApexNvramLoc l;
        l.name = (char *)project->symbols.items[i].name;
        l.addr = project->symbols.items[i].value;
        const ConfigDoc *d = nvram_doc_at(project, l.addr);
        l.doc = d ? d->text : (char *)"";
        arr.push_back(l);
    }
    for (size_t i = 0; i < project->docs.count; i++) {
        uint32_t a = project->docs.items[i].addr;
        if (a >= APEX_RAM_LIMIT || nvram_symbol_at(project, a)) continue;
        char b[16]; snprintf(b, sizeof(b), "RAM_%04x", a & 0xffffu);
        gen_names.push_back(b);
    }
    /* second pass now that gen_names won't reallocate */
    size_t gi = 0;
    for (size_t i = 0; i < project->docs.count; i++) {
        uint32_t a = project->docs.items[i].addr;
        if (a >= APEX_RAM_LIMIT || nvram_symbol_at(project, a)) continue;
        ApexNvramLoc l;
        l.name = (char *)gen_names[gi++].c_str();
        l.addr = a;
        l.doc  = project->docs.items[i].text;
        arr.push_back(l);
    }
    if (arr.empty()) { if (err) *err = "no RAM symbols/docs to export"; return 1; }

    /* Optional zero-loss merge into the originally-imported map.  If a template
       was requested but is no longer readable, fail rather than silently writing
       a fresh (lossy) map — otherwise the caller would wrongly report a merge. */
    std::string tmpl;
    bool want_merge = template_path && template_path[0];
    if (want_merge) {
        FILE *tf = fopen(template_path, "rb");
        if (!tf) {
            if (err) *err = std::string("template not readable: ") + template_path;
            return 1;
        }
        fseek(tf, 0, SEEK_END); long ts = ftell(tf); fseek(tf, 0, SEEK_SET);
        if (ts > 0) { tmpl.resize((size_t)ts); tmpl.resize(fread(&tmpl[0], 1, (size_t)ts, tf)); }
        fclose(tf);
    }

    FILE *f = fopen(json_path, "w");
    if (!f) { if (err) *err = std::string("cannot write ") + json_path; return 1; }
    int rc = 0;
    if (want_merge) {
        char terr[128] = {0};
        rc = apex_nvram_export_merged(f, tmpl.data(), tmpl.size(), arr.data(), arr.size(),
                                      terr, sizeof(terr));
        if (rc != 0 && err) *err = terr;
    } else {
        const char *rom = project->rom_path
            ? (strrchr(project->rom_path, '/') ? strrchr(project->rom_path, '/') + 1
                                               : project->rom_path)
            : nullptr;
        apex_nvram_write_json(f, arr.data(), arr.size(), rom);
    }
    fclose(f);
    return rc;
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
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS,
            ImVec2(0, 0)))
        return;

    ImGui::TableSetupColumn("Score",     ImGuiTableColumnFlags_WidthFixed,  48.0f, 0);
    ImGui::TableSetupColumn("Addr",      ImGuiTableColumnFlags_WidthFixed,  90.0f, 1);
    ImGui::TableSetupColumn("Spec",      ImGuiTableColumnFlags_WidthFixed, 120.0f, 2);
    ImGui::TableSetupColumn("Callsites", ImGuiTableColumnFlags_WidthFixed,  70.0f, 3);
    ImGui::TableSetupColumn("##act",     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                            100.0f, 4);
    ImGui::TableHeadersRow();

    {
        int sort_col; bool sort_asc;
        if (ui_table_sort(&sort_col, &sort_asc) && state->inline_candidates.count > 1) {
            std::stable_sort(state->inline_candidates.items,
                state->inline_candidates.items + state->inline_candidates.count,
                [&](const ApexInlineCandidate &a, const ApexInlineCandidate &b) {
                    int c = 0;
                    switch (sort_col) {
                    case 0: c = ui_cmp_int(a.score, b.score); break;
                    case 1: c = ui_cmp_u32(((uint32_t)a.bank<<16)|(a.addr&0xffffu),
                                           ((uint32_t)b.bank<<16)|(b.addr&0xffffu)); break;
                    case 2: c = strcmp(a.spec, b.spec); break;
                    case 3: c = ui_cmp_int(a.callsite_count, b.callsite_count); break;
                    }
                    return sort_asc ? c < 0 : c > 0;
                });
        }
    }

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
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                           APEX_TABLE_SORT_FLAGS,
                           ImVec2(0, 0))) {
        return;
    }
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100.f, 0);
    ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                            0.0f, 1);
    ImGui::TableSetupColumn("##act",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                            120.f, 2);
    ImGui::TableHeadersRow();

    std::vector<size_t> order(project->ref_exclusions.count);
    for (size_t k = 0; k < order.size(); k++) order[k] = k;
    {
        int sort_col; bool sort_asc;
        if (ui_table_sort(&sort_col, &sort_asc)) {
            std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib) {
                const ConfigEntry *a = &project->ref_exclusions.items[ia];
                const ConfigEntry *b = &project->ref_exclusions.items[ib];
                uint8_t ab = a->has_bank ? a->bank : 0xffu;
                uint8_t bb = b->has_bank ? b->bank : 0xffu;
                int c = ui_cmp_u32(((uint32_t)ab<<16)|(a->addr&0xffffu),
                                   ((uint32_t)bb<<16)|(b->addr&0xffffu));
                return sort_asc ? c < 0 : c > 0;
            });
        }
    }

    int to_remove_idx = -1;
    for (size_t oi = 0; oi < order.size(); oi++) {
        size_t i = order[oi];
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

