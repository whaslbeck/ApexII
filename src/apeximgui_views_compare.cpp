#include "apeximgui_core.h"
#include "apeximgui_views_internal.h"
#include "ImGuiFileDialog.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cfloat>

static void apply_one_match(ApexProject *dst, const ApexProject *src,
                             const MatchWindowState::Result &r)
{
    int has_bank = (r.dst_bank != 0xffu) ? 1 : 0;
    apex_project_begin_edit_group(dst, "apply match");
    apex_project_set_label(dst, has_bank, r.dst_bank, r.dst_addr, r.label_name.c_str());
    apex_project_set_kind(dst, has_bank, r.dst_bank, r.dst_addr, APEX_KIND_CODE, "code");

    const InlineSignature *sig = inline_signature_for(&src->inline_sigs, r.src_bank, r.src_addr);
    if (sig) {
        std::string spec = inline_sig_spec_string(sig);
        if (!spec.empty())
            apex_project_set_inline(dst, has_bank, r.dst_bank, r.dst_addr, spec.c_str());
    }
    const char *doc = config_doc_at(&src->docs, r.src_bank, r.src_addr);
    if (doc && doc[0])
        apex_project_set_doc(dst, has_bank, r.dst_bank, r.dst_addr, doc);
    apex_project_end_edit_group(dst);
}

/* Accept all results within [min_conf, max_conf], return count applied. */
static int accept_tier(ApexProject *dst, MatchWindowState &ms, int min_conf, int max_conf)
{
    int n = 0;
    apex_project_begin_edit_group(dst, "apply matches");
    for (auto &r : ms.results) {
        if (r.accepted) continue;
        if (r.confidence < min_conf || r.confidence > max_conf) continue;
        apply_one_match(dst, ms.src_project, r);
        r.accepted = true;
        n++;
    }
    apex_project_end_edit_group(dst);
    return n;
}

void render_match_window(ApexProject *project,
                         const ApexRenderedDocument **document_ptr,
                         UiState *state)
{
    const ApexRenderedDocument *d = *document_ptr;
    MatchWindowState &ms = state->match_state;

    /* --- Path inputs with Browse buttons --- */
    ImGui::SetNextItemWidth(-100.0f);
    ImGui::InputText("Ref ROM", ms.ref_rom_path, sizeof(ms.ref_rom_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##brom")) {
        IGFD::FileDialogConfig cfg;
        cfg.path = ms.ref_rom_path[0] ? ms.ref_rom_path : ".";
        ImGuiFileDialog::Instance()->OpenDialog("MatchRefRom", "Select Reference ROM",
                                                ".rom,.bin", cfg);
    }
    if (ImGuiFileDialog::Instance()->Display("MatchRefRom",
            ImGuiWindowFlags_NoCollapse, ImVec2(600, 400))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
            strncpy(ms.ref_rom_path, p.c_str(), sizeof(ms.ref_rom_path) - 1);
        }
        ImGuiFileDialog::Instance()->Close();
    }

    ImGui::SetNextItemWidth(-100.0f);
    ImGui::InputText("Ref INI", ms.ref_ini_path, sizeof(ms.ref_ini_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##bini")) {
        IGFD::FileDialogConfig cfg;
        cfg.path = ms.ref_ini_path[0] ? ms.ref_ini_path : ".";
        ImGuiFileDialog::Instance()->OpenDialog("MatchRefIni", "Select Reference INI",
                                                ".ini", cfg);
    }
    if (ImGuiFileDialog::Instance()->Display("MatchRefIni",
            ImGuiWindowFlags_NoCollapse, ImVec2(600, 400))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
            strncpy(ms.ref_ini_path, p.c_str(), sizeof(ms.ref_ini_path) - 1);
        }
        ImGuiFileDialog::Instance()->Close();
    }

    /* --- Options --- */
    ImGui::Checkbox("System scan (--scan)", &ms.scan_enabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(50.0f);
    ImGui::InputInt("Min conf%", &ms.min_confidence);
    ms.min_confidence = std::max(0, std::min(100, ms.min_confidence));
    ImGui::SameLine();

    bool can_run = ms.ref_rom_path[0] != '\0' && ms.ref_ini_path[0] != '\0'
                   && project->rom_path != NULL;
    if (!can_run) ImGui::BeginDisabled();
    bool run_clicked = ImGui::Button("Run Match");
    if (!can_run) ImGui::EndDisabled();

    if (!ms.run_status.empty())
        ImGui::TextDisabled("%s", ms.run_status.c_str());

    /* --- Execute match --- */
    if (run_clicked && can_run) {
        if (ms.src_project) {
            apex_project_free(ms.src_project);
            ms.src_project = nullptr;
        }
        ms.results.clear();
        ms.has_results = false;
        ms.run_status  = "Running...";

        ApexProject *src = apex_project_open(ms.ref_rom_path, ms.ref_ini_path);
        if (!src) {
            ms.run_status = "Error: cannot open reference ROM/INI";
            return;
        }
        if (apex_project_analyze(src)) {
            apex_project_free(src);
            ms.run_status = "Error: reference analysis failed";
            return;
        }

        ApexProject *dst_tmp = apex_project_open(project->rom_path, NULL);
        if (!dst_tmp) {
            apex_project_free(src);
            ms.run_status = "Error: cannot re-open target ROM";
            return;
        }
        apex_match_inject_entries(dst_tmp, src, 1 /* system_only */);
        if (apex_project_analyze(dst_tmp)) {
            apex_project_free(dst_tmp);
            apex_project_free(src);
            ms.run_status = "Error: target analysis failed";
            return;
        }

        ApexFingerprintDB *src_db = apex_fingerprint_build(src);
        ApexFingerprintDB *dst_db = apex_fingerprint_build(dst_tmp);
        if (!src_db || !dst_db) {
            apex_fingerprint_free(src_db);
            apex_fingerprint_free(dst_db);
            apex_project_free(dst_tmp);
            apex_project_free(src);
            ms.run_status = "Error: out of memory";
            return;
        }

        size_t result_count = 0;
        ApexMatchResult *raw = apex_match_roms(src_db, dst_db,
                                               ms.min_confidence, 5, &result_count);

        if (ms.scan_enabled) {
            size_t scan_count = 0;
            ApexMatchResult *scan_raw = apex_match_scan_system_bank(
                src_db, dst_db, dst_tmp, raw, result_count, 5, &scan_count);
            if (scan_count > 0) {
                ApexMatchResult *combined = (ApexMatchResult *)realloc(
                    raw, (result_count + scan_count) * sizeof(*raw));
                if (combined) {
                    raw = combined;
                    memcpy(raw + result_count, scan_raw, scan_count * sizeof(*scan_raw));
                    result_count += scan_count;
                }
                apex_match_results_free(scan_raw, scan_count);
            }
        }

        int n_exact = 0, n_high = 0, n_med = 0;
        ms.results.reserve(result_count);
        for (size_t i = 0; i < result_count; i++) {
            const ApexMatchResult &rr = raw[i];
            MatchWindowState::Result r;
            r.label_name = rr.label_name ? rr.label_name : "";
            r.src_addr   = rr.src_addr;
            r.src_bank   = rr.src_bank;
            r.dst_addr   = rr.dst_addr;
            r.dst_bank   = rr.dst_bank;
            r.confidence = rr.confidence;
            r.accepted   = false;
            if      (r.confidence >= APEX_MATCH_CONF_EXACT) n_exact++;
            else if (r.confidence >= APEX_MATCH_CONF_HIGH)  n_high++;
            else                                             n_med++;
            ms.results.push_back(std::move(r));
        }

        apex_match_results_free(raw, result_count);
        apex_fingerprint_free(src_db);
        apex_fingerprint_free(dst_db);
        apex_project_free(dst_tmp);
        ms.src_project = src;
        ms.has_results = true;

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%zu matches: %d exact  %d high  %d medium",
                 result_count, n_exact, n_high, n_med);
        ms.run_status = buf;
    }

    if (!ms.has_results) return;

    ImGui::Separator();

    /* Pending counts per tier */
    int p_exact = 0, p_high = 0, p_med = 0;
    for (const auto &r : ms.results) {
        if (r.accepted) continue;
        if      (r.confidence >= APEX_MATCH_CONF_EXACT) p_exact++;
        else if (r.confidence >= APEX_MATCH_CONF_HIGH)  p_high++;
        else                                             p_med++;
    }
    int p_total = p_exact + p_high + p_med;

    /* --- Accept All buttons --- */
    {
        char btn[64];

        snprintf(btn, sizeof(btn), "Accept All Exact (%d)", p_exact);
        if (p_exact == 0) ImGui::BeginDisabled();
        if (ImGui::Button(btn)) {
            accept_tier(project, ms, APEX_MATCH_CONF_EXACT, 100);
            state->overlay_dirty = true;
            uint8_t cb = 0xffu; uint32_t ca = 0u;
            selected_address(d, state, &cb, &ca);
            rerender_and_reselect(project, document_ptr, state, cb, ca);
            d = *document_ptr;
        }
        if (p_exact == 0) ImGui::EndDisabled();

        ImGui::SameLine();
        snprintf(btn, sizeof(btn), "Accept All High (%d)", p_high);
        if (p_high == 0) ImGui::BeginDisabled();
        if (ImGui::Button(btn)) {
            accept_tier(project, ms, APEX_MATCH_CONF_HIGH, APEX_MATCH_CONF_EXACT - 1);
            state->overlay_dirty = true;
            uint8_t cb = 0xffu; uint32_t ca = 0u;
            selected_address(d, state, &cb, &ca);
            rerender_and_reselect(project, document_ptr, state, cb, ca);
            d = *document_ptr;
        }
        if (p_high == 0) ImGui::EndDisabled();

        ImGui::SameLine();
        snprintf(btn, sizeof(btn), "Accept All Med (%d)", p_med);
        if (p_med == 0) ImGui::BeginDisabled();
        if (ImGui::Button(btn)) {
            accept_tier(project, ms, ms.min_confidence, APEX_MATCH_CONF_HIGH - 1);
            state->overlay_dirty = true;
            uint8_t cb = 0xffu; uint32_t ca = 0u;
            selected_address(d, state, &cb, &ca);
            rerender_and_reselect(project, document_ptr, state, cb, ca);
            d = *document_ptr;
        }
        if (p_med == 0) ImGui::EndDisabled();

        ImGui::SameLine();
        snprintf(btn, sizeof(btn), "Accept All (%d)", p_total);
        if (p_total == 0) ImGui::BeginDisabled();
        if (ImGui::Button(btn)) {
            accept_tier(project, ms, ms.min_confidence, 100);
            state->overlay_dirty = true;
            uint8_t cb = 0xffu; uint32_t ca = 0u;
            selected_address(d, state, &cb, &ca);
            rerender_and_reselect(project, document_ptr, state, cb, ca);
            d = *document_ptr;
        }
        if (p_total == 0) ImGui::EndDisabled();
    }

    /* --- Filter bar --- */
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputText("##matchfilter", ms.filter, sizeof(ms.filter));
    ImGui::SameLine();
    ImGui::RadioButton("All",      &ms.show_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Pending",  &ms.show_mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Accepted", &ms.show_mode, 2);

    /* --- Results table --- */
    static const ImGuiTableFlags kMatchTblFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("match_results", 4, kMatchTblFlags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed,   16.0f);
        ImGui::TableSetupColumn("Conf",   ImGuiTableColumnFlags_WidthFixed,   52.0f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed,  105.0f);
        ImGui::TableSetupColumn("Label",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        int row_id = 0;
        for (auto &r : ms.results) {
            if (ms.show_mode == 1 && r.accepted)  continue;
            if (ms.show_mode == 2 && !r.accepted) continue;
            if (ms.filter[0] && !str_icontains(r.label_name.c_str(), ms.filter)) continue;

            ImGui::PushID(row_id++);
            ImGui::TableNextRow();

            /* Col 0: accepted checkmark */
            ImGui::TableSetColumnIndex(0);
            if (r.accepted)
                ImGui::TextColored(ImVec4(0.47f, 0.86f, 0.47f, 1.0f), "\xe2\x9c\x93");

            /* Col 1: confidence tier */
            ImGui::TableSetColumnIndex(1);
            if (r.confidence >= APEX_MATCH_CONF_EXACT)
                ImGui::TextColored(ImVec4(0.47f, 0.86f, 0.47f, 1.0f), "Exact");
            else if (r.confidence >= APEX_MATCH_CONF_HIGH)
                ImGui::TextColored(ImVec4(0.47f, 0.70f, 0.95f, 1.0f), "High");
            else
                ImGui::TextColored(ImVec4(0.95f, 0.82f, 0.45f, 1.0f), "Med");

            /* Col 2: destination address — click to navigate */
            ImGui::TableSetColumnIndex(2);
            char addrstr[32];
            if (r.dst_bank == 0xffu)
                snprintf(addrstr, sizeof(addrstr), "0x%04x", r.dst_addr);
            else
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x", r.dst_bank, r.dst_addr);
            size_t target_li = 0;
            bool found = apex_render_find_line_by_address(d, r.dst_bank, r.dst_addr,
                                                          &target_li) != NULL;
            if (ImGui::SmallButton(addrstr) && found)
                select_line(state, target_li, 1);

            /* Col 3: label name + per-row Accept button */
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(r.label_name.c_str());
            if (!r.accepted) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Accept")) {
                    apply_one_match(project, ms.src_project, r);
                    r.accepted = true;
                    state->overlay_dirty = true;
                    rerender_and_reselect(project, document_ptr, state,
                                         r.dst_bank, r.dst_addr);
                    d = *document_ptr;
                }
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ============================================================
// ROM Compare window
// ============================================================

static ImVec4 cmp_status_color(ApexCompareStatus s)
{
    switch (s) {
    case APEX_CMP_IDENTICAL: return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    case APEX_CMP_MOVED:     return ImVec4(0.40f, 0.80f, 0.90f, 1.0f);
    case APEX_CMP_CHANGED:   return ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
    case APEX_CMP_REMOVED:   return ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
    case APEX_CMP_ADDED:     return ImVec4(0.50f, 0.85f, 0.50f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

static bool cmp_status_enabled(const CompareWindowState &cs, ApexCompareStatus s)
{
    switch (s) {
    case APEX_CMP_IDENTICAL: return cs.show_identical;
    case APEX_CMP_MOVED:     return cs.show_moved;
    case APEX_CMP_CHANGED:   return cs.show_changed;
    case APEX_CMP_REMOVED:   return cs.show_removed;
    case APEX_CMP_ADDED:     return cs.show_added;
    }
    return true;
}

static void compare_run(ApexProject *project, CompareWindowState &cs)
{
    cs.reset();
    cs.run_status = "Running...";

    if (apex_project_analyze(project) != 0) {
        cs.run_status = "Error: analysis of current ROM failed";
        return;
    }
    ApexProject *bp = apex_project_open(cs.rom_b_path,
                                        cs.ini_b_path[0] ? cs.ini_b_path : NULL);
    if (!bp) {
        cs.run_status = "Error: cannot open ROM B";
        return;
    }
    apex_match_inject_entries(bp, project, cs.inject_paged ? 0 : 1);
    if (apex_project_analyze(bp) != 0) {
        apex_project_free(bp);
        cs.run_status = "Error: analysis of ROM B failed";
        return;
    }

    ApexCompareOptions opt;
    apex_compare_default_options(&opt);
    opt.include_code      = cs.inc_code;
    opt.include_strings   = cs.inc_strings;
    opt.include_tables    = cs.inc_tables;
    opt.include_identical = cs.show_identical;
    opt.min_instrs        = cs.min_instrs > 0 ? cs.min_instrs : 5;

    ApexCompareReport rep;
    if (apex_compare_run(project, bp, &opt, &rep) != 0) {
        apex_project_free(bp);
        cs.run_status = "Error: comparison failed";
        return;
    }

    cs.results.assign(rep.items, rep.items + rep.count);
    cs.n_identical = rep.n_identical;
    cs.n_moved     = rep.n_moved;
    cs.n_changed   = rep.n_changed;
    cs.n_removed   = rep.n_removed;
    cs.n_added     = rep.n_added;
    apex_compare_report_free(&rep);

    cs.b_project = bp;
    cs.a_db = apex_fingerprint_build(project);
    cs.b_db = apex_fingerprint_build(bp);
    cs.has_results = true;
    char buf[96];
    snprintf(buf, sizeof(buf), "%zu changed, %zu moved, %zu removed, %zu added",
             cs.n_changed, cs.n_moved, cs.n_removed, cs.n_added);
    cs.run_status = buf;
}

static void compare_browse(const char *id, const char *title, const char *filters,
                           char *target, size_t target_sz)
{
    ImGui::SameLine();
    if (ImGui::Button(id)) {
        IGFD::FileDialogConfig cfg;
        cfg.path = target[0] ? target : ".";
        ImGuiFileDialog::Instance()->OpenDialog(id, title, filters, cfg);
    }
    if (ImGuiFileDialog::Instance()->Display(id, ImGuiWindowFlags_NoCollapse,
                                             ImVec2(600, 400))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
            strncpy(target, p.c_str(), target_sz - 1);
            target[target_sz - 1] = '\0';
        }
        ImGuiFileDialog::Instance()->Close();
    }
}

void render_rom_compare_window(ApexProject *project,
                               const ApexRenderedDocument **document_ptr,
                               UiState *state)
{
    const ApexRenderedDocument *d = *document_ptr;
    CompareWindowState &cs = state->compare_state;

    ImGui::TextDisabled("Compare the current ROM (A) against another version (B).");

    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputText("ROM B", cs.rom_b_path, sizeof(cs.rom_b_path));
    compare_browse("CmpRomB", "Select ROM B", ".rom,.bin", cs.rom_b_path,
                   sizeof(cs.rom_b_path));

    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputText("INI B (opt)", cs.ini_b_path, sizeof(cs.ini_b_path));
    compare_browse("CmpIniB", "Select ROM B config", ".ini", cs.ini_b_path,
                   sizeof(cs.ini_b_path));

    ImGui::Checkbox("Code", &cs.inc_code);
    ImGui::SameLine();
    ImGui::Checkbox("Strings", &cs.inc_strings);
    ImGui::SameLine();
    ImGui::Checkbox("Tables", &cs.inc_tables);
    ImGui::SameLine();
    ImGui::Checkbox("Inject paged", &cs.inject_paged);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::InputInt("Min instrs", &cs.min_instrs, 0, 0);
    if (cs.min_instrs < 1) cs.min_instrs = 1;

    bool can_run = cs.rom_b_path[0] != '\0' && project->rom_path != NULL;
    if (!can_run) ImGui::BeginDisabled();
    if (ImGui::Button("Run Compare")) {
        compare_run(project, cs);
    }
    if (!can_run) ImGui::EndDisabled();
    if (!cs.run_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", cs.run_status.c_str());
    }

    /* ---- candidates for the currently-selected routine ---- */
    if (cs.has_results && cs.a_db && cs.b_db) {
        uint8_t sb = 0xffu;
        uint32_t sa = 0;
        if (selected_address(d, state, &sb, &sa)) {
            ApexCompareCandidate cand[3];
            size_t nc = apex_compare_candidates(cs.a_db, cs.b_db, sb, sa, cand, 3);
            ImGui::SeparatorText("Candidates in B for selected routine");
            ImGui::Text("A: B%02x_A%04x", sb, (unsigned)sa & 0xffffu);
            if (nc == 0) {
                ImGui::TextDisabled("  (no fingerprint / no candidates)");
            }
            for (size_t i = 0; i < nc; i++) {
                ImGui::BulletText("B%02x_A%04x   %d%%   %s",
                                  cand[i].bank, (unsigned)cand[i].addr & 0xffffu,
                                  cand[i].confidence, cand[i].exact ? "(exact)" : "");
            }
        }
    }

    if (!cs.has_results) {
        return;
    }

    ImGui::SeparatorText("Differences");
    ImGui::Text("%zu identical | %zu moved | %zu changed | %zu removed | %zu added",
                cs.n_identical, cs.n_moved, cs.n_changed, cs.n_removed, cs.n_added);
    ImGui::Checkbox("identical", &cs.show_identical);
    ImGui::SameLine(); ImGui::Checkbox("moved", &cs.show_moved);
    ImGui::SameLine(); ImGui::Checkbox("changed", &cs.show_changed);
    ImGui::SameLine(); ImGui::Checkbox("removed", &cs.show_removed);
    ImGui::SameLine(); ImGui::Checkbox("added", &cs.show_added);
    ImGui::TextDisabled("(identical entries are listed only when 'identical' was "
                        "ticked before Run)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##cmpfilter", "filter label / detail...", cs.filter,
                             sizeof(cs.filter));

    /* Build the filtered index list. */
    static std::vector<size_t> visible;
    visible.clear();
    for (size_t i = 0; i < cs.results.size(); i++) {
        const ApexCompareEntry &e = cs.results[i];
        if (!cmp_status_enabled(cs, e.status)) continue;
        if (cs.filter[0]) {
            char hay[160];
            snprintf(hay, sizeof(hay), "%s %s", e.label, e.detail);
            if (!str_icontains(hay, cs.filter)) continue;
        }
        visible.push_back(i);
    }

    if (ImGui::BeginTable("##cmptbl", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 72.0f, 0);
        ImGui::TableSetupColumn("Kind",   ImGuiTableColumnFlags_WidthFixed, 50.0f, 1);
        ImGui::TableSetupColumn("A",      ImGuiTableColumnFlags_WidthFixed, 86.0f, 2);
        ImGui::TableSetupColumn("B",      ImGuiTableColumnFlags_WidthFixed, 86.0f, 3);
        ImGui::TableSetupColumn("Label",  ImGuiTableColumnFlags_WidthFixed, 200.0f, 4);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 0.0f, 5);
        ImGui::TableHeadersRow();

        int sort_col; bool sort_asc;
        if (ui_table_sort(&sort_col, &sort_asc)) {
            const std::vector<ApexCompareEntry> &res = cs.results;
            std::stable_sort(visible.begin(), visible.end(),
                [&](size_t ia, size_t ib) {
                    const ApexCompareEntry &a = res[ia];
                    const ApexCompareEntry &b = res[ib];
                    int c = 0;
                    switch (sort_col) {
                    case 0: c = ui_cmp_int(a.status, b.status); break;
                    case 1: c = ui_cmp_int(a.kind, b.kind); break;
                    case 2: c = a.has_a != b.has_a ? (a.has_a ? -1 : 1)
                                : ui_cmp_u32((a.a_bank<<16)|a.a_addr, (b.a_bank<<16)|b.a_addr); break;
                    case 3: c = a.has_b != b.has_b ? (a.has_b ? -1 : 1)
                                : ui_cmp_u32((a.b_bank<<16)|a.b_addr, (b.b_bank<<16)|b.b_addr); break;
                    case 4: c = strcmp(a.label, b.label); break;
                    case 5: c = strcmp(a.detail, b.detail); break;
                    }
                    return sort_asc ? c < 0 : c > 0;
                });
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)visible.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const ApexCompareEntry &e = cs.results[visible[(size_t)row]];
                char abuf[16] = "--", bbuf[16] = "--";
                if (e.has_a)
                    snprintf(abuf, sizeof(abuf), "B%02x_A%04x", e.a_bank,
                             (unsigned)e.a_addr & 0xffffu);
                if (e.has_b)
                    snprintf(bbuf, sizeof(bbuf), "B%02x_A%04x", e.b_bank,
                             (unsigned)e.b_addr & 0xffffu);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                ImGui::TextColored(cmp_status_color(e.status), "%s",
                                   apex_compare_status_name(e.status));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(apex_compare_kind_name(e.kind));
                ImGui::TableNextColumn();
                if (e.has_a) {
                    if (ImGui::Selectable(abuf, false,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                        size_t li;
                        if (d && apex_render_find_line_by_address(d, e.a_bank, e.a_addr,
                                                                  &li)) {
                            select_line(state, li, 1);
                        }
                    }
                } else {
                    ImGui::TextUnformatted(abuf);
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(bbuf);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.label);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.detail);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

// ============================================================
// Coverage / worklist window
// ============================================================

static void coverage_compute(ApexProject *project, const ApexRenderedDocument *doc,
                             CoverageWindowState &cv)
{
    size_t rom_size = project->rom.size;
    size_t i, fill;
    std::vector<uint8_t> kinds(rom_size, (uint8_t)APEX_RENDER_BLOCK_UNKNOWN);

    /* Forward pass: assign each ROM byte its block kind (mirrors apexini coverage). */
    {
        uint8_t cur = (uint8_t)APEX_RENDER_BLOCK_UNKNOWN;
        fill = 0;
        for (i = 0; i < doc->line_count && fill < rom_size; i++) {
            const ApexRenderedLine *l = &doc->lines[i];
            if (!l->has_location) continue;
            if (l->rom_addr <= fill) {
                cur = (uint8_t)l->block_kind;
            } else {
                size_t end = l->rom_addr < rom_size ? l->rom_addr : rom_size;
                while (fill < end) kinds[fill++] = cur;
                cur = (uint8_t)l->block_kind;
            }
        }
        while (fill < rom_size) kinds[fill++] = cur;
    }

    for (i = 0; i < 7; i++) cv.totals[i] = 0;
    for (i = 0; i < rom_size; i++) {
        uint8_t k = kinds[i];
        if (k < 7) cv.totals[k]++;
    }

    /* Build the worklist: runs of UNCLASSIFIED (and optionally UNKNOWN). */
    cv.gaps.clear();
    i = 0;
    while (i < rom_size) {
        uint8_t k = kinds[i];
        int is_uncl = (k == APEX_RENDER_BLOCK_UNCLASSIFIED);
        int is_unk  = (k == APEX_RENDER_BLOCK_UNKNOWN);
        if (is_uncl || (is_unk && cv.include_unknown)) {
            size_t start = i;
            while (i < rom_size && kinds[i] == k) i++;
            size_t len = i - start;
            if (len >= (size_t)(cv.min_gap > 0 ? cv.min_gap : 1)) {
                CoverageWindowState::Gap g;
                g.off = start;
                g.len = len;
                g.unknown = is_unk;
                if (!rom_offset_to_cpu_address(project, start, &g.bank, &g.addr)) {
                    g.bank = 0xffu;
                    g.addr = 0;
                }
                cv.gaps.push_back(g);
            }
        } else {
            i++;
        }
    }

    cv.rom_size = rom_size;
    cv.doc_ptr = doc;
    cv.computed = true;
    cv.next_gap = 0;
}

static void coverage_bar(const char *label, size_t count, size_t total, ImVec4 col)
{
    float frac = total ? (float)count / (float)total : 0.0f;
    char overlay[48];
    snprintf(overlay, sizeof(overlay), "%s  %zu (%.1f%%)", label, count, frac * 100.0);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
    ImGui::PopStyleColor();
}

void render_coverage_window(ApexProject *project,
                            const ApexRenderedDocument **document_ptr,
                            UiState *state)
{
    const ApexRenderedDocument *d = *document_ptr;
    CoverageWindowState &cv = state->coverage_state;

    if (!d) {
        ImGui::TextDisabled("No disassembly available.");
        return;
    }

    bool refresh = ImGui::Button("Refresh");
    ImGui::SameLine();
    if (ImGui::Checkbox("include unreached (unknown)", &cv.include_unknown)) {
        refresh = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::InputInt("min gap", &cv.min_gap, 1, 8)) {
        if (cv.min_gap < 1) cv.min_gap = 1;
        refresh = true;
    }

    if (refresh || !cv.computed || cv.doc_ptr != (const void *)d ||
        cv.rom_size != project->rom.size) {
        coverage_compute(project, d, cv);
    }

    size_t total = cv.rom_size;
    ImGui::SeparatorText("Coverage");
    coverage_bar("code",         cv.totals[APEX_RENDER_BLOCK_CODE],         total, ImVec4(0.40f,0.70f,0.95f,1));
    coverage_bar("data",         cv.totals[APEX_RENDER_BLOCK_DATA],         total, ImVec4(0.55f,0.80f,0.55f,1));
    coverage_bar("table",        cv.totals[APEX_RENDER_BLOCK_TABLE],        total, ImVec4(0.75f,0.65f,0.95f,1));
    coverage_bar("sprite",       cv.totals[APEX_RENDER_BLOCK_SPRITE],       total, ImVec4(0.90f,0.65f,0.40f,1));
    coverage_bar("unclassified", cv.totals[APEX_RENDER_BLOCK_UNCLASSIFIED], total, ImVec4(0.95f,0.80f,0.30f,1));
    coverage_bar("unknown",      cv.totals[APEX_RENDER_BLOCK_UNKNOWN],      total, ImVec4(0.90f,0.45f,0.45f,1));
    coverage_bar("free (0xff)",  cv.totals[APEX_RENDER_BLOCK_FREE],         total, ImVec4(0.50f,0.50f,0.50f,1));

    {
        size_t classified = cv.totals[APEX_RENDER_BLOCK_CODE] +
                            cv.totals[APEX_RENDER_BLOCK_DATA] +
                            cv.totals[APEX_RENDER_BLOCK_TABLE] +
                            cv.totals[APEX_RENDER_BLOCK_SPRITE];
        size_t denom = total - cv.totals[APEX_RENDER_BLOCK_FREE];
        ImGui::Text("classified (excl. free): %.1f%%",
                    denom ? classified * 100.0 / denom : 0.0);
    }

    ImGui::SeparatorText("Worklist");
    ImGui::Text("%zu gap%s", cv.gaps.size(), cv.gaps.size() == 1 ? "" : "s");
    ImGui::SameLine();
    if (ImGui::Button("Jump to next gap") && !cv.gaps.empty()) {
        if (cv.next_gap >= (int)cv.gaps.size()) cv.next_gap = 0;
        const CoverageWindowState::Gap &g = cv.gaps[(size_t)cv.next_gap];
        size_t li;
        if (apex_render_find_line_by_address(d, g.bank, g.addr, &li))
            select_line(state, li, 1);
        cv.next_gap++;
    }

    if (ImGui::BeginTable("##covtbl", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100.0f, 0);
        ImGui::TableSetupColumn("Bytes",   ImGuiTableColumnFlags_WidthFixed, 70.0f, 1);
        ImGui::TableSetupColumn("Kind",    ImGuiTableColumnFlags_WidthFixed, 90.0f, 2);
        ImGui::TableSetupColumn("ROM off", ImGuiTableColumnFlags_WidthStretch, 0.0f, 3);
        ImGui::TableHeadersRow();

        int sort_col; bool sort_asc;
        if (ui_table_sort(&sort_col, &sort_asc)) {
            std::stable_sort(cv.gaps.begin(), cv.gaps.end(),
                [&](const CoverageWindowState::Gap &a, const CoverageWindowState::Gap &b) {
                    int c = 0;
                    switch (sort_col) {
                    case 0: c = ui_cmp_u32(((uint32_t)a.bank<<16)|a.addr,
                                           ((uint32_t)b.bank<<16)|b.addr); break;
                    case 1: c = ui_cmp_sz(a.len, b.len); break;
                    case 2: c = ui_cmp_int(a.unknown, b.unknown); break;
                    case 3: c = ui_cmp_sz(a.off, b.off); break;
                    }
                    return sort_asc ? c < 0 : c > 0;
                });
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)cv.gaps.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const CoverageWindowState::Gap &g = cv.gaps[(size_t)row];
                char addrbuf[16];
                snprintf(addrbuf, sizeof(addrbuf), "B%02x_A%04x", g.bank,
                         (unsigned)g.addr & 0xffffu);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                if (ImGui::Selectable(addrbuf, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    size_t li;
                    if (apex_render_find_line_by_address(d, g.bank, g.addr, &li))
                        select_line(state, li, 1);
                }
                ImGui::TableNextColumn();
                ImGui::Text("%zu", g.len);
                ImGui::TableNextColumn();
                ImGui::TextColored(g.unknown ? ImVec4(0.90f,0.45f,0.45f,1)
                                             : ImVec4(0.95f,0.80f,0.30f,1),
                                   "%s", g.unknown ? "unknown" : "unclassified");
                ImGui::TableNextColumn();
                ImGui::Text("0x%06zx", g.off);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}
