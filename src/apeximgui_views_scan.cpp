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
            apex_project_set_symbol(project, r.name.c_str(), r.addr);
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

