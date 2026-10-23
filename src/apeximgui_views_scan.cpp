#include "ImGuiFileDialog.h"
#include "apex_nvram.h"
#include "apeximgui_core.h"
#include "apeximgui_views_internal.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void render_pattern_search(ApexProject *project, const ApexRenderedDocument **document_ptr,
                           UiState *state) {
    if (state->request_focus_pattern_search) {
        ImGui::SetKeyboardFocusHere();
        state->request_focus_pattern_search = 0;
    }
    ImGui::TextUnformatted("Pattern (hex bytes, ?? = wildcard):");
    ImGui::SetNextItemWidth(-ui_w(80.0f));
    bool run = ImGui::InputText("##pat", state->pattern_search_input, 128,
                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    run |= ImGui::Button("Search");
    if (run && state->pattern_search_input[0]) {
        state->pattern_search_results = search_hex_pattern(project, state->pattern_search_input);
        char msg[64];
        snprintf(msg, 64, "%lu match(es)%s", (unsigned long)state->pattern_search_results.size(),
                 state->pattern_search_results.size() >= 500 ? " (capped at 500)" : "");
        set_status(state, msg);
    }
    ImGui::Text("%lu result(s)", (unsigned long)state->pattern_search_results.size());
    if (ImGui::BeginTable("pat_results", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("ROM Offset", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
        ImGui::TableSetupColumn("CPU Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch);
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
                if (ImGui::Selectable(off_buf, has_line && state->selected_line == line_idx,
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
                     UiState *state) {
    (void)project;
    if (state->request_focus_ram_refs) {
        ImGui::SetKeyboardFocusHere();
        state->request_focus_ram_refs = 0;
    }
    ImGui::TextUnformatted("RAM address ($XX, $XXXX, or hex):");
    ImGui::SetNextItemWidth(-ui_w(80.0f));
    bool run = ImGui::InputText("##ramaddr", state->ram_ref_input, 32,
                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    run |= ImGui::Button("Search");
    if (run && state->ram_ref_input[0]) {
        state->ram_ref_query = state->ram_ref_input;
        state->ram_ref_results = find_ram_refs(document, state->ram_ref_input);
        state->ram_ref_gen = document->generation;
        state->ram_ref_ver++;
        char msg[64];
        snprintf(msg, 64, "%lu RAM ref(s) found", (unsigned long)state->ram_ref_results.size());
        set_status(state, msg);
    } else if (!state->ram_ref_query.empty() && state->ram_ref_gen != document->generation) {
        /* The results are line indices: redo the search after a re-render. */
        state->ram_ref_results = find_ram_refs(document, state->ram_ref_query.c_str());
        state->ram_ref_gen = document->generation;
        state->ram_ref_ver++;
    }
    ImGui::Text("%lu result(s)", (unsigned long)state->ram_ref_results.size());
    if (ImGui::BeginTable("ram_results", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                              APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f), 0);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed, ui_w(50.0f), 1);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch, 0.0f, 2);
        ImGui::TableHeadersRow();
        {
            int sort_col;
            bool sort_asc;
            if (ui_table_sort_if_changed(&sort_col, &sort_asc, state->ram_ref_ver)) {
                std::stable_sort(state->ram_ref_results.begin(), state->ram_ref_results.end(),
                                 [&](size_t ia, size_t ib) {
                                     if (ia >= document->line_count || ib >= document->line_count)
                                         return ia < ib;
                                     const ApexRenderedLine *a = &document->lines[ia];
                                     const ApexRenderedLine *b = &document->lines[ib];
                                     int c = 0;
                                     if (sort_col == 0)
                                         c = ui_cmp_u32(
                                             ((uint32_t)a->bank << 16) | (a->cpu_addr & 0xffffu),
                                             ((uint32_t)b->bank << 16) | (b->cpu_addr & 0xffffu));
                                     else if (sort_col == 1)
                                         c = ui_cmp_int(a->block_kind, b->block_kind);
                                     else {
                                         size_t n = a->length < b->length ? a->length : b->length;
                                         c = memcmp(a->text, b->text, n);
                                         if (c == 0)
                                             c = ui_cmp_sz(a->length, b->length);
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
                    snprintf(addr_buf, sizeof(addr_buf), "B%02x_A%04x", line->bank,
                             (unsigned)line->cpu_addr & 0xffff);
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
void render_imm_loads(ApexProject *project, const ApexRenderedDocument **document_ptr,
                      UiState *state) {
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
        if (state->imm_only_rom && !r.tgt_in_rom)
            continue;
        if (state->imm_only_unlabeled && r.symbolic)
            continue;
        view.push_back(i);
    }
    ImGui::Text("%lu shown", (unsigned long)view.size());

    int label_idx = -1; /* index into imm_load_results; label action is deferred */

    if (ImGui::BeginTable("imm_loads", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                              APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Instr", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f), 0);
        ImGui::TableSetupColumn("Op", ImGuiTableColumnFlags_WidthFixed, ui_w(48.0f), 1);
        ImGui::TableSetupColumn("Imm", ImGuiTableColumnFlags_WidthFixed, ui_w(72.0f), 2);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, ui_w(110.0f), 3);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.0f, 4);
        ImGui::TableHeadersRow();

        {
            int sort_col;
            bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc)) {
                auto &res = state->imm_load_results;
                std::stable_sort(view.begin(), view.end(), [&](size_t ia, size_t ib) {
                    const ImmLoadRef &a = res[ia];
                    const ImmLoadRef &b = res[ib];
                    int c = 0;
                    if (sort_col == 0)
                        c = ui_cmp_u32(((uint32_t)a.bank << 16) | (a.cpu_addr & 0xffffu),
                                       ((uint32_t)b.bank << 16) | (b.cpu_addr & 0xffffu));
                    else if (sort_col == 1)
                        c = strcmp(a.mnem, b.mnem);
                    else if (sort_col == 2)
                        c = ui_cmp_u32(a.imm, b.imm);
                    else if (sort_col == 3)
                        c = ui_cmp_u32(((uint32_t)a.tgt_bank << 16) | (a.imm & 0xffffu),
                                       ((uint32_t)b.tgt_bank << 16) | (b.imm & 0xffffu));
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
                snprintf(addr_buf, sizeof(addr_buf), "B%02x_A%04x##i", r.bank,
                         (unsigned)r.cpu_addr & 0xffff);
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
                        const uint8_t *tsrc;
                        size_t tlen, tro = 0;
                        if (project_locate_rom_bytes(project, r.tgt_bank, r.imm, &tsrc, &tlen,
                                                     &tro)) {
                            state->hex_selected_offset = tro;
                            state->hex_active = true;
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
            case APEX_RENDER_BLOCK_CODE:
                prefix = "Sub_";
                break;
            case APEX_RENDER_BLOCK_DATA:
                prefix = "Dat_";
                break;
            case APEX_RENDER_BLOCK_SPRITE:
                prefix = "Spr_";
                break;
            case APEX_RENDER_BLOCK_TABLE:
                prefix = "Tab_";
                break;
            case APEX_RENDER_BLOCK_UNCLASSIFIED:
                prefix = "Unc_";
                break;
            default:
                prefix = "Loc_";
                break;
            }
        }
        char name[32];
        /* Include the bank for paged targets so the name is unique across banks
           (the same CPU address exists in every paged bank). */
        if (has_bank) {
            snprintf(name, sizeof(name), "%s%02X_%04X", prefix, (unsigned)r.tgt_bank,
                     (unsigned)r.imm);
        } else {
            snprintf(name, sizeof(name), "%s%04X", prefix, (unsigned)r.imm);
        }
        if (apex_project_set_label(project, has_bank, r.tgt_bank, r.imm, name) == 0) {
            uint8_t nb = r.tgt_bank;
            uint32_t na = r.imm;
            uint8_t ib = r.bank;
            uint32_t ia = r.cpu_addr;
            /* Optimistically flag every immediate that points at this target as
               symbolic — the async re-render will show the new label; we can't
               re-scan against the freshly rendered document yet. */
            for (auto &e : state->imm_load_results)
                if (e.tgt_in_rom && e.tgt_bank == nb && e.imm == na)
                    e.symbolic = 1;
            rerender_and_reselect(project, document_ptr, state, ib, ia);
            set_status(state, name);
        }
    }
}

/* Change Log: a session audit trail of every recorded edit (classify, label,
   clear, doc, …), newest first, fed by the ApexProject change listener.  Clicking
   an address jumps the disassembly there. */
void render_change_log(const ApexRenderedDocument *document, UiState *state) {
    ImGui::Text("%lu change(s)", (unsigned long)state->change_log.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        state->change_log.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("(this session; not saved)");

    if (state->change_log.empty()) {
        ImGui::TextDisabled("No edits yet — classify, label or clear something.");
        return;
    }

    if (ImGui::BeginTable("change_log", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ui_w(54.0f));
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, ui_w(120.0f));
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 0.0f);
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
   start as CODE.  Non-code runs are dynamic evidence of missed code.
   The fetch (one HTTP request per bank window) runs on pm.cov_job; the result is
   turned into runs by pinmame_finish_coverage_import on the UI thread. */
static void pinmame_start_coverage_import(const ApexProject *p, PinmameState &pm) {
    std::vector<uint8_t> bank_ids;
    for (size_t i = 0; i < p->banks; i++)
        bank_ids.push_back(bank_id_for_index(p->banks, (int)i));
    int port = pm.port;
    PinmameState *pmp = &pm;
    pm.status = "importing coverage...";
    pm.cov_job.start([pmp, port, bank_ids] {
        std::vector<std::pair<uint8_t, uint32_t>> reached;
        long executed = 0, addressable = 0;
        apex_pinmame_coverage_summary(port, executed, addressable);
        /* Collected per bank in ascending address order so contiguous runs are
           already adjacent. */
        for (uint8_t bid : bank_ids) {
            if (pmp->cov_job.cancelled())
                return;
            std::vector<uint8_t> f;
            if (apex_pinmame_coverage_window(port, bid, 0x4000u, 0x4000u, f)) {
                for (size_t o = 0; o < f.size(); o++) {
                    if (f[o])
                        reached.push_back({bid, 0x4000u + (uint32_t)o});
                }
            }
        }
        std::vector<uint8_t> sysf;
        if (apex_pinmame_coverage_window(port, -1, 0x8000u, 0x8000u, sysf)) {
            for (size_t o = 0; o < sysf.size(); o++) {
                if (sysf[o])
                    reached.push_back({0xffu, 0x8000u + (uint32_t)o});
            }
        }
        pmp->cov_fetched.swap(reached);
        pmp->cov_fetched_executed = executed;
        pmp->cov_fetched_addressable = addressable;
    });
}

static void pinmame_finish_coverage_import(const ApexRenderedDocument *doc, PinmameState &pm) {
    const std::vector<std::pair<uint8_t, uint32_t>> &reached = pm.cov_fetched;
    pm.cov_runs.clear();
    pm.cov_reached.clear();
    pm.cov_executed = pm.cov_fetched_executed;
    pm.cov_addressable = pm.cov_fetched_addressable;

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
                     doc->lines[li].block_kind == APEX_RENDER_BLOCK_CODE)
                        ? 1
                        : 0;
        pm.cov_runs.push_back(r);
        i = j;
    }
    /* Per-address reached set for the disassembly overlay (O(1) lookup). */
    for (const std::pair<uint8_t, uint32_t> &pr : reached) {
        pm.cov_reached.insert(((uint32_t)pr.first << 16) | (pr.second & 0xffff));
    }
    pm.cov_fetched.clear();
    pm.cov_diff_dirty = true;
    char msg[128];
    snprintf(msg, sizeof(msg), "coverage: %ld executed, %zu run(s)", pm.cov_executed,
             pm.cov_runs.size());
    pm.status = msg;
}

/* Name of the routine containing `addr` — the nearest code label at or before it
   in the same bank.  Empty if none.  Shared by the RAM-xref, backtrace and
   call-stack features. */
static std::string pm_routine_name(const ApexRenderedDocument *doc, uint8_t bank, uint32_t addr) {
    size_t li;
    if (!doc || !apex_render_find_line_by_address(doc, bank, addr, &li)) {
        return "";
    }
    for (size_t j = li + 1; j-- > 0;) {
        const ApexRenderedLine *l = &doc->lines[j];
        if (l->has_location && l->bank != bank)
            break; /* left the bank */
        if (l->kind == APEX_RENDER_LINE_LABEL && l->has_location && l->bank == bank) {
            std::string nm(l->text, l->length);
            size_t c = nm.find(':');
            if (c != std::string::npos)
                nm.erase(c);
            return nm;
        }
    }
    return "";
}

/* Bounded substring test within one rendered line (text is not NUL-terminated). */
static bool pm_line_has(const ApexRenderedLine *l, const char *needle) {
    size_t nl = strlen(needle);
    if (l->length < nl)
        return false;
    for (size_t i = 0; i + nl <= l->length; i++) {
        if (memcmp(l->text + i, needle, nl) == 0)
            return true;
    }
    return false;
}

/* True if (bank,v) is a return address: an instruction boundary whose preceding
   instruction is a JSR/BSR call. */
static bool pm_is_return_addr(const ApexRenderedDocument *doc, uint8_t bank, uint32_t v) {
    size_t li;
    if (!apex_render_find_line_by_address(doc, bank, v, &li))
        return false;
    /* A label/comment line may sit at v before the instruction — find the instr. */
    bool is_instr = false;
    for (size_t j = li;
         j < doc->line_count && doc->lines[j].bank == bank && doc->lines[j].cpu_addr == v; j++) {
        if (doc->lines[j].kind == APEX_RENDER_LINE_INSTRUCTION) {
            is_instr = true;
            break;
        }
    }
    if (!is_instr)
        return false;
    const ApexRenderedLine *prev = nullptr;
    for (size_t j = li; j-- > 0;) {
        const ApexRenderedLine *l = &doc->lines[j];
        if (l->has_location && l->bank != bank)
            break;
        if (l->kind == APEX_RENDER_LINE_INSTRUCTION && l->bank == bank) {
            prev = l;
            break;
        }
    }
    return prev && (pm_line_has(prev, "JSR") || pm_line_has(prev, "BSR"));
}

/* Heuristic call stack: scan the S stack for 16-bit values that are return
   addresses (see pm_is_return_addr).  Innermost first (closest to S).  A paged
   return (0x4000-0x7fff) has an ambiguous bank, so try the live bank first then
   every bank — the WPC task loop keeps stacks shallow, so this is best-effort. */
static void pm_build_callstack(const ApexProject *p, const ApexRenderedDocument *doc,
                               PinmameState &pm) {
    pm.callstack.clear();
    if (!doc)
        return;
    /* The emulator tracks every call: use its stack (innermost first); the
       stack scan below is the fallback for emulators without the endpoint. */
    std::vector<ApexPinmameFrame> frames_api;
    if (apex_pinmame_callstack(pm.port, frames_api)) {
        /* Older emulators do not prune frames by S: show the innermost 32. */
        size_t lo = frames_api.size() > 32 ? frames_api.size() - 32 : 0;
        for (size_t i = frames_api.size(); i-- > lo;) {
            const ApexPinmameFrame &f = frames_api[i];
            uint32_t ret = (uint32_t)f.pc & 0xffffu;
            uint8_t rb = ret >= 0x8000u ? 0xffu : (uint8_t)(f.bank & 0xff);
            uint32_t recv = (uint32_t)f.receiver & 0xffffu;
            uint8_t vb = recv >= 0x8000u ? 0xffu : (uint8_t)(f.bank & 0xff);
            std::string nm = pm_routine_name(doc, rb, ret);
            std::string rn = pm_routine_name(doc, vb, recv);
            if (!rn.empty())
                nm += (nm.empty() ? "-> " : " -> ") + rn;
            pm.callstack.push_back({rb, ret, 0, nm, -1});
        }
        return;
    }
    unsigned sp = (unsigned)pm.cpu.sp & 0xffffu;
    if (sp == 0)
        return;
    std::vector<uint8_t> buf;
    if (!apex_pinmame_memory(pm.port, sp, 64, -1, buf))
        return;
    int frames = 0;
    for (size_t off = 0; off + 1 < buf.size() && frames < 16; off++) {
        unsigned v = ((unsigned)buf[off] << 8) | buf[off + 1];
        if (v < 0x4000u)
            continue; /* RAM — not a code return address */
        uint8_t hit = 0;
        bool found = false;
        if (v >= 0x8000u) {
            found = pm_is_return_addr(doc, 0xffu, v);
            hit = 0xffu;
        } else {
            uint8_t cur = (uint8_t)pm.info.wpc_bank;
            if (pm_is_return_addr(doc, cur, v)) {
                found = true;
                hit = cur;
            }
            for (size_t bi = 0; !found && bi < p->banks; bi++) {
                uint8_t b = bank_id_for_index(p->banks, (int)bi);
                if (b == cur)
                    continue;
                if (pm_is_return_addr(doc, b, v)) {
                    found = true;
                    hit = b;
                }
            }
        }
        if (!found)
            continue;
        pm.callstack.push_back({hit, v, 0, pm_routine_name(doc, hit, v), -1});
        frames++;
        off++; /* consumed a 2-byte return address */
    }
}

/* Add or bump an accessor/PC entry (bank,addr) in a PmHotEntry list. */
static void pm_bump(std::vector<PmHotEntry> &v, uint8_t bank, uint32_t addr,
                    const std::string &name) {
    for (PmHotEntry &e : v) {
        if (e.bank == bank && e.addr == addr) {
            e.count++;
            return;
        }
    }
    v.push_back({bank, addr, 1, name, -1});
}

/* Parse the switch-control mini-script (one op per line) into PmScriptOp[]. */
static void pm_parse_script(const char *src, std::vector<PmScriptOp> &ops) {
    ops.clear();
    std::string s(src);
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        std::string line = s.substr(i, (e == std::string::npos) ? std::string::npos : e - i);
        i = (e == std::string::npos) ? s.size() : e + 1;
        char kw[16] = {0};
        int a = 0, b = 0;
        if (sscanf(line.c_str(), " %15s %d %d", kw, &a, &b) < 1)
            continue;
        for (char *c = kw; *c; c++) {
            if (*c >= 'A' && *c <= 'Z')
                *c = (char)(*c + 32);
        }
        if (kw[0] == '#' || kw[0] == 0)
            continue;
        PmScriptOp op{};
        if (!strcmp(kw, "press")) {
            op.kind = PM_OP_PRESS;
            op.a = a;
        } else if (!strcmp(kw, "release")) {
            op.kind = PM_OP_RELEASE;
            op.a = a;
        } else if (!strcmp(kw, "pulse")) {
            op.kind = PM_OP_PULSE;
            op.a = a;
            op.b = b;
        } else if (!strcmp(kw, "wait")) {
            op.kind = PM_OP_WAIT;
            op.b = a;
        } else if (!strcmp(kw, "resume")) {
            op.kind = PM_OP_RESUME;
        } else if (!strcmp(kw, "pause")) {
            op.kind = PM_OP_PAUSE;
        } else
            continue;
        ops.push_back(op);
    }
}

/* RAM xref: fold new memory-trace log entries into the accessor list.  With
   a `total` (newer emulators) only the entries since the last read are new;
   otherwise the log is consumed and the range traced again. */
static void pm_poll_xref_trace(const ApexRenderedDocument *doc, PinmameState &pm) {
    std::vector<ApexPinmameAccess> log;
    long total = 0;
    bool has_total = false;
    if (!apex_pinmame_trace_read(pm.port, log, total, has_total))
        return;
    size_t first = 0;
    if (has_total) {
        long fresh = total - pm.xref_total;
        if (fresh < 0)
            fresh = (long)log.size();
        first = (size_t)fresh >= log.size() ? 0 : log.size() - (size_t)fresh;
        pm.xref_total = total;
    } else {
        unsigned a = (unsigned)strtoul(pm.xref_addr, nullptr, 16);
        apex_pinmame_trace_clear(pm.port);
        apex_pinmame_trace_add(pm.port, a, 1, pm.xref_mode, -1);
    }
    for (size_t i = first; i < log.size(); i++) {
        const ApexPinmameAccess &a = log[i];
        /* older emulators log every access whatever the mode: filter here */
        if ((a.write && !(pm.xref_mode & 2)) || (!a.write && !(pm.xref_mode & 1)))
            continue;
        uint8_t b = (a.pc >= 0x8000) ? 0xffu : (uint8_t)(a.bank & 0xff);
        uint32_t pc = (uint32_t)a.pc & 0xffffu;
        PmAccessEntry *e = nullptr;
        for (PmAccessEntry &x : pm.xref_accessors) {
            if (x.bank == b && x.addr == pc) {
                e = &x;
                break;
            }
        }
        if (!e) {
            PmAccessEntry n;
            n.bank = b;
            n.addr = pc;
            n.name = pm_routine_name(doc, b, pc);
            pm.xref_accessors.push_back(n);
            e = &pm.xref_accessors.back();
        }
        if (a.write) {
            e->writes++;
            if (a.has_value) {
                e->has_value = true;
                e->last_value = a.value;
            }
        } else {
            e->reads++;
        }
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
void pinmame_pump(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state) {
    const ApexRenderedDocument *document = *document_ptr;
    PinmameState &pm = state->pinmame;

    /* An attached (external) emulator counts as alive; the poll below decides
       whether it actually answers. */
    pm.alive = pm.external || apex_pinmame_is_alive(pm.pm);
    if (pm.alive) {
        if (pm.connected && !apex_pinmame_events_active(pm.pm))
            apex_pinmame_events_start(pm.pm);
        ApexPinmameHalt h;
        if (apex_pinmame_get_halt(pm.pm, h) && h.seq != pm.last_halt_seq) {
            pm.last_halt = h;
            pm.last_halt_seq = h.seq;
            if (pm.jmp_collecting && h.reason == "bp" && (uint32_t)h.pc == pm.jmp_pc) {
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
                            (uint32_t)h.addr <
                                (uint32_t)pt.addr + (uint32_t)(pt.len > 0 ? pt.len : 1)) {
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
        if (pm.xref_collecting && pm.connected && now >= pm.xref_next_poll) {
            pm.xref_next_poll = now + 0.25;
            pm_poll_xref_trace(document, pm);
        }
        if (now >= pm.next_poll && !pm.poll_job.running()) {
            pm.next_poll = now + 0.5;
            std::vector<std::pair<uint32_t, int>> keys;
            for (const PmWatch &w : pm.watches)
                keys.push_back({w.addr, w.size});
            int port = pm.port;
            PinmameState::PollResult *out = &pm.poll_result;
            pm.poll_job.start([out, port, keys] {
                PinmameState::PollResult r;
                r.connected = apex_pinmame_get_info(port, r.info);
                if (r.connected) {
                    apex_pinmame_get_cpu0(port, r.cpu);
                    apex_pinmame_points(port, r.points);
                    apex_pinmame_switches(port, r.switches);
                    r.watch_keys = keys;
                    for (const auto &k : keys) {
                        std::vector<uint8_t> v;
                        apex_pinmame_memory(port, k.first, k.second, -1, v); /* RAM: no bank */
                        r.watch_vals.push_back(v);
                    }
                }
                *out = std::move(r);
            });
        }
        if (pm.poll_job.finished()) {
            PinmameState::PollResult &r = pm.poll_result;
            if (!r.connected && pm.external && pm.launching) {
                /* Connect found nobody on the port: give up instead of "alive". */
                pm.external = false;
                pm.launching = false;
                pm.status =
                    "connect failed: no debugger answering on port " + std::to_string(pm.port);
            }
            pm.connected = r.connected;
            pm.info = r.info;
            if (pm.connected) {
                pm.cpu = r.cpu;
                pm.points.swap(r.points);
                pm.switches.swap(r.switches);
                /* Watches may have been added/removed meanwhile: match by key. */
                for (PmWatch &w : pm.watches) {
                    for (size_t k = 0; k < r.watch_keys.size(); k++) {
                        if (r.watch_keys[k].first == w.addr && r.watch_keys[k].second == w.size) {
                            w.val = r.watch_vals[k];
                            break;
                        }
                    }
                }
                if (pm.info.paused)
                    pm_build_callstack(project, document, pm);
                else
                    pm.callstack.clear();
                if (pm.launching) {
                    pm.launching = false;
                    pm.status = "connected";
                    patch_pinmame_connected(state); /* data-op traces, before resuming */
                    if (pm.coverage_on_connect) {
                        apex_pinmame_coverage_cmd(pm.port, "clear");
                        apex_pinmame_coverage_cmd(pm.port, "start");
                        pm.coverage_on_connect = false;
                    }
                    if (pm.resume_on_connect) {
                        apex_pinmame_control(pm.port, "resume");
                        pm.resumed = true;
                    }
                }
                pm.resume_on_connect = false;
            }
        }
        if ((pm.show_dmd || pm.dmd_recording) && pm.connected && !pm.dmd_job.running()) {
            double dnow = ImGui::GetTime();
            if (dnow >= pm.dmd_next) {
                pm.dmd_next = dnow + 0.12; /* ~8 Hz refresh */
                int port = pm.port;
                PinmameState::DmdResult *out = &pm.dmd_result;
                pm.dmd_job.start([out, port] {
                    PinmameState::DmdResult r;
                    r.got = apex_pinmame_dmd(port, r.w, r.h, r.lum);
                    *out = std::move(r);
                });
            }
        }
        if (pm.dmd_job.finished() && pm.dmd_result.got) {
            double dnow = ImGui::GetTime();
            pm.dmd_w = pm.dmd_result.w;
            pm.dmd_h = pm.dmd_result.h;
            pm.dmd_lum.swap(pm.dmd_result.lum);
            /* Append to the GIF only when the frame actually changed; write the
               PREVIOUS frame with the time it was on screen (one-frame delay). */
            if (pm.dmd_recording && pm.dmd_gif &&
                pm.dmd_lum.size() != (size_t)pm.dmd_gif_w * (size_t)pm.dmd_gif_h) {
                /* apex_gif_add_frame reads exactly w*h bytes */
                pinmame_stop_gif(state);
                pm.status = "GIF recording stopped: DMD size changed";
            } else if (pm.dmd_recording && pm.dmd_gif && !pm.dmd_lum.empty() &&
                       pm.dmd_lum != pm.dmd_pending) {
                if (pm.dmd_has_pending) {
                    int delay = (int)((dnow - pm.dmd_pending_time) * 100.0 + 0.5);
                    if (delay < 2)
                        delay = 2;
                    if (delay > 6000)
                        delay = 6000;
                    if (!apex_gif_add_frame(pm.dmd_gif, pm.dmd_pending.data(), delay)) {
                        pinmame_stop_gif(state);
                        pm.status = "GIF recording stopped: write error";
                    } else {
                        pm.dmd_rec_frames++;
                    }
                }
                pm.dmd_pending = pm.dmd_lum;
                pm.dmd_pending_time = dnow;
                pm.dmd_has_pending = true;
            }
        }
    } else {
        if (pm.launching) {
            std::string tail = apex_pinmame_log_tail(pm.pm);
            pm.status = "xpinmamed exited before responding" +
                        (tail.empty() ? std::string(" (no log output)") : (": " + tail));
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
                    case PM_OP_PRESS:
                        apex_pinmame_input(pm.port, op.a, 1, 0);
                        break;
                    case PM_OP_RELEASE:
                        apex_pinmame_input(pm.port, op.a, 0, 0);
                        break;
                    case PM_OP_PULSE:
                        apex_pinmame_input(pm.port, op.a, 1, op.b > 0 ? op.b : 100);
                        break;
                    case PM_OP_WAIT:
                        pm.script_next = now + (op.b > 0 ? op.b : 0) / 1000.0;
                        break;
                    case PM_OP_RESUME:
                        apex_pinmame_control(pm.port, "resume");
                        pm.resumed = true;
                        break;
                    case PM_OP_PAUSE:
                        apex_pinmame_control(pm.port, "pause");
                        break;
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
                    UiState *state) {
    const ApexRenderedDocument *document = *document_ptr;
    PinmameState &pm = state->pinmame;
    bool alive = pm.alive;

    /* ---- Config ---- */
    if (ImGui::CollapsingHeader("Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("xpinmamed binary", pm.bin_path, sizeof(pm.bin_path));
        ImGui::InputText("PinMAME rom dir", pm.rompath, sizeof(pm.rompath));
        ImGui::InputText("Game (romset)", pm.game, sizeof(pm.game));
        ImGui::SameLine();
        if (ImGui::Button("Browse…")) {
            IGFD::FileDialogConfig cfg;
            cfg.path = pm.rompath[0] ? pm.rompath : ".";
            ImGuiFileDialog::Instance()->OpenDialog("PmRomZip", "Select PinMAME rom (.zip)", ".zip",
                                                    cfg);
        }
        ImGui::InputInt("HTTP port", &pm.port);
        if (pm.port < 1 || pm.port > 65535)
            pm.port = 8080;
    }
    /* Rom-zip picker: derive rom dir + game name (basename without .zip). */
    center_file_dialog("PmRomZip");
    if (ImGuiFileDialog::Instance()->Display("PmRomZip", ImGuiWindowFlags_NoCollapse,
                                             ImVec2(560, 360))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string dir = ImGuiFileDialog::Instance()->GetCurrentPath();
            std::string name = ImGuiFileDialog::Instance()->GetCurrentFileName();
            snprintf(pm.rompath, sizeof(pm.rompath), "%s", dir.c_str());
            size_t dot = name.rfind(".zip");
            if (dot != std::string::npos)
                name.erase(dot);
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
                if (pm.pm.last_error.find("in use") != std::string::npos)
                    pm.status += " (use Connect to attach to it)";
                pm.launching = false;
            }
        }
        ImGui::SameLine();
        /* Attach to an xpinmamed started outside ApexII (the only way on
           Windows, where auto-launch is not available). */
        if (ImGui::Button("Connect")) {
            apex_pinmame_events_stop(pm.pm); /* reader reads pm.pm.port */
            pm.pm.port = pm.port;            /* the SSE event reader uses it */
            pm.external = true;
            pm.launching = true;
            pm.connected = false;
            pm.next_poll = 0.0;
            pm.status = "connecting to port " + std::to_string(pm.port) + "…";
        }
    } else {
        if (pm.external) {
            if (ImGui::Button("Disconnect")) {
                apex_pinmame_events_stop(pm.pm);
                pm.external = false;
                pm.connected = false;
                pm.resumed = false;
                pm.status = "disconnected";
            }
        } else if (ImGui::Button("Stop")) {
            apex_pinmame_stop(pm.pm);
            pm.connected = false;
            pm.resumed = false;
            pm.status = "stopped";
        }
        ImGui::SameLine();
        if (ImGui::Button("Resume")) {
            apex_pinmame_control(pm.port, "resume");
            pm.resumed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause")) {
            apex_pinmame_control(pm.port, "pause");
            pm.resumed = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Step"))
            apex_pinmame_control(pm.port, "step");
        ImGui::SameLine();
        if (ImGui::Button("Step over"))
            apex_pinmame_control(pm.port, "stepover");
        ImGui::SameLine();
        if (ImGui::Button("Step out"))
            apex_pinmame_control(pm.port, "stepout");
        ImGui::SameLine();
        if (ImGui::Button("Run to selection") && state->selected_line < document->line_count) {
            const ApexRenderedLine *l = &document->lines[state->selected_line];
            if (l->has_location) {
                int bank = (l->cpu_addr >= 0x8000u) ? -1 : (int)l->bank;
                if (apex_pinmame_runto(pm.port, l->cpu_addr, bank))
                    pm.resumed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset"))
            apex_pinmame_control(pm.port, "reset");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reset the machine (F3): NVRAM, breakpoints and pause state stay");
    }

    if (pm.alive && pm.connected) {
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "connected: %s (%s)",
                           pm.info.game.c_str(), pm.info.description.c_str());
        ImGui::Text("state: %s   wpc_bank: 0x%02x", pm.info.paused ? "paused" : "running",
                    pm.info.wpc_bank & 0xff);
        /* Full 6809 register set (frozen & accurate while halted at a breakpoint). */
        const ApexPinmameCpu &c = pm.cpu;
        unsigned d = ((unsigned)(c.a & 0xff) << 8) | (unsigned)(c.b & 0xff);
        char cc[9];
        const char *bits = "EFHINZVC"; /* 6809 CC: E F H I N Z V C (bit 7..0) */
        for (int i = 0; i < 8; i++)
            cc[i] = (c.cc & (0x80 >> i)) ? bits[i] : '.';
        cc[8] = '\0';
        ImGui::TextUnformatted("registers:");
        ImGui::Text("  PC %04lx  S %04lx  U %04lx  X %04lx  Y %04lx", (unsigned long)c.pc & 0xffff,
                    (unsigned long)c.sp & 0xffff, (unsigned long)c.u & 0xffff,
                    (unsigned long)c.x & 0xffff, (unsigned long)c.y & 0xffff);
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
                                            pm.bp_cond[0] ? pm.bp_cond : nullptr, pm.bp_ignore))
                    pm.status = "breakpoint added at selection";
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear BPs"))
        apex_pinmame_breakpoint(pm.port, "clear", 0, -1, nullptr);
    ImGui::SetNextItemWidth(ui_w(160));
    ImGui::InputTextWithHint("##bpcond", "condition e.g. A==7F", pm.bp_cond, sizeof(pm.bp_cond));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_w(90));
    ImGui::InputInt("ignore##bpign", &pm.bp_ignore);
    if (pm.bp_ignore < 0)
        pm.bp_ignore = 0;
    ImGui::SameLine();
    ImGui::TextDisabled("(new breakpoints: REG ==/!=/</> HEX; skip N hits)");
    {
        static char wp_addr[16] = "";
        const char *modes[] = {"read", "write", "rw"};
        int mi = pm.wp_mode - 1;
        if (mi < 0 || mi > 2)
            mi = 1;
        ImGui::SetNextItemWidth(ui_w(90));
        ImGui::InputText("RAM addr (hex)", wp_addr, sizeof(wp_addr));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_w(70));
        ImGui::Combo("##wpmode", &mi, modes, 3);
        pm.wp_mode = mi + 1;
        ImGui::SameLine();
        const char *vconds[] = {"any value", "==", "!=", "<", ">", "<=", ">="};
        static const char *vcond_api[] = {nullptr, "eq", "ne", "lt", "gt", "le", "ge"};
        ImGui::SetNextItemWidth(ui_w(90));
        ImGui::Combo("##wpvcond", &pm.wp_vcond, vconds, 7);
        if (pm.wp_vcond > 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui_w(40));
            ImGui::InputText("##wpval", pm.wp_val, sizeof(pm.wp_val));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Halt only when the byte at the address compares true\n"
                              "(e.g. write == 05: the write that sets it to 5)");
        ImGui::SameLine();
        if (ImGui::Button("Add watch") && wp_addr[0]) {
            unsigned a = (unsigned)strtoul(wp_addr, nullptr, 16);
            apex_pinmame_watchpoint(pm.port, "add", a, -1, 1, pm.wp_mode, vcond_api[pm.wp_vcond],
                                    (unsigned)strtoul(pm.wp_val, nullptr, 16));
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
    if (pm.connected && pm.info.paused && pm.resumed && !pm.xref_collecting && !pm.jmp_collecting) {
        const ApexPinmameHalt &h = pm.last_halt;
        bool have = pm.last_halt_seq != 0;
        uint32_t pc = (uint32_t)(have ? h.pc : pm.cpu.pc);
        uint8_t pcbank =
            (pc >= 0x8000u) ? 0xffu : (uint8_t)(have ? (h.bank & 0xff) : pm.info.wpc_bank);
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

    if (!pm.points.empty() &&
        ImGui::BeginTable("pm_points", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, 90))) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, ui_w(26.0f));
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ui_w(40.0f));
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, ui_w(48.0f));
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ui_w(40.0f));
        ImGui::TableHeadersRow();
        const char *wpmode[] = {"?", "read", "write", "rw"};
        int del_wp = -1, del_idx = -1, tog_wp = -1, tog_idx = -1;
        int prow = 0;
        for (const ApexPinmamePoint &pt : pm.points) {
            ImGui::PushID(prow++);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool on = pt.enabled != 0;
            if (ImGui::Checkbox("##on", &on)) {
                tog_wp = pt.is_wp;
                tog_idx = pt.idx;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(pt.is_wp ? "WP" : pt.temp ? "run" : "BP");
            ImGui::TableSetColumnIndex(2);
            uint8_t db = (pt.bank < 0) ? 0xffu : (uint8_t)pt.bank;
            ImGui::Text("B%02x_A%04x", db, (unsigned)pt.addr & 0xffff);
            ImGui::TableSetColumnIndex(2);
            long hits = pt.hits;
            if (pt.is_wp) { /* watchpoints have no API hit count — use our SSE tally */
                auto it = pm.wp_hits.find((uint32_t)pt.addr & 0xffffu);
                hits = (it != pm.wp_hits.end()) ? it->second : 0;
            }
            ImGui::Text("%ld", hits);
            ImGui::TableSetColumnIndex(4);
            if (pt.is_wp)
                ImGui::TextDisabled("%s%s%s", wpmode[(pt.mode >= 1 && pt.mode <= 3) ? pt.mode : 0],
                                    pt.cond.empty() ? "" : "  value ", pt.cond.c_str());
            else if (!pt.cond.empty() || pt.ignore)
                ImGui::TextDisabled("%s%s%s", pt.cond.c_str(),
                                    pt.ignore ? (pt.cond.empty() ? "ignore " : "  ignore ") : "",
                                    pt.ignore ? std::to_string(pt.ignore).c_str() : "");
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("del")) {
                del_wp = pt.is_wp;
                del_idx = pt.idx;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (del_idx >= 0) {
            apex_pinmame_point_delete(pm.port, del_wp, del_idx);
            pm.next_poll = 0.0; /* refresh the list immediately */
        }
        if (tog_idx >= 0) {
            apex_pinmame_point_cmd(pm.port, "toggle", tog_wp, tog_idx);
            pm.next_poll = 0.0;
        }
    }

    /* ---- Live RAM watch (values resolved to NVRAM symbols) ---- */
    ImGui::SeparatorText("RAM watch");
    ImGui::BeginDisabled(!alive || !pm.connected);
    ImGui::SetNextItemWidth(ui_w(90));
    ImGui::InputText("addr (hex)##watch", pm.watch_addr, sizeof(pm.watch_addr));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_w(90));
    ImGui::Combo("##wsize", &pm.watch_size,
                 "1 byte\0"
                 "2 bytes\0");
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
    if (!pm.watches.empty() &&
        ImGui::BeginTable("pm_watch", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, 110))) {
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(64.0f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, ui_w(120.0f));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ui_w(80.0f));
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
                if (w.size == 2)
                    v = ((unsigned)w.val[0] << 8) | w.val[1]; /* big-endian */
                ImGui::Text(w.size == 2 ? "0x%04x (%u)" : "0x%02x (%u)", v, v);
            } else {
                ImGui::TextDisabled("…");
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::BeginDisabled(!pm.alive || !pm.connected);
            char wpl[16];
            snprintf(wpl, sizeof(wpl), "wp:%s", wml); /* set a watchpoint (current mode) */
            if (ImGui::SmallButton(wpl)) {
                apex_pinmame_watchpoint(pm.port, "add", w.addr, -1, w.size, pm.wp_mode);
                pm.wp_hits[w.addr & 0xffff] = 0;
                pm.status = "watchpoint set on RAM";
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                del = (int)i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (del >= 0)
            pm.watches.erase(pm.watches.begin() + del);
    }

    /* ---- Heuristic call stack (return addresses on the S stack) ---- */
    if (pm.connected && pm.info.paused && !pm.callstack.empty()) {
        ImGui::SeparatorText("Call stack (heuristic)");
        if (ImGui::BeginTable("pm_stack", 2,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV,
                              ImVec2(0, 110))) {
            ImGui::TableSetupColumn("Return", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
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
                                               ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                   ImGuiTableFlags_BordersInnerV,
                                               ImVec2(0, 150))) {
        ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
        ImGui::TableSetupColumn("A B X", ImGuiTableColumnFlags_WidthFixed, ui_w(120.0f));
        ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableHeadersRow();
        int shown = (int)pm.trace.size();
        if (shown > 200)
            shown = 200; /* the immediate lead-up is what matters */
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
        if (mi < 0 || mi > 2)
            mi = 2;
        ImGui::BeginDisabled(!alive || !pm.connected || pm.xref_collecting);
        ImGui::SetNextItemWidth(ui_w(90));
        ImGui::InputText("addr (hex)##xref", pm.xref_addr, sizeof(pm.xref_addr));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_w(70));
        ImGui::Combo("##xrefmode", &mi, modes, 3);
        pm.xref_mode = mi + 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!pm.xref_collecting) {
            ImGui::BeginDisabled(!alive || !pm.connected || pm.xref_addr[0] == 0);
            if (ImGui::Button("Collect accessors")) {
                /* The emulator's memory access trace logs every access while the
                   game keeps running -- no halts, and newer emulators log the
                   written values. */
                unsigned a = (unsigned)strtoul(pm.xref_addr, nullptr, 16);
                apex_pinmame_trace_clear(pm.port);
                apex_pinmame_trace_add(pm.port, a, 1, pm.xref_mode, -1);
                pm.xref_accessors.clear();
                pm.xref_total = 0;
                pm.xref_next_poll = 0;
                pm.xref_collecting = true;
                apex_pinmame_control(pm.port, "resume");
                pm.resumed = true;
            }
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Stop")) {
                pm_poll_xref_trace(document, pm);
                pm.xref_collecting = false;
                apex_pinmame_trace_clear(pm.port);
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "collecting… %zu site(s)",
                               pm.xref_accessors.size());
        }
        if (!pm.xref_accessors.empty() &&
            ImGui::BeginTable("pm_xref", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV,
                              ImVec2(0, 110))) {
            ImGui::TableSetupColumn("Reads", ImGuiTableColumnFlags_WidthFixed, ui_w(56.0f));
            ImGui::TableSetupColumn("Writes", ImGuiTableColumnFlags_WidthFixed, ui_w(56.0f));
            ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, ui_w(40.0f));
            ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
            ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch, 0.0f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < pm.xref_accessors.size(); i++) {
                const PmAccessEntry &e = pm.xref_accessors[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char cb[24];
                snprintf(cb, sizeof(cb), "%ld", e.reads);
                if (ImGui::Selectable(cb, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    size_t li;
                    if (apex_render_find_line_by_address(document, e.bank, e.addr, &li))
                        select_line(state, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%ld", e.writes);
                ImGui::TableSetColumnIndex(2);
                if (e.has_value)
                    ImGui::Text("%02lx", (unsigned long)e.last_value & 0xff);
                else
                    ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("B%02x_A%04x", e.bank, e.addr & 0xffff);
                ImGui::TableSetColumnIndex(4);
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
    ImGui::SetNextItemWidth(ui_w(90));
    ImGui::InputText("jump addr (hex)", pm.jmp_addr, sizeof(pm.jmp_addr));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!pm.jmp_collecting) {
        ImGui::BeginDisabled(!alive || !pm.connected || pm.jmp_addr[0] == 0);
        if (ImGui::Button("Resolve targets")) {
            pm.jmp_pc = (uint32_t)strtoul(pm.jmp_addr, nullptr, 16);
            /* Bank the breakpoint like the disasm: system >=0x8000 → none. */
            /* A paged address needs its bank: take it from the selected line
               when that is the jump, else from the currently mapped bank. */
            int selb = pm.info.wpc_bank;
            if (state->selected_line < document->line_count &&
                document->lines[state->selected_line].cpu_addr == pm.jmp_pc)
                selb = document->lines[state->selected_line].bank;
            pm.jmp_bank = (pm.jmp_pc >= 0x8000u) ? -1 : selb;
            apex_pinmame_points_stash(pm.port, 0, pm.jmp_saved_bps);
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
            apex_pinmame_points_restore(pm.port, 0, pm.jmp_saved_bps);
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "resolving… %zu target(s)",
                           pm.jmp_targets.size());
    }
    if (!pm.jmp_targets.empty() &&
        ImGui::BeginTable("pm_jmp", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, 110))) {
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, ui_w(56.0f));
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
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
static void pm_dmd_palette(uint8_t pal[256 * 3]) {
    for (int i = 0; i < 256; i++) {
        pal[i * 3 + 0] = (uint8_t)i;
        pal[i * 3 + 1] = (uint8_t)(i * 150 / 255);
        pal[i * 3 + 2] = (uint8_t)(i * 20 / 255);
    }
}

/* Finalise an in-progress GIF recording (writes the last pending frame). */
void pinmame_stop_gif(UiState *state) {
    PinmameState &pm = state->pinmame;
    if (pm.dmd_gif) {
        bool ok = true;
        if (pm.dmd_has_pending &&
            pm.dmd_pending.size() == (size_t)pm.dmd_gif_w * (size_t)pm.dmd_gif_h) {
            ok = apex_gif_add_frame(pm.dmd_gif, pm.dmd_pending.data(), 50);
            pm.dmd_rec_frames++;
        }
        ok = apex_gif_end(pm.dmd_gif) && ok;
        pm.dmd_gif = nullptr;
        if (!ok)
            pm.status = "GIF write error: the file may be incomplete";
    }
    pm.dmd_recording = false;
    pm.dmd_has_pending = false;
    pm.dmd_pending.clear();
}

/* ---- PinMAME · DMD window ---- */
void render_pinmame_dmd(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state) {
    (void)project;
    (void)document_ptr;
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
            ImGuiFileDialog::Instance()->OpenDialog("PmDmdGif", "Record DMD to (.gif)", ".gif",
                                                    cfg);
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Stop recording"))
            pinmame_stop_gif(state);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "REC  %ld frame(s)", pm.dmd_rec_frames);
    }
    /* Save-PNG dialog */
    center_file_dialog("PmDmdPng");
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
                            ? "DMD PNG saved"
                            : "PNG save failed";
        }
        ImGuiFileDialog::Instance()->Close();
    }
    /* Record-GIF dialog */
    center_file_dialog("PmDmdGif");
    if (ImGuiFileDialog::Instance()->Display("PmDmdGif", ImGuiWindowFlags_NoCollapse,
                                             ImVec2(560, 360))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
            uint8_t pal[256 * 3];
            pm_dmd_palette(pal);
            int w = pm.dmd_w > 0 ? pm.dmd_w : 128;
            int h = pm.dmd_h > 0 ? pm.dmd_h : 32;
            pm.dmd_gif = apex_gif_begin(path.c_str(), w, h, pal);
            pm.dmd_gif_w = w;
            pm.dmd_gif_h = h;
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
                if (!l)
                    continue;
                float f = l / 255.0f;
                ImU32 col =
                    IM_COL32((int)(255 * f), (int)(150 * f), (int)(20 * f), 255); /* amber */
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
                             UiState *state) {
    (void)project;
    (void)document_ptr;
    PinmameState &pm = state->pinmame;
    ImGui::BeginDisabled(!pm.alive || !pm.connected);
    if (!pm.switches.empty() && ImGui::BeginTable("pm_sw", 4,
                                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                      ImGuiTableFlags_BordersInnerV,
                                                  ImVec2(0, 200))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ui_w(34.0f));
        ImGui::TableSetupColumn("Col/Row", ImGuiTableColumnFlags_WidthFixed, ui_w(54.0f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ui_w(100.0f));
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)pm.switches.size());
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const ApexPinmameSwitch &sw = pm.switches[(size_t)row];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (sw.active)
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%d", sw.num);
                else
                    ImGui::Text("%d", sw.num);
                ImGui::TableSetColumnIndex(1);
                if (sw.col >= 0 && sw.row >= 0)
                    ImGui::Text("%d/%d", sw.col, sw.row);
                else
                    ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(2);
                if (!sw.name.empty())
                    ImGui::TextUnformatted(sw.name.c_str());
                else
                    ImGui::TextDisabled("(unnamed)");
                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton("pulse"))
                    apex_pinmame_input(pm.port, sw.num, 1, 100);
                ImGui::SameLine();
                if (ImGui::SmallButton(sw.active ? "off" : "on"))
                    apex_pinmame_input(pm.port, sw.num, sw.active ? 0 : 1, 0);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "script: press/release/pulse <sw> [ms], wait <ms>, resume, pause  (# = comment)");
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

/* Group the PCs executed in only one of baseline and current import (view 1:
   new since the baseline, view 2: no longer executed) by containing routine,
   so "what runs only when the ball save fires" is a short list of routines. */
static void pm_compute_cov_diff(const ApexRenderedDocument *doc, PinmameState &pm) {
    pm.cov_diff.clear();
    pm.cov_diff_dirty = false;
    pm.cov_diff_doc = doc;
    if (pm.cov_view == 0 || !doc)
        return;
    const std::unordered_set<uint32_t> &from = pm.cov_view == 1 ? pm.cov_reached : pm.cov_baseline;
    const std::unordered_set<uint32_t> &other = pm.cov_view == 1 ? pm.cov_baseline : pm.cov_reached;
    std::vector<uint32_t> keys;
    for (uint32_t k : from) {
        if (!other.count(k))
            keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    std::map<uint32_t, size_t> by_routine;
    for (uint32_t k : keys) {
        uint8_t bank = (uint8_t)(k >> 16);
        uint32_t addr = k & 0xffffu;
        uint32_t raddr = addr;
        std::string name;
        size_t li;
        if (find_routine_start(doc, bank, addr, &li) && doc->lines[li].bank == bank) {
            raddr = doc->lines[li].cpu_addr;
            name.assign(doc->lines[li].text, doc->lines[li].length);
            size_t colon = name.find(':');
            if (colon != std::string::npos)
                name.erase(colon);
        }
        uint32_t rk = ((uint32_t)bank << 16) | raddr;
        auto it = by_routine.find(rk);
        if (it == by_routine.end()) {
            PmDiffRoutine r;
            r.bank = bank;
            r.addr = raddr;
            r.first = addr;
            r.count = 1;
            r.name = name;
            by_routine[rk] = pm.cov_diff.size();
            pm.cov_diff.push_back(r);
        } else {
            pm.cov_diff[it->second].count++;
        }
    }
}

/* ---- PinMAME · Coverage window (coverage import/overlay + call hotlist) ---- */
void render_pinmame_coverage(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state) {
    const ApexRenderedDocument *document = *document_ptr;
    PinmameState &pm = state->pinmame;

    ImGui::SeparatorText("Code coverage");
    ImGui::BeginDisabled(!pm.alive || !pm.connected);
    if (ImGui::Button("Start"))
        apex_pinmame_coverage_cmd(pm.port, "start");
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        apex_pinmame_coverage_cmd(pm.port, "clear");
    ImGui::SameLine();
    ImGui::BeginDisabled(pm.cov_job.running());
    if (ImGui::Button(pm.cov_job.running() ? "Importing..." : "Import"))
        pinmame_start_coverage_import(project, pm);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (pm.cov_job.finished())
        pinmame_finish_coverage_import(document, pm);

    if (pm.cov_addressable > 0) {
        ImGui::Text("executed %ld / %ld bytes (%.1f%%)", pm.cov_executed, pm.cov_addressable,
                    100.0 * (double)pm.cov_executed / (double)pm.cov_addressable);
    }
    ImGui::Checkbox("overlay in disasm", &pm.cov_overlay);

    /* Differential coverage: baseline = one import, diff = a later one. */
    ImGui::SeparatorText("Differential coverage");
    ImGui::BeginDisabled(pm.cov_reached.empty());
    if (ImGui::Button("Set baseline from import")) {
        pm.cov_baseline = pm.cov_reached;
        pm.cov_baseline_executed = (long)pm.cov_reached.size();
        pm.cov_diff_dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(pm.cov_baseline.empty());
    if (ImGui::Button("Drop baseline")) {
        pm.cov_baseline.clear();
        pm.cov_baseline_executed = 0;
        pm.cov_view = 0;
        pm.cov_diff_dirty = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("1. Start + play the reference situation, Import, Set baseline\n"
                          "2. Clear, Start, trigger the feature (switch script), Import\n"
                          "3. View \"new since baseline\": only code the feature ran");
    if (!pm.cov_baseline.empty())
        ImGui::TextDisabled("baseline: %ld instruction(s)", pm.cov_baseline_executed);
    ImGui::BeginDisabled(pm.cov_baseline.empty());
    int prev_view = pm.cov_view;
    ImGui::RadioButton("all runs", &pm.cov_view, 0);
    ImGui::SameLine();
    ImGui::RadioButton("new since baseline", &pm.cov_view, 1);
    ImGui::SameLine();
    ImGui::RadioButton("no longer executed", &pm.cov_view, 2);
    ImGui::EndDisabled();
    if (pm.cov_baseline.empty())
        pm.cov_view = 0;
    if (pm.cov_view != prev_view)
        pm.cov_diff_dirty = true;
    if (pm.cov_overlay && !pm.cov_reached.empty()) {
        if (pm.cov_view == 1)
            ImGui::TextDisabled("disasm: amber = new since baseline, green = executed in both");
        else if (pm.cov_view == 2)
            ImGui::TextDisabled("disasm: purple = executed at baseline only");
        else
            ImGui::TextDisabled("disasm: green = executed, red = never executed");
    }

    if (pm.cov_view != 0) {
        if (pm.cov_diff_dirty || pm.cov_diff_doc != document)
            pm_compute_cov_diff(document, pm);
        int pcs = 0;
        for (const PmDiffRoutine &r : pm.cov_diff)
            pcs += r.count;
        ImGui::Text("%d instruction(s) in %zu routine(s)", pcs, pm.cov_diff.size());
        if (ImGui::BeginTable("pm_cov_diff", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                  APEX_TABLE_SORT_FLAGS,
                              ImVec2(0, 220))) {
            ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f), 1);
            ImGui::TableSetupColumn("PCs", ImGuiTableColumnFlags_WidthFixed, ui_w(50.0f), 2);
            ImGui::TableSetupColumn("First", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f), 3);
            ImGui::TableHeadersRow();
            int sc;
            bool asc;
            if (ui_table_sort(&sc, &asc)) {
                std::stable_sort(
                    pm.cov_diff.begin(), pm.cov_diff.end(),
                    [&](const PmDiffRoutine &a, const PmDiffRoutine &b) {
                        int c = 0;
                        if (sc == 0)
                            c = a.name.compare(b.name);
                        else if (sc == 2)
                            c = ui_cmp_int(a.count, b.count);
                        else {
                            uint32_t ka = ((uint32_t)a.bank << 16) | (sc == 1 ? a.addr : a.first);
                            uint32_t kb = ((uint32_t)b.bank << 16) | (sc == 1 ? b.addr : b.first);
                            c = ui_cmp_u32(ka, kb);
                        }
                        return asc ? c < 0 : c > 0;
                    });
            }
            ImGuiListClipper dclip;
            dclip.Begin((int)pm.cov_diff.size());
            while (dclip.Step()) {
                for (int row = dclip.DisplayStart; row < dclip.DisplayEnd; row++) {
                    const PmDiffRoutine &r = pm.cov_diff[(size_t)row];
                    ImGui::PushID(row);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    char nb[160];
                    snprintf(nb, sizeof(nb), "%s", r.name.empty() ? "(no label)" : r.name.c_str());
                    if (ImGui::Selectable(nb, false, ImGuiSelectableFlags_SpanAllColumns)) {
                        size_t li;
                        if (apex_render_find_line_by_address(document, r.bank, r.first, &li))
                            select_line(state, li, 1);
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("B%02x_A%04x", r.bank, (unsigned)r.addr & 0xffff);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", r.count);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("B%02x_A%04x", r.bank, (unsigned)r.first & 0xffff);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::SeparatorText("Executed runs");
    ImGui::Checkbox("Only runs not classified as code (missed code)", &pm.cov_only_noncode);

    std::vector<size_t> view;
    for (size_t i = 0; i < pm.cov_runs.size(); i++) {
        if (pm.cov_only_noncode && pm.cov_runs[i].is_code)
            continue;
        view.push_back(i);
    }
    ImGui::Text("%zu run(s) shown", view.size());

    if (ImGui::BeginTable("pm_cov", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, 150))) {
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
        ImGui::TableSetupColumn("Len", ImGuiTableColumnFlags_WidthFixed, ui_w(56.0f));
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, ui_w(64.0f));
        ImGui::TableSetupColumn("Go", ImGuiTableColumnFlags_WidthStretch, 0.0f);
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
                if (r.is_code)
                    ImGui::TextDisabled("code");
                else
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "not code");
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
    ImGui::BeginDisabled(pm.hot_job.running());
    if (ImGui::Button(pm.hot_job.running() ? "Instrumenting..." : "Instrument routines")) {
        /* Collect the routine list here (it reads the document); the one HTTP
           request per routine then runs on a worker. */
        const size_t CAP = 2000;
        std::vector<std::pair<unsigned, int>> routines; /* (cpu addr, bank or -1) */
        for (size_t i = 0; i < document->line_count && routines.size() < CAP; i++) {
            const ApexRenderedLine *l = &document->lines[i];
            if (l->kind == APEX_RENDER_LINE_LABEL && l->block_kind == APEX_RENDER_BLOCK_CODE &&
                l->has_location) {
                routines.push_back({l->cpu_addr, (l->cpu_addr >= 0x8000u) ? -1 : (int)l->bank});
            }
        }
        int port = pm.port;
        PinmameState *pmp = &pm;
        pm.hot_job_capped = routines.size() >= CAP;
        pm.status = "instrumenting routines...";
        pm.hot_job.start([pmp, port, routines] {
            int n = 0;
            apex_pinmame_instrument(port, "clear", 0, -1);
            for (const auto &r : routines) {
                if (pmp->hot_job.cancelled())
                    break;
                apex_pinmame_instrument(port, "add", r.first, r.second);
                n++;
            }
            pmp->hot_job_count = n;
        });
    }
    ImGui::EndDisabled();
    if (pm.hot_job.finished()) {
        pm.hot_instrumented = pm.hot_job_count;
        char m[64];
        snprintf(m, sizeof(m), "instrumented %d routine(s)%s", pm.hot_job_count,
                 pm.hot_job_capped ? " (capped)" : "");
        pm.status = m;
    }
    ImGui::SameLine();
    if (ImGui::Button("Read hotlist")) {
        std::vector<ApexPinmameCount> cs;
        apex_pinmame_instrument_read(pm.port, cs);
        pm.hotlist.clear();
        for (const ApexPinmameCount &c : cs) {
            if (c.count <= 0)
                continue;
            PmHotEntry e;
            e.bank = (c.bank < 0) ? 0xffu : (uint8_t)c.bank;
            e.addr = (uint32_t)c.addr;
            e.count = c.count;
            e.base = -1;
            size_t li;
            if (apex_render_find_line_by_address(document, e.bank, e.addr, &li)) {
                for (size_t j = li; j < document->line_count && document->lines[j].bank == e.bank &&
                                    document->lines[j].cpu_addr == e.addr;
                     j++) {
                    if (document->lines[j].kind == APEX_RENDER_LINE_LABEL) {
                        std::string nm(document->lines[j].text, document->lines[j].length);
                        size_t colon = nm.find(':');
                        if (colon != std::string::npos)
                            nm.erase(colon);
                        e.name = nm;
                        break;
                    }
                }
            }
            if (pm.hot_has_baseline) {
                auto it = pm.hot_baseline.find(((uint32_t)e.bank << 16) | (e.addr & 0xffff));
                e.base = it != pm.hot_baseline.end() ? it->second : 0;
            }
            pm.hotlist.push_back(e);
        }
        /* With a baseline, the routines called most since then come first. */
        std::sort(pm.hotlist.begin(), pm.hotlist.end(),
                  [](const PmHotEntry &a, const PmHotEntry &b) {
                      long da = a.base >= 0 ? a.count - a.base : a.count;
                      long db = b.base >= 0 ? b.count - b.base : b.count;
                      return da > db;
                  });
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset counts"))
        apex_pinmame_instrument(pm.port, "clear", 0, -1);
    ImGui::SameLine();
    if (ImGui::Button("Set baseline")) {
        /* Snapshot the current counters; the hotlist then shows the delta. */
        std::vector<ApexPinmameCount> cs;
        apex_pinmame_instrument_read(pm.port, cs);
        pm.hot_baseline.clear();
        for (const ApexPinmameCount &c : cs) {
            uint8_t b = (c.bank < 0) ? 0xffu : (uint8_t)c.bank;
            pm.hot_baseline[((uint32_t)b << 16) | ((uint32_t)c.addr & 0xffff)] = c.count;
        }
        pm.hot_has_baseline = true;
        for (PmHotEntry &e : pm.hotlist) {
            auto it = pm.hot_baseline.find(((uint32_t)e.bank << 16) | (e.addr & 0xffff));
            e.base = it != pm.hot_baseline.end() ? it->second : 0;
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Snapshot the counters now; Read hotlist then shows the calls made "
                          "since (delta column)");
    ImGui::EndDisabled();
    if (pm.hot_has_baseline) {
        ImGui::SameLine();
        if (ImGui::SmallButton("drop")) {
            pm.hot_has_baseline = false;
            pm.hot_baseline.clear();
            pm.hot_only_new = false;
            for (PmHotEntry &e : pm.hotlist)
                e.base = -1;
        }
        ImGui::SameLine();
        ImGui::Checkbox("only routines not called at baseline", &pm.hot_only_new);
    }
    if (pm.hot_instrumented)
        ImGui::TextDisabled("%d routine(s) instrumented; run the game, then Read hotlist",
                            pm.hot_instrumented);

    std::vector<size_t> hview;
    for (size_t i = 0; i < pm.hotlist.size(); i++) {
        if (pm.hot_only_new && pm.hotlist[i].base != 0)
            continue;
        hview.push_back(i);
    }
    if (!pm.hotlist.empty() &&
        ImGui::BeginTable("pm_hot", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                              APEX_TABLE_SORT_FLAGS,
                          ImVec2(0, 200))) {
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f), 0);
        ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_WidthFixed, ui_w(80.0f), 3);
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f), 1);
        ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch, 0.0f, 2);
        ImGui::TableHeadersRow();
        int sc;
        bool asc;
        if (ui_table_sort(&sc, &asc)) {
            std::stable_sort(pm.hotlist.begin(), pm.hotlist.end(),
                             [&](const PmHotEntry &a, const PmHotEntry &b) {
                                 int c = 0;
                                 if (sc == 0)
                                     c = ui_cmp_int(a.count, b.count);
                                 else if (sc == 3)
                                     c = ui_cmp_int(a.base >= 0 ? a.count - a.base : a.count,
                                                    b.base >= 0 ? b.count - b.base : b.count);
                                 else if (sc == 1)
                                     c = ui_cmp_u32(((uint32_t)a.bank << 16) | (a.addr & 0xffff),
                                                    ((uint32_t)b.bank << 16) | (b.addr & 0xffff));
                                 else
                                     c = a.name.compare(b.name);
                                 return asc ? c < 0 : c > 0;
                             });
            hview.clear();
            for (size_t i = 0; i < pm.hotlist.size(); i++) {
                if (pm.hot_only_new && pm.hotlist[i].base != 0)
                    continue;
                hview.push_back(i);
            }
        }
        ImGuiListClipper clip;
        clip.Begin((int)hview.size());
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const PmHotEntry &e = pm.hotlist[hview[(size_t)row]];
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
                if (e.base < 0)
                    ImGui::TextDisabled("-");
                else if (e.base == 0)
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.15f, 1.0f), "+%ld new", e.count);
                else
                    ImGui::Text("+%ld", e.count - e.base);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("B%02x_A%04x", e.bank, e.addr & 0xffff);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(e.name.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void render_code_candidates(ApexProject *project, const ApexRenderedDocument **document_ptr,
                            UiState *state) {
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
        state->code_candidates_ver++;
        state->code_candidates_stale = false;
    }

    if (state->code_candidates.count == 0)
        return;

    ImGui::Separator();

    /* ---- Table ---- */
    if (!ImGui::BeginTable("cand_tbl", 5,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                               APEX_TABLE_SORT_FLAGS,
                           ImVec2(0, 0)))
        return;

    ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, ui_w(48.0f), 0);
    ImGui::TableSetupColumn("T", ImGuiTableColumnFlags_WidthFixed, ui_w(18.0f), 1);
    ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f), 2);
    ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.0f, 3);
    ImGui::TableSetupColumn(
        "##act", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 100.0f, 4);
    ImGui::TableHeadersRow();

    {
        int sort_col;
        bool sort_asc;
        if (ui_table_sort_if_changed(&sort_col, &sort_asc, state->code_candidates_ver) &&
            state->code_candidates.count > 1) {
            std::stable_sort(state->code_candidates.items,
                             state->code_candidates.items + state->code_candidates.count,
                             [&](const ApexCodeCandidate &a, const ApexCodeCandidate &b) {
                                 int c = 0;
                                 switch (sort_col) {
                                 case 0:
                                     c = ui_cmp_int(a.score, b.score);
                                     break;
                                 case 1:
                                     c = ui_cmp_int(a.tier, b.tier);
                                     break;
                                 case 2:
                                     c = ui_cmp_u32(((uint32_t)a.bank << 16) | (a.addr & 0xffffu),
                                                    ((uint32_t)b.bank << 16) | (b.addr & 0xffffu));
                                     break;
                                 case 3:
                                     c = strcmp(a.preview, b.preview);
                                     break;
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
        ImVec4 score_col = c->score >= 80   ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f)
                           : c->score >= 60 ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                                            : ImVec4(0.9f, 0.55f, 0.2f, 1.0f);
        ImGui::TextColored(score_col, "%d", c->score);

        /* tier column */
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%d", c->tier);

        /* address column */
        ImGui::TableSetColumnIndex(2);
        char addr_buf[24];
        snprintf(addr_buf, sizeof(addr_buf), "B%02x_A%04x", c->bank, (unsigned)c->addr & 0xffffu);
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
                    if (dl->has_location && dl->bank == c->bank && dl->cpu_addr <= c->addr) {
                        if (li == (size_t)-1 || dl->cpu_addr > document->lines[li].cpu_addr)
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
            if (apex_project_set_kind(project, 1, c->bank, c->addr, APEX_KIND_CODE, NULL) != 0) {
                set_status(state, "classify as code failed"); /* keep the row */
                ImGui::PopID();
                i++;
                continue;
            }
            state->overlay_dirty = true;
            state->labels_valid = false;
            state->code_candidates_stale = true;
            rerender_and_reselect(project, document_ptr, state, c->bank, c->addr);
            document = *document_ptr;
            /* remove this candidate from the list */
            size_t rem = state->code_candidates.count - i - 1;
            if (rem > 0)
                memmove(&state->code_candidates.items[i], &state->code_candidates.items[i + 1],
                        rem * sizeof(state->code_candidates.items[0]));
            state->code_candidates.count--;
            ImGui::PopID();
            continue;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) {
            size_t rem = state->code_candidates.count - i - 1;
            if (rem > 0)
                memmove(&state->code_candidates.items[i], &state->code_candidates.items[i + 1],
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
static void rebuild_warnings(const ApexRenderedDocument *d, UiState *s) {
    s->warnings.clear();
    s->warnings_stale = false;
    if (!d)
        return;
    static const char PREFIX[] = "; WARNING ";
    static const char PREFIX_ACK[] = "; WARNING_ACK ";
    const size_t PLEN = sizeof(PREFIX) - 1;
    const size_t PLEN_ACK = sizeof(PREFIX_ACK) - 1;
    for (size_t i = 0; i < d->line_count; i++) {
        const ApexRenderedLine *l = &d->lines[i];
        /* "; WARNING " and "; WARNING_ACK " diverge at the space/underscore, so
           they are mutually exclusive; test the acked form first. */
        bool acked = (l->length >= PLEN_ACK && memcmp(l->text, PREFIX_ACK, PLEN_ACK) == 0);
        if (!acked && (l->length < PLEN || memcmp(l->text, PREFIX, PLEN) != 0))
            continue;
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
        if (!anchor)
            anchor = cptr;
        const char *detail = NULL;
        if (anchor && (detail = strchr(anchor, ' ')) != NULL) {
            while (*detail == ' ')
                detail++;
        } else if (sp) {
            detail = sp;
            while (*detail == ' ')
                detail++;
        }
        if (detail && *detail)
            w.detail.assign(detail);

        s->warnings.push_back(std::move(w));
    }
}

void render_warnings_view(ApexProject *project, const ApexRenderedDocument **document_ptr,
                          UiState *state) {
    (void)project;
    const ApexRenderedDocument *document = *document_ptr;

    if (state->warnings_stale)
        rebuild_warnings(document, state);

    size_t acked_count = 0;
    for (auto &w : state->warnings)
        if (w.acked)
            acked_count++;
    size_t active_count = state->warnings.size() - acked_count;

    ImGui::Text("%zu active", active_count);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "%zu acked", acked_count);
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        rebuild_warnings(document, state);
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
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, 0)))
        return;
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ui_w(190.0f));
    ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
    ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##ack", ImGuiTableColumnFlags_WidthFixed, ui_w(70.0f));
    ImGui::TableHeadersRow();

    /* Deferred ack toggle: applied after the loop so we never rerender (which
       rebuilds state->warnings) while iterating it. */
    int do_toggle = 0; /* +1 = ack, -1 = un-ack */
    uint8_t tog_bank = 0;
    uint32_t tog_addr = 0;
    const ImVec4 kAckGreen(0.40f, 0.85f, 0.45f, 1.0f);

    for (size_t i = 0; i < state->warnings.size(); i++) {
        const WarningEntry &w = state->warnings[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (w.acked)
            ImGui::PushStyleColor(ImGuiCol_Text, kAckGreen);
        /* AllowOverlap so the Ack button (submitted later, in the last column)
           receives clicks instead of this row-spanning Selectable swallowing them. */
        bool clicked = ImGui::Selectable(w.type.c_str(), false,
                                         ImGuiSelectableFlags_SpanAllColumns |
                                             ImGuiSelectableFlags_AllowOverlap);
        if (w.acked)
            ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(1);
        if (w.has_location) {
            char addr[32];
            snprintf(addr, sizeof(addr), "B%02x_A%04x", w.bank, w.cpu_addr);
            if (w.acked)
                ImGui::TextColored(kAckGreen, "%s", addr);
            else
                ImGui::TextUnformatted(addr);
        } else {
            ImGui::TextDisabled("-");
        }

        ImGui::TableSetColumnIndex(2);
        if (w.acked)
            ImGui::TextColored(kAckGreen, "%s", w.detail.c_str());
        else
            ImGui::TextUnformatted(w.detail.c_str());

        ImGui::TableSetColumnIndex(3);
        if (w.has_location) {
            if (ImGui::SmallButton(w.acked ? "Un-ack" : "Ack")) {
                do_toggle = w.acked ? -1 : 1;
                tog_bank = w.bank;
                tog_addr = w.cpu_addr;
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

static const char *block_kind_short(ApexRenderedBlockKind k) {
    switch (k) {
    case APEX_RENDER_BLOCK_DATA:
        return "data";
    case APEX_RENDER_BLOCK_TABLE:
        return "table";
    case APEX_RENDER_BLOCK_UNCLASSIFIED:
        return "unclassified";
    case APEX_RENDER_BLOCK_FREE:
        return "fill (0xFF)";
    case APEX_RENDER_BLOCK_SPRITE:
        return "sprite";
    default:
        return "non-code";
    }
}

/* Scan the whole document for code blocks whose last instruction does not
   transfer control (no flow-stop, and not a call into a flow-stop tail-call
   helper) yet is immediately followed by a non-code block. */
static void rebuild_flow_breaks(const ApexProject *p, const ApexRenderedDocument *d, UiState *s) {
    s->flow_breaks.clear();
    s->flow_breaks_stale = false;
    if (!d)
        return;

    const ApexRenderedLine *last_code = nullptr;
    size_t last_code_li = 0;
    for (size_t li = 0; li < d->line_count; li++) {
        const ApexRenderedLine *l = &d->lines[li];
        if (!l->has_location)
            continue;

        if (l->block_kind == APEX_RENDER_BLOCK_CODE) {
            /* Track the last real instruction, skipping inline payload lines
               (INLINE_*) so `last_code` is the call/branch itself. */
            if (l->kind == APEX_RENDER_LINE_INSTRUCTION && l->length >= 1) {
                const char *t = l->text;
                size_t n = l->length;
                while (n && (*t == ' ' || *t == '\t')) {
                    t++;
                    n--;
                }
                if (!(n >= 7 && memcmp(t, "INLINE_", 7) == 0)) {
                    last_code = l;
                    last_code_li = li;
                }
            }
            continue;
        }

        /* code -> non-code transition: judge the last code instruction */
        if (last_code && last_code->rom_addr < p->rom.size) {
            const uint8_t *isrc;
            size_t irem;
            bool clean = false;
            if (project_locate_rom_bytes(p, last_code->bank, last_code->cpu_addr, &isrc, &irem,
                                         NULL)) {
                char mn[32];
                Cpu6809InstrInfo info = cpu6809_disassemble_info(
                    isrc, irem < 8u ? irem : 8u, last_code->cpu_addr, mn, sizeof(mn));
                if (info.flags & CPU6809_FLOW_STOP) {
                    clean = true;
                } else if (info.has_target && (info.flags & CPU6809_CALL)) {
                    /* a call into a flow-stop tail-call helper ends the block */
                    const InlineSignature *sig =
                        inline_signature_for(&p->inline_sigs, last_code->bank, info.target);
                    if (sig && sig->flow_stop)
                        clean = true;
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
                if (b != std::string::npos && b)
                    e.insn.erase(0, b);
                e.next = block_kind_short(l->block_kind);
                s->flow_breaks.push_back(std::move(e));
            }
        }
        last_code = nullptr;
    }
}

void render_flow_breaks_view(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state) {
    const ApexRenderedDocument *document = *document_ptr;
    if (state->flow_breaks_stale)
        rebuild_flow_breaks(project, document, state);

    ImGui::Text("%zu flow break%s", state->flow_breaks.size(),
                state->flow_breaks.size() == 1 ? "" : "s");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload"))
        rebuild_flow_breaks(project, document, state);
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
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, 0)))
        return;
    ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
    ImGui::TableSetupColumn("Last instruction", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Runs into", ImGuiTableColumnFlags_WidthFixed, ui_w(110.0f));
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < state->flow_breaks.size(); i++) {
        const FlowBreakEntry &e = state->flow_breaks[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char ab[24];
        snprintf(ab, sizeof(ab), "B%02x_A%04x", e.bank, e.cpu_addr);
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

static const ConfigSymbol *nvram_symbol_at(const ApexProject *p, uint32_t addr) {
    for (size_t i = 0; i < p->symbols.count; i++)
        if (p->symbols.items[i].value == addr)
            return &p->symbols.items[i];
    return nullptr;
}

static const ConfigSymbol *nvram_symbol_named(const ApexProject *p, const char *name) {
    for (size_t i = 0; i < p->symbols.count; i++)
        if (p->symbols.items[i].name && strcmp(p->symbols.items[i].name, name) == 0)
            return &p->symbols.items[i];
    return nullptr;
}

static const ConfigDoc *nvram_doc_at(const ApexProject *p, uint32_t addr) {
    for (size_t i = 0; i < p->docs.count; i++)
        if (p->docs.items[i].addr == addr)
            return &p->docs.items[i];
    return nullptr;
}

int nvram_prepare_import(ApexProject *project, UiState *state, const char *json_path,
                         std::string *err) {
    FILE *f = fopen(json_path, "rb");
    if (!f) {
        if (err)
            *err = std::string("cannot read ") + json_path;
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        if (err)
            *err = "cannot size file";
        return 1;
    }
    std::string text((size_t)sz, '\0');
    size_t got = fread(&text[0], 1, (size_t)sz, f);
    fclose(f);
    text.resize(got);

    ApexNvramLocs locs;
    char emsg[128] = {0};
    if (apex_nvram_parse_json(text.data(), text.size(), 0, &locs, emsg, sizeof(emsg)) != 0) {
        if (err)
            *err = emsg;
        return 1;
    }
    state->nvram_import_rows.clear();
    for (size_t i = 0; i < locs.count; i++) {
        NvramImportRow r;
        r.name = locs.items[i].name ? locs.items[i].name : "";
        r.addr = locs.items[i].addr;
        r.doc = locs.items[i].doc ? locs.items[i].doc : "";
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
            char b[48];
            snprintf(b, sizeof(b), "name used at 0x%04x", sn->value & 0xffffu);
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

void render_nvram_import_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                                UiState *state) {
    auto &rows = state->nvram_import_rows;
    size_t sel = 0, conflicts = 0;
    for (auto &r : rows) {
        if (r.selected)
            sel++;
        if (r.overwrites)
            conflicts++;
    }

    ImGui::Text("%zu location%s", rows.size(), rows.size() == 1 ? "" : "s");
    ImGui::SameLine();
    if (conflicts)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "| %zu would overwrite existing entries",
                           conflicts);
    if (ImGui::SmallButton("Select all"))
        for (auto &r : rows)
            r.selected = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Select none"))
        for (auto &r : rows)
            r.selected = false;
    ImGui::SameLine();
    if (ImGui::SmallButton("Only non-conflicting"))
        for (auto &r : rows)
            r.selected = !r.overwrites;
    ImGui::Separator();

    if (ImGui::BeginTable("nvimp", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4.0f))) {
        ImGui::TableSetupColumn("##sel", ImGuiTableColumnFlags_WidthFixed, ui_w(24.0f));
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, ui_w(60.0f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, ui_w(170.0f));
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
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "  [!] %s", r.conflict.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("Import selected")) {
        int applied = 0;
        for (auto &r : rows) {
            if (!r.selected)
                continue;
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

int nvram_export(const ApexProject *project, const char *json_path, const char *template_path,
                 std::string *err) {
    std::vector<ApexNvramLoc> arr;
    std::vector<std::string> gen_names; /* backing store for generated names */
    for (size_t i = 0; i < project->symbols.count; i++) {
        if (project->symbols.items[i].value >= APEX_RAM_LIMIT)
            continue;
        ApexNvramLoc l;
        l.name = (char *)project->symbols.items[i].name;
        l.addr = project->symbols.items[i].value;
        const ConfigDoc *d = nvram_doc_at(project, l.addr);
        l.doc = d ? d->text : (char *)"";
        arr.push_back(l);
    }
    for (size_t i = 0; i < project->docs.count; i++) {
        uint32_t a = project->docs.items[i].addr;
        if (a >= APEX_RAM_LIMIT || nvram_symbol_at(project, a))
            continue;
        char b[16];
        snprintf(b, sizeof(b), "RAM_%04x", a & 0xffffu);
        gen_names.push_back(b);
    }
    /* second pass now that gen_names won't reallocate */
    size_t gi = 0;
    for (size_t i = 0; i < project->docs.count; i++) {
        uint32_t a = project->docs.items[i].addr;
        if (a >= APEX_RAM_LIMIT || nvram_symbol_at(project, a))
            continue;
        ApexNvramLoc l;
        l.name = (char *)gen_names[gi++].c_str();
        l.addr = a;
        l.doc = project->docs.items[i].text;
        arr.push_back(l);
    }
    if (arr.empty()) {
        if (err)
            *err = "no RAM symbols/docs to export";
        return 1;
    }

    /* Optional zero-loss merge into the originally-imported map.  If a template
       was requested but is no longer readable, fail rather than silently writing
       a fresh (lossy) map — otherwise the caller would wrongly report a merge. */
    std::string tmpl;
    bool want_merge = template_path && template_path[0];
    if (want_merge) {
        FILE *tf = fopen(template_path, "rb");
        if (!tf) {
            if (err)
                *err = std::string("template not readable: ") + template_path;
            return 1;
        }
        fseek(tf, 0, SEEK_END);
        long ts = ftell(tf);
        fseek(tf, 0, SEEK_SET);
        if (ts > 0) {
            tmpl.resize((size_t)ts);
            tmpl.resize(fread(&tmpl[0], 1, (size_t)ts, tf));
        }
        fclose(tf);
    }

    FILE *f = fopen(json_path, "w");
    if (!f) {
        if (err)
            *err = std::string("cannot write ") + json_path;
        return 1;
    }
    int rc = 0;
    if (want_merge) {
        char terr[128] = {0};
        rc = apex_nvram_export_merged(f, tmpl.data(), tmpl.size(), arr.data(), arr.size(), terr,
                                      sizeof(terr));
        if (rc != 0 && err)
            *err = terr;
    } else {
        const char *rom = project->rom_path ? apex_path_basename(project->rom_path) : nullptr;
        apex_nvram_write_json(f, arr.data(), arr.size(), rom);
    }
    fclose(f);
    return rc;
}

/* Inline detection for the system ("prime") bank: 0xCF 0x7n markers plus the
   return-address walker (see apex_inline_detect.h).  Findings are listed with
   their evidence; "Apply all high" applies the new high-confidence ones as one
   undo step, conflicts with hand-made signatures only on explicit request. */
void render_inline_detect(ApexProject *project, const ApexRenderedDocument **document_ptr,
                          UiState *state) {
    const ApexRenderedDocument *document = *document_ptr;
    ApexInlineFindings &f = state->inline_findings;
    long apply_index = -1;
    bool apply_high = false;

    if (ImGui::Button("Analyse prime bank")) {
        apex_free_inline_findings(&f);
        apex_detect_inline_routines(project, &f);
        state->inline_findings_valid = true;
        state->inline_findings_gen = document->generation;
        state->inline_detect_status[0] = '\0';
    }
    ImGui::SameLine();
    ImGui::Checkbox("show confirmed", &state->inline_detect_show_confirmed);
    if (!state->inline_findings_valid) {
        ImGui::TextDisabled("Finds routines with inline arguments in the system bank: the OS's "
                            "0xCF 0x73+N markers and routines that read their inline bytes "
                            "through the return address.");
        return;
    }
    if (state->inline_findings_gen != document->generation) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "listing changed - analyse again (applied signatures can reveal "
                           "more routines)");
    }
    {
        size_t k, n_new_high = 0, n_new = 0, n_conf = 0, n_ok = 0;
        for (k = 0; k < f.count; k++) {
            const ApexInlineFinding &x = f.items[k];
            if (x.status == APEX_INL_NEW) {
                n_new++;
                if (x.confidence == APEX_INL_HIGH)
                    n_new_high++;
            } else if (x.status == APEX_INL_CONFLICT) {
                n_conf++;
            } else {
                n_ok++;
            }
        }
        ImGui::Text("%zu new (%zu high), %zu conflict(s), %zu confirmed", n_new, n_new_high, n_conf,
                    n_ok);
        ImGui::SameLine();
        ImGui::BeginDisabled(n_new_high == 0);
        if (ImGui::Button("Apply all high (new)")) {
            apply_high = true;
        }
        ImGui::EndDisabled();
    }
    if (state->inline_detect_status[0]) {
        ImGui::TextDisabled("%s", state->inline_detect_status);
    }

    std::vector<size_t> rows;
    for (size_t k = 0; k < f.count; k++) {
        if (f.items[k].status != APEX_INL_CONFIRMED || state->inline_detect_show_confirmed)
            rows.push_back(k);
    }
    if (ImGui::BeginTable("inline_detect", 9,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
        ImGui::TableSetupColumn("Src", ImGuiTableColumnFlags_WidthFixed, ui_w(30.0f));
        ImGui::TableSetupColumn("Len", ImGuiTableColumnFlags_WidthFixed, ui_w(30.0f));
        ImGui::TableSetupColumn("Conf", ImGuiTableColumnFlags_WidthFixed, ui_w(55.0f));
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, ui_w(70.0f));
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, ui_w(50.0f));
        ImGui::TableSetupColumn("Detected", ImGuiTableColumnFlags_WidthFixed, ui_w(170.0f));
        ImGui::TableSetupColumn("Configured / note", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, ui_w(60.0f));
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int)rows.size());
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
                const ApexInlineFinding &x = f.items[rows[(size_t)r]];
                char buf[32];
                ImGui::PushID((int)x.addr);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                snprintf(buf, sizeof(buf), "Bff_A%04x", (unsigned)x.addr);
                if (ImGui::Selectable(buf, false,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    size_t li;
                    if (apex_render_find_line_by_address(document, 0xffu, x.addr, &li))
                        select_line(state, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(x.sources == (APEX_INL_SRC_MARKER | APEX_INL_SRC_WALKER)
                                           ? "MW"
                                       : x.sources == APEX_INL_SRC_MARKER ? "M"
                                                                          : "W");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", x.length);
                ImGui::TableSetColumnIndex(3);
                ImVec4 cc = x.confidence == APEX_INL_HIGH     ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f)
                            : x.confidence == APEX_INL_MEDIUM ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                                                              : ImVec4(0.9f, 0.55f, 0.2f, 1.0f);
                ImGui::TextColored(cc, "%s", apex_inline_confidence_name(x.confidence));
                ImGui::TableSetColumnIndex(4);
                if (x.status == APEX_INL_CONFLICT)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "conflict");
                else
                    ImGui::TextUnformatted(apex_inline_status_name(x.status));
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%d/%d", x.callsites_ok, x.callsites);
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(x.spec);
                ImGui::TableSetColumnIndex(7);
                if (x.config_spec[0])
                    ImGui::Text("[%s] %s", x.config_spec, x.note);
                else
                    ImGui::TextDisabled("%s", x.note);
                ImGui::TableSetColumnIndex(8);
                if (x.status != APEX_INL_CONFIRMED &&
                    ImGui::SmallButton(x.status == APEX_INL_CONFLICT ? "Replace" : "Apply")) {
                    apply_index = (long)rows[(size_t)r];
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (apply_high || apply_index >= 0) {
        int n = apex_apply_inline_findings(project, &f, APEX_INL_HIGH, 0, apply_index);
        uint8_t b = 0xffu;
        uint32_t a = apply_index >= 0 ? f.items[apply_index].addr : 0x8000u;
        if (apply_index < 0)
            selected_address(document, state, &b, &a);
        snprintf(state->inline_detect_status, sizeof(state->inline_detect_status),
                 "applied %d signature(s) - analyse again to find newly reachable routines", n);
        if (n > 0) {
            state->overlay_dirty = true;
            rerender_and_reselect(project, document_ptr, state, b, a);
        }
    }
}

void render_ref_exclusions(ApexProject *project, const ApexRenderedDocument **document_ptr,
                           UiState *state) {
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
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, ui_w(100.f), 0);
    ImGui::TableSetupColumn(
        "Label", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 0.0f, 1);
    ImGui::TableSetupColumn(
        "##act", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 120.f, 2);
    ImGui::TableHeadersRow();

    std::vector<size_t> order(project->ref_exclusions.count);
    for (size_t k = 0; k < order.size(); k++)
        order[k] = k;
    {
        int sort_col;
        bool sort_asc;
        if (ui_table_sort(&sort_col, &sort_asc)) {
            std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib) {
                const ConfigEntry *a = &project->ref_exclusions.items[ia];
                const ConfigEntry *b = &project->ref_exclusions.items[ib];
                uint8_t ab = a->has_bank ? a->bank : 0xffu;
                uint8_t bb = b->has_bank ? b->bank : 0xffu;
                int c = ui_cmp_u32(((uint32_t)ab << 16) | (a->addr & 0xffffu),
                                   ((uint32_t)bb << 16) | (b->addr & 0xffffu));
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
        rerender_and_reselect(project, document_ptr, state, sel_bank, sel_addr);
        state->labels_valid = false;
        state->overlay_dirty = true;
        set_status(state, "ref exclusion removed");
    }
}

/* ---- Game Hooks window ---- */

/* Resolve the detected hook table against the current document: slot/target
   labels and the routine around each OS call site. */
static void hooks_compute(ApexProject *project, const ApexRenderedDocument *doc, UiState *state,
                          HooksWindowState &hs) {
    hs.rows.clear();
    hs.helpers.clear();
    hs.computed = true;
    hs.doc_ptr = doc;
    ApexWpcHooks h;
    if (apex_wpc_detect_hooks(project, &h) != 0)
        return;
    for (size_t k = 0; k < h.helper_count; k++)
        hs.helpers.push_back(h.helpers[k]);
    size_t c = 0;
    for (size_t i = 0; i < h.count; i++) {
        const ApexWpcHook &hk = h.items[i];
        HookRow r;
        r.slot = i;
        r.slot_addr = hk.slot_addr;
        r.target_bank = hk.target_bank;
        r.target_addr = hk.target_addr;
        r.stub = hk.is_stub != 0;
        r.shared = hk.shared;
        r.slot_name = label_at_address(doc, state, 0xffu, hk.slot_addr);
        if (generated_any_label_name(r.slot_name.c_str()))
            r.slot_name.clear();
        r.target_name = label_at_address(doc, state, hk.target_bank, hk.target_addr);
        for (size_t n = 0; n < hk.caller_count; n++, c++) {
            const ApexWpcHookCaller &cl = h.callers[c];
            HookCallerRow cr;
            cr.bank = cl.bank;
            cr.addr = cl.addr;
            cr.kind = cl.kind;
            size_t li;
            if (find_routine_start(doc, cl.bank, cl.addr, &li) && doc->lines[li].bank == cl.bank) {
                cr.routine.assign(doc->lines[li].text, doc->lines[li].length);
                size_t colon = cr.routine.find(':');
                if (colon != std::string::npos)
                    cr.routine.erase(colon);
            }
            r.callers.push_back(cr);
        }
        hs.rows.push_back(r);
    }
    apex_wpc_hooks_free(&h);
    if (hs.selected >= (int)hs.rows.size())
        hs.selected = -1;
}

static void hooks_jump(const ApexRenderedDocument *doc, UiState *state, uint8_t bank,
                       uint32_t addr) {
    size_t li;
    if (apex_render_find_line_by_address(doc, bank, addr, &li))
        select_line(state, li, 1);
}

/* PinMAME state of a hook target: 0 = no coverage, 1 = not executed,
   2 = executed, 3 = executed and new since the coverage baseline. */
static int hook_run_state(const PinmameState &pm, const HookRow &r) {
    if (pm.cov_reached.empty())
        return 0;
    uint32_t key = ((uint32_t)r.target_bank << 16) | (r.target_addr & 0xffff);
    if (!pm.cov_reached.count(key))
        return 1;
    if (!pm.cov_baseline.empty() && !pm.cov_baseline.count(key))
        return 3;
    return 2;
}

void render_hooks_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                         UiState *state) {
    const ApexRenderedDocument *document = *document_ptr;
    HooksWindowState &hs = state->hooks_state;
    if (!document)
        return;
    if (!hs.computed || hs.doc_ptr != document)
        hooks_compute(project, document, state, hs);

    if (hs.rows.empty()) {
        ImGui::TextWrapped("No game-hook table found: the system bank has no run of far "
                           "pointers at 0x8001.");
        return;
    }
    size_t stubs = 0, called = 0;
    for (const HookRow &r : hs.rows) {
        stubs += r.stub ? 1u : 0u;
        called += r.callers.empty() ? 0u : 1u;
    }
    ImGui::Text("Hook table Bff_A%04x: %zu slots, %zu used by the game, %zu unused (RTS)",
                APEX_WPC_HOOK_TABLE_ADDR, hs.rows.size(), hs.rows.size() - stubs, stubs);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The OS calls game code only through these 3-byte far pointers\n"
                          "(JSR far-call helper + inline slot address).  The OS caller tells\n"
                          "what a hook means; redirecting a slot hooks that event without\n"
                          "touching OS code.  Slot order is fixed per OS version.");
    std::string helpers;
    for (uint32_t a : hs.helpers) {
        std::string nm = label_at_address(document, state, 0xffu, a);
        char b[32];
        snprintf(b, sizeof(b), "Bff_A%04x", (unsigned)a);
        helpers += helpers.empty() ? "" : ", ";
        helpers += (nm.empty() || generated_any_label_name(nm.c_str())) ? b : nm;
    }
    ImGui::TextDisabled("far-call helper: %s  |  %zu slot(s) with an OS call site found",
                        helpers.empty() ? "-" : helpers.c_str(), called);
    if (ImGui::Button("Refresh"))
        hooks_compute(project, document, state, hs);
    ImGui::SameLine();
    /* Offer the table only while the slots are unclassified (a hand-made
       per-slot far_code classification must not be overlaid). */
    if (!table_def_at(0xffu, APEX_WPC_HOOK_TABLE_ADDR, &project->tables) &&
        !data_range_at(0xffu, APEX_WPC_HOOK_TABLE_ADDR, &project->data_ranges)) {
        if (ImGui::Button("Classify as far_code table")) {
            char spec[48];
            snprintf(spec, sizeof(spec), "rows[%zu](far_code)", hs.rows.size());
            if (apex_project_set_table(project, 0xffu, APEX_WPC_HOOK_TABLE_ADDR, spec) == 0) {
                state->overlay_dirty = true;
                rerender_and_reselect(project, document_ptr, state, 0xffu,
                                      APEX_WPC_HOOK_TABLE_ADDR);
                set_status(state, "hook table classified");
                return;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Add [tables] Bff_A8001 = rows[N](far_code), so every hook target\n"
                              "becomes a code entry and the slots render as far pointers.");
        ImGui::SameLine();
    }
    ImGui::Checkbox("hide unused (RTS)", &hs.hide_stubs);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_w(160.0f));
    ImGui::InputTextWithHint("##hookfilter", "filter names", hs.filter, sizeof(hs.filter));
    const PinmameState &pm = state->pinmame;
    if (!pm.cov_reached.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_w(170.0f));
        const char *runs[] = {"all slots", "target executed", "new since baseline"};
        ImGui::Combo("##hookrun", &hs.run_filter, runs, pm.cov_baseline.empty() ? 2 : 3);
    } else {
        hs.run_filter = 0;
    }

    std::vector<size_t> view;
    for (size_t i = 0; i < hs.rows.size(); i++) {
        const HookRow &r = hs.rows[i];
        if (hs.hide_stubs && r.stub)
            continue;
        int run = hook_run_state(pm, r);
        if (hs.run_filter == 1 && run < 2)
            continue;
        if (hs.run_filter == 2 && run != 3)
            continue;
        if (hs.filter[0]) {
            bool hit = str_icontains(r.slot_name.c_str(), hs.filter) ||
                       str_icontains(r.target_name.c_str(), hs.filter);
            for (const HookCallerRow &cr : r.callers)
                hit = hit || str_icontains(cr.routine.c_str(), hs.filter);
            if (!hit)
                continue;
        }
        view.push_back(i);
    }

    float detail_h = hs.selected >= 0 ? 190.0f : 0.0f;
    if (ImGui::BeginTable("hooks", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, -detail_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, ui_w(110.0f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("OS call site", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, ui_w(44.0f));
        ImGui::TableSetupColumn("Run", ImGuiTableColumnFlags_WidthFixed, ui_w(44.0f));
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)view.size());
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                size_t ri = view[(size_t)row];
                const HookRow &r = hs.rows[ri];
                ImGui::PushID((int)ri);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char sb[32];
                snprintf(sb, sizeof(sb), "%3zu  %04x", r.slot, (unsigned)r.slot_addr);
                if (ImGui::Selectable(sb, hs.selected == (int)ri,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                    hs.selected = (int)ri;
                    snprintf(hs.name_buf, sizeof(hs.name_buf), "%s", r.slot_name.c_str());
                    if (ImGui::IsMouseDoubleClicked(0))
                        hooks_jump(document, state, r.target_bank, r.target_addr);
                    else
                        hooks_jump(document, state, 0xffu, r.slot_addr);
                }
                ImGui::TableSetColumnIndex(1);
                if (r.slot_name.empty())
                    ImGui::TextDisabled("-");
                else
                    ImGui::TextUnformatted(r.slot_name.c_str());
                ImGui::TableSetColumnIndex(2);
                if (r.stub)
                    ImGui::TextDisabled("RTS (unused)");
                else if (!r.target_name.empty())
                    ImGui::TextUnformatted(r.target_name.c_str());
                else
                    ImGui::Text("B%02x_A%04x", r.target_bank, (unsigned)r.target_addr);
                if (r.shared > 1 && !r.stub && ImGui::IsItemHovered())
                    ImGui::SetTooltip("target shared by %d slots", r.shared);
                ImGui::TableSetColumnIndex(3);
                if (r.callers.empty()) {
                    ImGui::TextDisabled("-");
                } else {
                    const HookCallerRow &cr = r.callers[0];
                    if (cr.routine.empty())
                        ImGui::Text("B%02x_A%04x", cr.bank, (unsigned)cr.addr);
                    else
                        ImGui::TextUnformatted(cr.routine.c_str());
                    if (r.callers.size() > 1 && ImGui::IsItemHovered()) {
                        std::string tip;
                        for (const HookCallerRow &o : r.callers) {
                            char b[24];
                            snprintf(b, sizeof(b), "B%02x_A%04x  ", o.bank, (unsigned)o.addr);
                            tip += b;
                            tip += o.routine.empty() ? "-" : o.routine;
                            tip += "\n";
                        }
                        ImGui::SetTooltip("%s", tip.c_str());
                    }
                }
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%zu", r.callers.size());
                ImGui::TableSetColumnIndex(5);
                switch (hook_run_state(pm, r)) {
                case 1:
                    ImGui::TextDisabled("no");
                    break;
                case 2:
                    ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.40f, 1.0f), "yes");
                    break;
                case 3:
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.15f, 1.0f), "NEW");
                    break;
                default:
                    ImGui::TextDisabled("-");
                    break;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (hs.selected < 0 || hs.selected >= (int)hs.rows.size())
        return;
    const HookRow &r = hs.rows[(size_t)hs.selected];
    ImGui::SeparatorText("Selected hook");
    ImGui::Text("slot %zu at Bff_A%04x  ->  B%02x_A%04x", r.slot, (unsigned)r.slot_addr,
                r.target_bank, (unsigned)r.target_addr);
    ImGui::SameLine();
    if (ImGui::SmallButton("slot"))
        hooks_jump(document, state, 0xffu, r.slot_addr);
    ImGui::SameLine();
    if (ImGui::SmallButton("target"))
        hooks_jump(document, state, r.target_bank, r.target_addr);
    ImGui::SameLine();
    if (ImGui::SmallButton("Redirect to new code (patch)")) {
        patch_redirect_slot(project, state, r.slot_addr, r.target_bank, r.target_addr, r.stub,
                            r.slot_name.empty() ? nullptr : r.slot_name.c_str());
        return;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds a patch: new code in the target's bank that continues at the\n"
                          "original target, and a far pointer at the slot to it");
    ImGui::SetNextItemWidth(ui_w(260.0f));
    bool enter =
        ImGui::InputTextWithHint("##hookname", "slot label, e.g. Hook_EndOfBall", hs.name_buf,
                                 sizeof(hs.name_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Label slot") || enter) && hs.name_buf[0]) {
        uint32_t sa = r.slot_addr;
        if (apex_project_set_label(project, 1, 0xffu, sa, hs.name_buf) == 0) {
            state->overlay_dirty = true;
            rerender_and_reselect(project, document_ptr, state, 0xffu, sa);
            set_status(state, "hook slot labelled");
        } else {
            set_status(state, apex_project_last_error(project));
        }
        return; /* rows are rebuilt against the new document next frame */
    }
    if (ImGui::BeginTable("hook_callers", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Call site", ImGuiTableColumnFlags_WidthFixed, ui_w(100.0f));
        ImGui::TableSetupColumn("How", ImGuiTableColumnFlags_WidthFixed, ui_w(70.0f));
        ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < r.callers.size(); i++) {
            const HookCallerRow &cr = r.callers[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char b[32];
            snprintf(b, sizeof(b), "B%02x_A%04x", cr.bank, (unsigned)cr.addr);
            if (ImGui::Selectable(b, false, ImGuiSelectableFlags_SpanAllColumns))
                hooks_jump(document, state, cr.bank, cr.addr);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cr.kind == APEX_HOOK_CALL_LDX ? "LDX #" : "far call");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(cr.routine.empty() ? "-" : cr.routine.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

/* ---- Argument Values window ---- */

static std::string args_routine_name(const ApexRenderedDocument *doc, uint8_t bank, uint32_t addr) {
    size_t li;
    if (!find_routine_start(doc, bank, addr, &li) || doc->lines[li].bank != bank)
        return "";
    std::string nm(doc->lines[li].text, doc->lines[li].length);
    size_t colon = nm.find(':');
    if (colon != std::string::npos)
        nm.erase(colon);
    return nm;
}

static void args_compute(ApexProject *project, const ApexRenderedDocument *doc, UiState *state,
                         ArgsWindowState &as) {
    std::string keep_type = as.type_idx >= 0 && as.type_idx < (int)as.type_names.size()
                                ? as.type_names[as.type_idx]
                                : "";
    as.sites.clear();
    as.type_names.clear();
    as.values.clear();
    as.values_for = -1;
    as.computed = true;
    as.doc_ptr = doc;
    ApexArgSites raw;
    if (apex_collect_arg_sites(project, 1, &raw) != 0)
        return;
    for (size_t i = 0; i < raw.count; i++) {
        const ApexArgSite &a = raw.items[i];
        ArgSiteRow r;
        r.bank = a.bank;
        r.value_addr = a.value_addr;
        r.call_addr = a.call_addr;
        r.reg = a.reg;
        r.type_name = a.type_name;
        r.value = a.value;
        r.callee = label_at_address(doc, state, a.target_bank, a.target_addr);
        if (r.callee.empty()) {
            char b[24];
            snprintf(b, sizeof(b), "B%02x_A%04x", a.target_bank, (unsigned)a.target_addr);
            r.callee = b;
        }
        r.caller = args_routine_name(doc, a.bank, a.call_addr);
        as.sites.push_back(r);
        if (std::find(as.type_names.begin(), as.type_names.end(), r.type_name) ==
            as.type_names.end())
            as.type_names.push_back(r.type_name);
    }
    apex_free_arg_sites(&raw);
    std::sort(as.type_names.begin(), as.type_names.end());
    as.type_idx = 0;
    for (size_t i = 0; i < as.type_names.size(); i++) {
        if (as.type_names[i] == keep_type)
            as.type_idx = (int)i;
    }
}

static void args_build_values(const ApexProject *project, ArgsWindowState &as) {
    as.values.clear();
    as.values_for = as.type_idx;
    if (as.type_idx < 0 || as.type_idx >= (int)as.type_names.size())
        return;
    const std::string &tn = as.type_names[(size_t)as.type_idx];
    std::map<uint32_t, size_t> idx;
    for (size_t i = 0; i < as.sites.size(); i++) {
        if (as.sites[i].type_name != tn)
            continue;
        auto it = idx.find(as.sites[i].value);
        if (it == idx.end()) {
            ArgValueRow v;
            v.value = as.sites[i].value;
            const char *en =
                config_type_enum_name(&project->config_types, tn.c_str(), as.sites[i].value);
            v.name = en ? en : "";
            idx[v.value] = as.values.size();
            as.values.push_back(v);
            it = idx.find(as.sites[i].value);
        }
        as.values[it->second].sites.push_back(i);
    }
    std::sort(as.values.begin(), as.values.end(),
              [](const ArgValueRow &a, const ArgValueRow &b) { return a.value < b.value; });
    if (as.selected >= (int)as.values.size())
        as.selected = -1;
}

/* The [docs] entry at (bank, addr) as configured (its has_bank form), if any. */
static const ConfigDoc *args_find_doc(const ApexProject *p, uint8_t bank, uint32_t addr) {
    for (size_t i = 0; i < p->docs.count; i++) {
        const ConfigDoc *d = &p->docs.items[i];
        if (d->addr == addr &&
            ((d->has_bank && d->bank == bank) || (!d->has_bank && bank == 0xffu)))
            return d;
    }
    return nullptr;
}

/* "@args" declaration form for the routine under the disassembly cursor. */
static void args_declare_form(ApexProject *project, const ApexRenderedDocument **document_ptr,
                              UiState *state, ArgsWindowState &as) {
    const ApexRenderedDocument *doc = *document_ptr;
    ImGui::SeparatorText("Declare register argument");
    size_t li;
    if (state->selected_line >= doc->line_count || !doc->lines[state->selected_line].has_location ||
        !find_routine_start(doc, doc->lines[state->selected_line].bank,
                            doc->lines[state->selected_line].cpu_addr, &li)) {
        ImGui::TextDisabled("Place the disassembly cursor in a routine to declare its @args.");
        return;
    }
    const ApexRenderedLine &rl = doc->lines[li];
    uint8_t bank = rl.bank;
    uint32_t addr = rl.cpu_addr;
    std::string rname = args_routine_name(doc, bank, addr);
    const ConfigDoc *cd = args_find_doc(project, bank, addr);
    std::string text = cd && cd->text ? cd->text : "";
    char buf[256];
    const char *types[6];
    int n = apex_parse_args_doc(text.c_str(), buf, sizeof(buf), types);
    ImGui::Text("Routine %s (B%02x_A%04x)", rname.c_str(), bank, (unsigned)addr);
    if (n > 0) {
        std::string cur;
        for (int r = 0; r < 6; r++) {
            if (types[r]) {
                cur += cur.empty() ? "" : ", ";
                cur += apex_arg_reg_name(r);
                cur += ":";
                cur += types[r];
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("@args %s", cur.c_str());
    }
    if (project->config_types.count == 0) {
        ImGui::TextDisabled("Define a type first (Windows -> Types).");
        return;
    }
    const char *regs[] = {"A", "B", "D", "X", "Y", "U"};
    ImGui::SetNextItemWidth(ui_w(60.0f));
    ImGui::Combo("##argreg", &as.decl_reg, regs, 6);
    ImGui::SameLine();
    if (as.decl_type >= (int)project->config_types.count)
        as.decl_type = 0;
    ImGui::SetNextItemWidth(ui_w(240.0f));
    if (ImGui::BeginCombo("##argtype", project->config_types.items[as.decl_type].name)) {
        for (size_t i = 0; i < project->config_types.count; i++) {
            if (ImGui::Selectable(project->config_types.items[i].name, (int)i == as.decl_type))
                as.decl_type = (int)i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add @args")) {
        /* Rewrite the declaration with the new register (replacing its old type). */
        std::string item =
            std::string(regs[as.decl_reg]) + ":" + project->config_types.items[as.decl_type].name;
        types[as.decl_reg] = nullptr;
        std::string decl = "@args " + item;
        for (int r = 0; r < 6; r++) {
            if (types[r])
                decl += std::string(", ") + apex_arg_reg_name(r) + ":" + types[r];
        }
        size_t at = text.find("@args");
        if (at != std::string::npos) {
            size_t end = text.find('\n', at);
            text.replace(at, end == std::string::npos ? std::string::npos : end - at, decl);
        } else {
            text += text.empty() ? decl : " " + decl;
        }
        int has_bank = cd ? cd->has_bank : 1;
        if (apex_project_set_doc(project, has_bank, bank, addr, text.c_str()) == 0) {
            state->overlay_dirty = true;
            rerender_and_reselect(project, document_ptr, state, bank, addr);
            set_status(state, "@args declared");
        } else {
            set_status(state, apex_project_last_error(project));
        }
    }
}

void render_args_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state) {
    const ApexRenderedDocument *document = *document_ptr;
    ArgsWindowState &as = state->args_state;
    if (!document)
        return;
    if (!as.computed || as.doc_ptr != document)
        args_compute(project, document, state, as);
    if (as.values_for != as.type_idx)
        args_build_values(project, as);

    ImGui::TextDisabled("Typed values passed to routines: registers declared with '@args A:TYPE'"
                        " in a routine doc, and named-type [inline] fields.");
    if (as.type_names.empty()) {
        ImGui::TextWrapped("No typed argument sites yet.");
        args_declare_form(project, document_ptr, state, as);
        return;
    }
    ImGui::SetNextItemWidth(ui_w(260.0f));
    if (ImGui::BeginCombo("Type", as.type_names[(size_t)as.type_idx].c_str())) {
        for (size_t i = 0; i < as.type_names.size(); i++) {
            if (ImGui::Selectable(as.type_names[i].c_str(), (int)i == as.type_idx)) {
                as.type_idx = (int)i;
                as.selected = -1;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    size_t named = 0;
    for (const ArgValueRow &v : as.values)
        named += v.name.empty() ? 0u : 1u;
    ImGui::Text("%zu value(s), %zu named", as.values.size(), named);
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        args_compute(project, document, state, as);

    float detail_h = as.selected >= 0 ? 200.0f : 110.0f;
    if (ImGui::BeginTable("argvals", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, -detail_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, ui_w(70.0f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Sites", ImGuiTableColumnFlags_WidthFixed, ui_w(44.0f));
        ImGui::TableSetupColumn("Passed to", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)as.values.size());
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const ArgValueRow &v = as.values[(size_t)row];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char vb[16];
                snprintf(vb, sizeof(vb), v.value > 0xffu ? "0x%04x" : "0x%02x", (unsigned)v.value);
                if (ImGui::Selectable(vb, as.selected == row,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    as.selected = row;
                    snprintf(as.name_buf, sizeof(as.name_buf), "%s", v.name.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                if (v.name.empty())
                    ImGui::TextDisabled("-");
                else
                    ImGui::TextUnformatted(v.name.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", v.sites.size());
                ImGui::TableSetColumnIndex(3);
                std::vector<std::string> callees;
                for (size_t si : v.sites) {
                    if (std::find(callees.begin(), callees.end(), as.sites[si].callee) ==
                        callees.end())
                        callees.push_back(as.sites[si].callee);
                }
                std::string joined;
                for (const std::string &c : callees)
                    joined += (joined.empty() ? "" : ", ") + c;
                ImGui::TextUnformatted(joined.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (as.selected >= 0 && as.selected < (int)as.values.size()) {
        const ArgValueRow &v = as.values[(size_t)as.selected];
        const std::string &tn = as.type_names[(size_t)as.type_idx];
        ImGui::SetNextItemWidth(ui_w(240.0f));
        bool enter =
            ImGui::InputTextWithHint("##argname", "enum name, e.g. LEFT_RAMP", as.name_buf,
                                     sizeof(as.name_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Name value") || enter) && as.name_buf[0]) {
            /* Rebuild the type's value list with this value (re)named. */
            const ConfigType *ct = find_config_type(&project->config_types, tn.c_str());
            if (ct) {
                const char *vfmt = ct->kind == TABLE_WORD ? "0x%04x:" : "0x%02x:";
                std::string vs;
                char tmp[16];
                for (size_t i = 0; i < ct->value_count; i++) {
                    if (ct->values[i].value == v.value)
                        continue;
                    snprintf(tmp, sizeof(tmp), vfmt, (unsigned)ct->values[i].value);
                    vs += (vs.empty() ? "" : ", ") + std::string(tmp) + ct->values[i].name;
                }
                snprintf(tmp, sizeof(tmp), vfmt, (unsigned)v.value);
                vs += (vs.empty() ? "" : ", ") + std::string(tmp) + as.name_buf;
                if (apex_project_set_type(project, tn.c_str(), ct->kind == TABLE_WORD,
                                          vs.c_str()) == 0) {
                    state->overlay_dirty = true;
                    as.values_for = -1; /* names changed */
                    set_status(state, "value named");
                } else {
                    set_status(state, apex_project_last_error(project));
                }
            }
        }
        if (ImGui::BeginTable("argsites", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV,
                              ImVec2(0, 120.0f))) {
            ImGui::TableSetupColumn("Site", ImGuiTableColumnFlags_WidthFixed, ui_w(100.0f));
            ImGui::TableSetupColumn("How", ImGuiTableColumnFlags_WidthFixed, ui_w(50.0f));
            ImGui::TableSetupColumn("Passed to", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("In routine", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (size_t k = 0; k < v.sites.size(); k++) {
                const ArgSiteRow &r = as.sites[v.sites[k]];
                ImGui::PushID((int)k);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char b[24];
                snprintf(b, sizeof(b), "B%02x_A%04x", r.bank, (unsigned)r.value_addr);
                if (ImGui::Selectable(b, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    size_t li;
                    if (apex_render_find_line_by_address(document, r.bank, r.value_addr, &li) ||
                        apex_render_find_line_by_address(document, r.bank, r.call_addr, &li))
                        select_line(state, li, 1);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(apex_arg_reg_name(r.reg));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(r.callee.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(r.caller.empty() ? "-" : r.caller.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    args_declare_form(project, document_ptr, state, as);
}
