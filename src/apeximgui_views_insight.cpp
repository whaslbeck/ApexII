/* Insight windows: the lamp / flag Bit Map (apex_bits.h), the Routine Card
   (apex_card.h), the Event Timeline (recorded from PinMAME) and the adjustment
   names from the ROM's text tables (apex_adjnames.h, in the Types window). */
#include "apeximgui_core.h"
#include "apeximgui_views_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

/* ---- Bit Map ---- */

static void bits_compute(ApexProject *project, const ApexRenderedDocument *doc, UiState *state,
                         BitsWindowState &bs) {
    apex_bit_map_free(&bs.map);
    bs.path_names.clear();
    bs.computed = true;
    bs.doc_ptr = doc;
    bs.selected = -1;
    if (apex_bit_map(project, &bs.map) != 0)
        return;
    for (size_t k = 0; k < bs.map.path_count; k++) {
        std::string n = label_at_address(doc, state, bs.map.paths[k].bank, bs.map.paths[k].addr);
        if (n.empty()) {
            char b[16];
            snprintf(b, sizeof(b), "B%02x_A%04x", bs.map.paths[k].bank,
                     (unsigned)bs.map.paths[k].addr);
            n = b;
        }
        bs.path_names.push_back(n);
    }
}

/* Short name of a bit routine's operation: `@bitop NAME` in its doc, else a
   keyword of its label (on / off / set / clear / toggle / test / query),
   else "[k]". */
static std::string bit_op_name(const ApexProject *project, const ApexBitPath &pa,
                               const std::string &label, size_t k) {
    const char *doc = config_doc_at(&project->docs, pa.bank, pa.addr);
    const char *b = doc ? strstr(doc, "@bitop") : nullptr;
    if (b) {
        b += 6;
        while (*b == ' ')
            b++;
        size_t n = strcspn(b, " ;,");
        if (n)
            return std::string(b, n);
    }
    static const char *const words[] = {"toggle", "test", "query", "clear", "off", "set", "on"};
    std::string l = "_" + label + "_";
    for (const char *w : words)
        if (l.find("_" + std::string(w) + "_") != std::string::npos)
            return w;
    if (pa.is_test)
        return "test";
    char d[32];
    snprintf(d, sizeof(d), "[%lu]", (unsigned long)k);
    return d;
}

/* The routine around (bank, addr): its label, or "". */
static std::string routine_name(const ApexRenderedDocument *doc, uint8_t bank, uint32_t addr) {
    size_t li;
    if (!find_routine_start(doc, bank, addr, &li) || doc->lines[li].bank != bank)
        return "";
    std::string r(doc->lines[li].text, doc->lines[li].length);
    size_t colon = r.find(':');
    if (colon != std::string::npos)
        r.erase(colon);
    return r;
}

/* Declare `type` for matrix `code` in the first bit routine's doc
   (`@bits 0xNN:TYPE`, replacing an earlier one for that matrix; "" removes
   it).  Returns 0 on success. */
static int set_matrix_type(ApexProject *project, const ApexBitMap &m, int code, const char *type) {
    if (!m.path_count)
        return -1;
    const ApexBitPath &pa = m.paths[0];
    const char *old = config_doc_at(&project->docs, pa.bank, pa.addr);
    std::string doc = old ? old : "";
    char key[16];
    snprintf(key, sizeof(key), "@bits 0x%02x:", code);
    size_t at = doc.find(key);
    if (at != std::string::npos) {
        size_t end = doc.find_first_of(" ;,", at + strlen(key));
        doc.erase(at, end == std::string::npos ? std::string::npos : end - at);
    }
    while (!doc.empty() && doc.back() == ' ')
        doc.pop_back();
    if (type && *type)
        doc += (doc.empty() ? "" : " ") + std::string(key) + type;
    if (doc.empty())
        return apex_project_clear_doc(project, 1, pa.bank, pa.addr);
    return apex_project_set_doc(project, 1, pa.bank, pa.addr, doc.c_str());
}

/* Name bit `bit` of matrix `code`: a value of the matrix type, which is made
   (TYPE_BITS_NN) when the matrix has none. */
static int name_bit(ApexProject *project, const ApexBitMap &m, int code, uint32_t bit,
                    const char *name, const char *site_type) {
    std::string tn = site_type ? site_type : m.matrix_type[code] ? m.matrix_type[code] : "";
    if (tn.empty()) {
        char b[32];
        snprintf(b, sizeof(b), "TYPE_BITS_%02X", code);
        tn = b;
        if (!find_config_type(&project->config_types, tn.c_str()) &&
            apex_project_set_type(project, tn.c_str(), 0, "") != 0)
            return -1;
        if (set_matrix_type(project, m, code, tn.c_str()) != 0)
            return -1;
    }
    const ConfigType *ct = find_config_type(&project->config_types, tn.c_str());
    if (!ct)
        return -1;
    std::string vs;
    char tmp[16];
    for (size_t i = 0; i < ct->value_count; i++) {
        if (ct->values[i].value == bit)
            continue;
        snprintf(tmp, sizeof(tmp), "0x%02x:", (unsigned)ct->values[i].value);
        vs += (vs.empty() ? "" : ", ") + std::string(tmp) + ct->values[i].name;
    }
    snprintf(tmp, sizeof(tmp), "0x%02x:", (unsigned)bit);
    vs += (vs.empty() ? "" : ", ") + std::string(tmp) + name;
    return apex_project_set_type(project, tn.c_str(), ct->kind == TABLE_WORD, vs.c_str());
}

static void jump_to(const ApexRenderedDocument *doc, UiState *state, uint8_t bank, uint32_t addr) {
    size_t li;
    if (apex_render_find_line_by_address(doc, bank, addr, &li))
        select_line(state, li, 1);
}

void render_bits_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state) {
    const ApexRenderedDocument *doc = *document_ptr;
    BitsWindowState &bs = state->bits_state;
    if (!bs.computed || bs.doc_ptr != (const void *)doc)
        bits_compute(project, doc, state, bs);
    const ApexBitMap &m = bs.map;
    if (!m.path_count) {
        ImGui::TextWrapped("No bit routines found: the OS's lamp / flag routines are recognised "
                           "by their small wrappers (PSHS ; LDX n,S ; LDA ,X+ ; STX n,S ; LDB "
                           "#matrix ; JSR routine) -- the system bank must be analysed.");
        return;
    }
    /* the matrices (B codes) with their bit counts */
    std::vector<std::pair<int, int>> mats; /* code, bits */
    for (size_t i = 0; i < m.site_count;) {
        size_t j = i;
        int bits = 0;
        while (j < m.site_count && m.sites[j].matrix == m.sites[i].matrix) {
            size_t k = j;
            while (k < m.site_count && m.sites[k].matrix == m.sites[j].matrix &&
                   m.sites[k].bit == m.sites[j].bit)
                k++;
            bits++;
            j = k;
        }
        mats.push_back({m.sites[i].matrix, bits});
        i = j;
    }
    if (bs.matrix < 0 && !mats.empty())
        bs.matrix = mats[0].first;
    ImGui::TextUnformatted("matrix");
    for (const auto &mt : mats) {
        ImGui::SameLine();
        char lb[32];
        snprintf(lb, sizeof(lb), "0x%02x (%d)", mt.first, mt.second);
        if (ImGui::RadioButton(lb, bs.matrix == mt.first)) {
            bs.matrix = mt.first;
            bs.selected = -1;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The B code the bit routines get: on WPC usually 0x00 = lamps, "
                              "0x48 = game flags (bits)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("refresh"))
        bits_compute(project, doc, state, bs);
    if (bs.matrix >= 0) {
        ImGui::SameLine();
        const char *mt = m.matrix_type[bs.matrix];
        ImGui::SetNextItemWidth(ui_w(180));
        if (ImGui::BeginCombo("names from type", mt ? mt : "-")) {
            int sel = -2;
            std::string pick;
            if (ImGui::Selectable("-", !mt))
                sel = -1;
            for (size_t t = 0; t < project->config_types.count; t++) {
                const char *tn = project->config_types.items[t].name;
                if (ImGui::Selectable(tn, mt && !strcmp(mt, tn))) {
                    sel = (int)t;
                    pick = tn;
                }
            }
            ImGui::EndCombo();
            if (sel >= -1 && set_matrix_type(project, m, bs.matrix, pick.c_str()) == 0) {
                state->overlay_dirty = true;
                rerender_and_reselect(project, document_ptr, state, m.paths[0].bank,
                                      m.paths[0].addr);
                return; /* recomputed with the new document */
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A [types] type whose values name this matrix's bits; stored as "
                              "'@bits 0x%02x:TYPE' in the doc of the first bit routine",
                              bs.matrix);
    }
    ImGui::SetNextItemWidth(ui_w(220));
    ImGui::InputTextWithHint("##bitfilter", "filter: bit name / number / routine", bs.filter,
                             sizeof(bs.filter));
    ImGui::SameLine();
    ImGui::TextDisabled("%lu bit routine(s), %lu wrapper(s), %lu site(s)",
                        (unsigned long)m.path_count, (unsigned long)m.wrapper_count,
                        (unsigned long)m.site_count);

    int ncol = 2 + (int)m.path_count;
    float list_h = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.55f);
    if (ImGui::BeginTable("bits", ncol,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, list_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Bit", ImGuiTableColumnFlags_WidthFixed, ui_w(44.0f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        for (size_t k = 0; k < m.path_count; k++) {
            /* the operation (on / off / test ...): the label is in the tooltip */
            std::string h = bit_op_name(project, m.paths[k], bs.path_names[k], k);
            ImGui::TableSetupColumn(h.c_str(), ImGuiTableColumnFlags_WidthFixed, ui_w(56.0f));
        }
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < ncol; c++) {
            ImGui::TableSetColumnIndex(c);
            ImGui::TableHeader(ImGui::TableGetColumnName(c));
            if (c >= 2 && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n(name the operation with '@bitop NAME' in its doc)",
                                  bs.path_names[(size_t)c - 2].c_str());
        }
        for (size_t i = 0; i < m.site_count;) {
            const ApexBitSite &g = m.sites[i];
            size_t j = i;
            while (j < m.site_count && m.sites[j].matrix == g.matrix && m.sites[j].bit == g.bit)
                j++;
            if (g.matrix != bs.matrix) {
                i = j;
                continue;
            }
            const char *en = g.type_name
                                 ? config_type_enum_name(&project->config_types, g.type_name, g.bit)
                                 : nullptr;
            if (bs.filter[0]) {
                char num[16];
                snprintf(num, sizeof(num), "0x%02x", (unsigned)g.bit);
                bool hit = (en && strcasestr(en, bs.filter)) || strcasestr(num, bs.filter);
                for (size_t k = i; k < j && !hit; k++)
                    hit =
                        strcasestr(routine_name(doc, m.sites[k].bank, m.sites[k].call_addr).c_str(),
                                   bs.filter) != nullptr;
                if (!hit) {
                    i = j;
                    continue;
                }
            }
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char b[16];
            snprintf(b, sizeof(b), "0x%02x", (unsigned)g.bit);
            if (ImGui::Selectable(b, bs.selected == (int)i,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap))
                bs.selected = (int)i;
            ImGui::TableSetColumnIndex(1);
            if (en)
                ImGui::TextUnformatted(en);
            else
                ImGui::TextDisabled(g.type_name ? "-" : "(no type)");
            for (size_t k = 0; k < m.path_count; k++) {
                int n = 0;
                for (size_t q = i; q < j; q++)
                    n += m.sites[q].path == (int)k;
                ImGui::TableSetColumnIndex(2 + (int)k);
                if (n)
                    ImGui::Text("%d", n);
                else
                    ImGui::TextDisabled("-");
            }
            ImGui::PopID();
            i = j;
        }
        ImGui::EndTable();
    }
    /* the sites of the selected bit */
    if (bs.selected < 0 || (size_t)bs.selected >= m.site_count)
        return;
    const ApexBitSite &g = m.sites[bs.selected];
    ImGui::SeparatorText("sites");
    {
        static char name_buf[96];
        static int name_for = -1;
        if (name_for != bs.selected) {
            const char *en = g.type_name
                                 ? config_type_enum_name(&project->config_types, g.type_name, g.bit)
                                 : nullptr;
            snprintf(name_buf, sizeof(name_buf), "%s", en ? en : "");
            name_for = bs.selected;
        }
        ImGui::SetNextItemWidth(ui_w(260));
        ImGui::InputTextWithHint("##bitname", "name this bit, e.g. FLAG_BALL_SAVE", name_buf,
                                 sizeof(name_buf));
        ImGui::SameLine();
        ImGui::BeginDisabled(!name_buf[0]);
        if (ImGui::Button("Name")) {
            uint8_t jb = g.bank;
            uint32_t ja = g.call_addr;
            if (name_bit(project, m, g.matrix, g.bit, name_buf, g.type_name) == 0) {
                state->overlay_dirty = true;
                set_status(state, "bit named");
                name_for = -1;
                rerender_and_reselect(project, document_ptr, state, jb, ja);
                ImGui::EndDisabled();
                return;
            }
            set_status(state, apex_project_last_error(project));
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("A value of the matrix's type (TYPE_BITS_%02X is made if it has "
                              "none)",
                              g.matrix);
    }
    if (!ImGui::BeginTable("bitsites", 3,
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY,
                           ImVec2(0, -1)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Operation", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Site", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
    ImGui::TableSetupColumn("In routine", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableHeadersRow();
    for (size_t q = (size_t)bs.selected;
         q < m.site_count && m.sites[q].matrix == g.matrix && m.sites[q].bit == g.bit; q++) {
        const ApexBitSite &s = m.sites[q];
        ImGui::PushID((int)q);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char lb[160];
        snprintf(lb, sizeof(lb), "[%d] %s", s.path, bs.path_names[(size_t)s.path].c_str());
        if (ImGui::Selectable(
                lb, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            jump_to(doc, state, s.bank, s.call_addr);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("B%02x_A%04x", s.bank, (unsigned)s.call_addr);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(routine_name(doc, s.bank, s.call_addr).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/* ---- Routine Card ---- */

static void timeline_watch_card(const ApexRenderedDocument *doc, UiState *state);

/* lower-case identifier from text: letters and digits, words joined by '_' */
static std::string ident(const std::string &text, size_t max_words) {
    std::string out, word;
    size_t words = 0;
    for (size_t i = 0; i <= text.size() && words < max_words; i++) {
        char c = i < text.size() ? text[i] : ' ';
        if (isalnum((unsigned char)c)) {
            word += (char)tolower((unsigned char)c);
        } else if (!word.empty()) {
            out += (out.empty() ? "" : "_") + word;
            word.clear();
            words++;
        }
    }
    return out;
}

static bool generated_name(const std::string &n) {
    return n.empty() || (n.size() == 9 && n[0] == 'B' && n[3] == '_' && n[4] == 'A');
}

/* The text of a string at (bank, addr) (printable part, up to 40 chars). */
static std::string string_text(const ApexProject *p, uint8_t bank, uint32_t addr) {
    size_t off;
    if (bank == 0xffu)
        off = p->paged_size + (addr - 0x8000u);
    else {
        int bi = bank_index_for_id(p->rom.data, p->banks, bank);
        if (bi < 0)
            return "";
        off = (size_t)bi * 0x4000u + (addr - 0x4000u);
    }
    std::string t;
    for (size_t k = off; k < p->rom.size && p->rom.data[k] && t.size() < 40; k++) {
        uint8_t ch = p->rom.data[k];
        t += (ch >= 0x20 && ch < 0x7f) ? (char)ch : ' ';
    }
    return t;
}

void render_card_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state) {
    const ApexRenderedDocument *doc = *document_ptr;
    CardWindowState &cs = state->card_state;
    /* the routine: pinned, or the one around the cursor */
    uint8_t b = cs.bank;
    uint32_t a = cs.addr;
    if (!cs.pinned) {
        if (!doc || state->selected_line >= doc->line_count)
            return;
        size_t li = state->selected_line;
        while (li > 0 && !doc->lines[li].has_location)
            li--;
        size_t rs;
        if (!doc->lines[li].has_location ||
            !find_routine_start(doc, doc->lines[li].bank, doc->lines[li].cpu_addr, &rs))
            return;
        b = doc->lines[rs].bank;
        a = doc->lines[rs].cpu_addr;
        /* a mid-routine label: the card is for the routine that contains it */
        uint32_t st;
        if (apex_wpc_routine_at(project, b, a, &st) && st != a) {
            ApexRoutineCard probe;
            if (apex_routine_card(project, b, st, &probe) == 0) {
                if (apex_routine_card_has(&probe, b, a))
                    a = st;
                apex_routine_card_free(&probe);
            }
        }
    }
    if (!cs.valid || cs.bank != b || cs.addr != a || cs.doc_ptr != (const void *)doc) {
        apex_routine_card_free(&cs.card);
        cs.bank = b;
        cs.addr = a;
        cs.doc_ptr = doc;
        cs.valid = apex_routine_card(project, b, a, &cs.card) == 0;
    }
    if (ImGui::Checkbox("pin", &cs.pinned) && !cs.pinned)
        cs.valid = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep this routine while the cursor moves");
    if (!cs.valid) {
        ImGui::SameLine();
        ImGui::TextDisabled("no code at the cursor");
        return;
    }
    const ApexRoutineCard &c = cs.card;
    std::string name = label_at_address(doc, state, b, a);
    ImGui::SameLine();
    if (ImGui::SmallButton("go"))
        jump_to(doc, state, b, a);
    ImGui::SameLine();
    if (ImGui::SmallButton("watch")) {
        timeline_watch_card(doc, state);
        state->show_timeline = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Record its calls in the Event Timeline (with A, B, X)");
    ImGui::SameLine();
    ImGui::Text("%s", name.empty() ? "(no label)" : name.c_str());
    ImGui::TextDisabled("B%02x_A%04x-A%04x, %lu instruction(s), %d return(s)", b, (unsigned)c.low,
                        (unsigned)c.high, (unsigned long)c.insns, c.returns);
    const PinmameState &pm = state->pinmame;
    if (!pm.cov_reached.empty()) {
        size_t ran = 0;
        for (size_t i = 0; i < c.insn_count; i++)
            ran += pm.cov_reached.count(((uint32_t)c.insn[i].bank << 16) | c.insn[i].addr) != 0;
        ImGui::SameLine();
        if (ran)
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.55f, 1.0f), "ran: %lu/%lu", (unsigned long)ran,
                               (unsigned long)c.insn_count);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "never ran in PinMAME");
    }
    const char *docs = config_doc_at(&project->docs, b, a);
    if (docs)
        ImGui::TextWrapped("%s", docs);

    /* bits the routine sets / clears / tests (from the Bit Map) */
    BitsWindowState &bs = state->bits_state;
    if (!bs.computed || bs.doc_ptr != (const void *)doc)
        bits_compute(project, doc, state, bs);
    std::vector<const ApexBitSite *> bits;
    for (size_t i = 0; i < bs.map.site_count; i++)
        if (apex_routine_card_has(&c, bs.map.sites[i].bank, bs.map.sites[i].call_addr))
            bits.push_back(&bs.map.sites[i]);

    /* name suggestions */
    std::vector<std::string> sugg;
    auto add_s = [&](const std::string &n) {
        if (!n.empty() && n.size() < 60 && std::find(sugg.begin(), sugg.end(), n) == sugg.end())
            sugg.push_back(n);
    };
    for (size_t i = 0; i < c.data_count && sugg.size() < 3; i++)
        if (c.data[i].is_string) {
            std::string w = ident(string_text(project, c.data[i].bank, c.data[i].addr), 3);
            if (!w.empty())
                add_s("show_" + w);
        }
    for (const ApexBitSite *s : bits) {
        const char *en = s->type_name
                             ? config_type_enum_name(&project->config_types, s->type_name, s->bit)
                             : nullptr;
        if (!en)
            continue;
        std::string op = bit_op_name(project, bs.map.paths[s->path], bs.path_names[(size_t)s->path],
                                     (size_t)s->path);
        std::string verb = op == "on" || op == "set"      ? "set"
                           : op == "off" || op == "clear" ? "clear"
                           : op == "toggle"               ? "toggle"
                                                          : "check";
        add_s(verb + "_" + ident(en, 4));
    }
    for (size_t i = 0; i < c.ram_count; i++) {
        for (size_t k = 0; k < project->symbols.count; k++) {
            if (project->symbols.items[k].value != c.ram[i].addr)
                continue;
            add_s((c.ram[i].writes ? "update_" : "read_") +
                  ident(project->symbols.items[k].name, 4));
            break;
        }
    }
    if (c.insns <= 6 && c.callee_count == 1) {
        std::string cn = label_at_address(doc, state, c.callees[0].bank, c.callees[0].addr);
        if (!generated_name(cn))
            add_s(cn + "_wrapper");
    }
    uint32_t key = ((uint32_t)b << 16) | a;
    if (cs.name_for != key) {
        snprintf(cs.name_buf, sizeof(cs.name_buf), "%s", generated_name(name) ? "" : name.c_str());
        cs.name_for = key;
    }
    ImGui::SetNextItemWidth(ui_w(280));
    ImGui::InputTextWithHint("##cardname", "label", cs.name_buf, sizeof(cs.name_buf));
    ImGui::SameLine();
    if (ImGui::Button("Rename") && cs.name_buf[0]) {
        if (apex_project_set_label(project, 1, b, a, cs.name_buf) == 0) {
            state->overlay_dirty = true;
            set_status(state, "routine renamed");
            cs.valid = false;
            rerender_and_reselect(project, document_ptr, state, b, a);
            return;
        }
        set_status(state, apex_project_last_error(project));
    }
    if (!sugg.empty()) {
        ImGui::TextDisabled("suggestions:");
        for (const auto &sg : sugg) {
            ImGui::SameLine();
            if (ImGui::SmallButton(sg.c_str()))
                snprintf(cs.name_buf, sizeof(cs.name_buf), "%s", sg.c_str());
        }
    }

    auto loc_row = [&](uint8_t lb, uint32_t la, const std::string &txt, int id) {
        ImGui::PushID(id);
        if (ImGui::Selectable(txt.c_str(), false))
            jump_to(doc, state, lb, la);
        ImGui::PopID();
    };
    char tmp[256];
    if (ImGui::CollapsingHeader("Callers", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!c.caller_count)
            ImGui::TextDisabled("none known (an entry, a computed call or a table)");
        for (size_t i = 0; i < c.caller_count; i++) {
            const ApexCardCaller &cl = c.callers[i];
            snprintf(tmp, sizeof(tmp), "B%02x_A%04x  %s%s", cl.bank, (unsigned)cl.addr,
                     cl.from_table ? "(table) " : "", routine_name(doc, cl.bank, cl.addr).c_str());
            loc_row(cl.bank, cl.addr, tmp, (int)i);
        }
    }
    if (ImGui::CollapsingHeader("Calls", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < c.callee_count; i++) {
            const ApexCardCallee &ce = c.callees[i];
            std::string n = label_at_address(doc, state, ce.bank, ce.addr);
            snprintf(tmp, sizeof(tmp), "%s%s%s%s", n.empty() ? "" : n.c_str(),
                     ce.far ? "  (far)" : "", ce.tail ? "  (jump)" : "", ce.count > 1 ? "  x" : "");
            std::string row = tmp;
            if (n.empty()) {
                char l[24];
                snprintf(l, sizeof(l), "B%02x_A%04x", ce.bank, (unsigned)ce.addr);
                row = l + row;
            }
            if (ce.count > 1)
                row += std::to_string(ce.count);
            loc_row(ce.bank, ce.addr, row, 1000 + (int)i);
        }
    }
    if (!bits.empty() && ImGui::CollapsingHeader("Bits", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < bits.size(); i++) {
            const ApexBitSite *s = bits[i];
            const char *en =
                s->type_name ? config_type_enum_name(&project->config_types, s->type_name, s->bit)
                             : nullptr;
            std::string op = bit_op_name(project, bs.map.paths[s->path],
                                         bs.path_names[(size_t)s->path], (size_t)s->path);
            snprintf(tmp, sizeof(tmp), "%-6s matrix 0x%02x bit 0x%02x %s", op.c_str(), s->matrix,
                     (unsigned)s->bit, en ? en : "");
            loc_row(s->bank, s->call_addr, tmp, 2000 + (int)i);
        }
    }
    if (c.ram_count && ImGui::CollapsingHeader("RAM", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < c.ram_count; i++) {
            const char *sym = nullptr;
            for (size_t k = 0; k < project->symbols.count && !sym; k++)
                if (project->symbols.items[k].value == c.ram[i].addr)
                    sym = project->symbols.items[k].name;
            ImGui::Text("0x%04x  %-5s %s", (unsigned)c.ram[i].addr,
                        c.ram[i].writes ? (c.ram[i].reads ? "r/w" : "write") : "read",
                        sym ? sym : "");
        }
    }
    if (c.data_count && ImGui::CollapsingHeader("Data", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < c.data_count; i++) {
            const ApexCardData &d = c.data[i];
            std::string n = label_at_address(doc, state, d.bank, d.addr);
            snprintf(tmp, sizeof(tmp), "B%02x_A%04x  %s%s%s%s", d.bank, (unsigned)d.addr, n.c_str(),
                     d.is_string ? "  \"" : "",
                     d.is_string ? string_text(project, d.bank, d.addr).c_str() : "",
                     d.is_string ? "\"" : "");
            loc_row(d.bank, d.addr, tmp, 3000 + (int)i);
        }
    }
}

/* ---- Event Timeline ---- */

/* The name of bit `bit` of matrix `m` from the bit map's types. */
static const char *bit_name(const ApexProject *p, const ApexBitMap &map, int m, int bit) {
    const char *tn = nullptr;
    for (size_t i = 0; i < map.site_count && !tn; i++)
        if (map.sites[i].matrix == m && (int)map.sites[i].bit == bit)
            tn = map.sites[i].type_name;
    if (!tn)
        tn = map.matrix_type[m & 0xff];
    return tn ? config_type_enum_name(&p->config_types, tn, (uint32_t)bit) : nullptr;
}

static void timeline_note(TimelineState &tl, const std::string &text) {
    TimelineEvent e;
    e.t = ImGui::GetTime() - tl.t0;
    e.kind = 3;
    e.text = text;
    tl.events.push_back(e);
}

/* (Re)install the tracepoints: bit routines (tests only when wanted) and
   the watched routines. */
static void timeline_points(ApexProject *project, const ApexRenderedDocument *doc, UiState *state) {
    TimelineState &tl = state->timeline;
    BitsWindowState &bs = state->bits_state;
    int port = state->pinmame.port;
    if (!bs.computed || bs.doc_ptr != (const void *)doc)
        bits_compute(project, doc, state, bs);
    apex_pinmame_tracepoint_clear(port);
    tl.tp_role.clear();
    tl.tp_addr.clear();
    for (size_t k = 0; tl.want_bits && k < bs.map.path_count; k++) {
        const ApexBitPath &pa = bs.map.paths[k];
        if (pa.is_test && !tl.want_tests)
            continue;
        if (apex_pinmame_tracepoint_add(port, pa.addr, pa.bank == 0xffu ? -1 : pa.bank)) {
            tl.tp_role.push_back(-1 - (int)k);
            tl.tp_addr.push_back(pa.addr);
        }
    }
    for (size_t k = 0; k < tl.watches.size(); k++) {
        const TimelineWatch &w = tl.watches[k];
        if (apex_pinmame_tracepoint_add(port, w.addr, w.bank == 0xffu ? -1 : w.bank)) {
            tl.tp_role.push_back((int)k);
            tl.tp_addr.push_back(w.addr);
        }
    }
    long hits = 0;
    std::vector<ApexPinmameTpEntry> log;
    apex_pinmame_tracepoints(port, hits, log);
    tl.hits_seen = hits;
}

static void timeline_start(ApexProject *project, const ApexRenderedDocument *doc, UiState *state) {
    TimelineState &tl = state->timeline;
    tl.events.clear();
    tl.t0 = ImGui::GetTime();
    tl.next_poll = 0.0;
    timeline_points(project, doc, state);
    tl.sw_state.clear();
    std::vector<ApexPinmameSwitch> sw;
    if (apex_pinmame_switches(state->pinmame.port, sw))
        for (const auto &s : sw) {
            if (s.num >= (int)tl.sw_state.size())
                tl.sw_state.resize((size_t)s.num + 1, -1);
            tl.sw_state[(size_t)s.num] = s.active;
        }
    tl.recording = true;
    timeline_note(tl, "recording started");
}

static void timeline_stop(UiState *state) {
    TimelineState &tl = state->timeline;
    if (!tl.recording)
        return;
    tl.recording = false;
    if (state->pinmame.connected)
        apex_pinmame_tracepoint_clear(state->pinmame.port);
    timeline_note(tl, "recording stopped");
}

void timeline_pump(ApexProject *project, const ApexRenderedDocument **document_ptr,
                   UiState *state) {
    TimelineState &tl = state->timeline;
    if (!tl.recording)
        return;
    PinmameState &pm = state->pinmame;
    if (!pm.connected) {
        tl.recording = false;
        timeline_note(tl, "PinMAME disconnected");
        return;
    }
    if (tl.points_dirty) {
        tl.points_dirty = false;
        timeline_points(project, *document_ptr, state);
    }
    double now = ImGui::GetTime();
    if (now < tl.next_poll)
        return;
    tl.next_poll = now + 0.1;
    double t = now - tl.t0;
    const BitsWindowState &bs = state->bits_state;
    /* tracepoints: the newest (hits - seen) log entries are new */
    long hits = 0;
    std::vector<ApexPinmameTpEntry> log;
    if (apex_pinmame_tracepoints(pm.port, hits, log)) {
        long fresh = hits - tl.hits_seen;
        tl.hits_seen = hits;
        if (fresh > (long)log.size()) {
            char m[64];
            snprintf(m, sizeof(m), "%ld event(s) lost (ring full)", fresh - (long)log.size());
            timeline_note(tl, m);
            fresh = (long)log.size();
        }
        for (size_t i = log.size() - (size_t)(fresh > 0 ? fresh : 0); i < log.size(); i++) {
            const ApexPinmameTpEntry &e = log[i];
            int role = 0;
            bool found = false;
            for (size_t k = 0; k < tl.tp_addr.size() && !found; k++)
                if (tl.tp_addr[k] == (uint32_t)e.pc) {
                    role = tl.tp_role[k];
                    found = true;
                }
            if (!found)
                continue;
            TimelineEvent ev;
            ev.t = t;
            char tx[200];
            if (role < 0) {
                int k = -1 - role;
                if (k >= (int)bs.map.path_count)
                    continue;
                ev.kind = 1;
                ev.path = k;
                ev.matrix = e.b;
                ev.bit = e.a;
                ev.bank = bs.map.paths[k].bank;
                ev.addr = bs.map.paths[k].addr;
                std::string op =
                    bit_op_name(project, bs.map.paths[k], bs.path_names[(size_t)k], (size_t)k);
                const char *en = bit_name(project, bs.map, e.b, e.a);
                snprintf(tx, sizeof(tx), "%-6s matrix 0x%02x bit 0x%02x %s", op.c_str(), e.b, e.a,
                         en ? en : "");
            } else {
                if (role >= (int)tl.watches.size())
                    continue;
                const TimelineWatch &w = tl.watches[(size_t)role];
                ev.kind = 2;
                ev.bank = w.bank;
                ev.addr = w.addr;
                snprintf(tx, sizeof(tx), "%s  A=%02x B=%02x X=%04lx", w.name.c_str(), e.a, e.b,
                         e.x);
            }
            ev.text = tx;
            tl.events.push_back(ev);
        }
    }
    /* switches: changes since the last poll */
    if (tl.want_switches) {
        std::vector<ApexPinmameSwitch> sw;
        if (apex_pinmame_switches(pm.port, sw)) {
            for (const auto &s : sw) {
                if (s.num < 0)
                    continue;
                if (s.num >= (int)tl.sw_state.size())
                    tl.sw_state.resize((size_t)s.num + 1, -1);
                int &old = tl.sw_state[(size_t)s.num];
                if (old >= 0 && old != s.active) {
                    TimelineEvent ev;
                    ev.t = t;
                    ev.kind = 0;
                    ev.bit = s.num;
                    ev.on = s.active;
                    char tx[160];
                    snprintf(tx, sizeof(tx), "switch %d %s %s", s.num, s.name.c_str(),
                             s.active ? "closed" : "open");
                    ev.text = tx;
                    tl.events.push_back(ev);
                }
                old = s.active;
            }
        }
    }
    if (tl.events.size() > 20000)
        tl.events.erase(tl.events.begin(), tl.events.begin() + 5000);
}

/* Add the routine at the cursor's Routine Card to the watched routines. */
static void timeline_watch_card(const ApexRenderedDocument *doc, UiState *state) {
    CardWindowState &cs = state->card_state;
    TimelineState &tl = state->timeline;
    if (!cs.valid)
        return;
    for (const auto &w : tl.watches)
        if (w.bank == cs.bank && w.addr == cs.addr)
            return;
    std::string n = label_at_address(doc, state, cs.bank, cs.addr);
    if (n.empty()) {
        char b[16];
        snprintf(b, sizeof(b), "B%02x_A%04x", cs.bank, (unsigned)cs.addr);
        n = b;
    }
    tl.watches.push_back({cs.bank, cs.addr, n});
    tl.points_dirty = true;
}

static ImU32 event_color(int kind) {
    switch (kind) {
    case 0:
        return IM_COL32(110, 190, 255, 255); /* switch */
    case 1:
        return IM_COL32(255, 190, 70, 255); /* bit */
    case 2:
        return IM_COL32(120, 230, 140, 255); /* watched routine */
    default:
        return IM_COL32(160, 160, 160, 255);
    }
}

void render_timeline_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                            UiState *state) {
    const ApexRenderedDocument *doc = *document_ptr;
    TimelineState &tl = state->timeline;
    const PinmameState &pm = state->pinmame;
    ImGui::BeginDisabled(!pm.connected && !tl.recording);
    if (!tl.recording) {
        if (ImGui::Button("Record"))
            timeline_start(project, doc, state);
    } else if (ImGui::Button("Stop")) {
        timeline_stop(state);
    }
    ImGui::EndDisabled();
    if (!pm.connected && !tl.recording) {
        ImGui::SameLine();
        ImGui::TextDisabled("(connect PinMAME first)");
    } else if (pm.connected && pm.info.paused) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "PinMAME is paused");
        ImGui::SameLine();
        if (ImGui::SmallButton("resume"))
            apex_pinmame_control(pm.port, "resume");
    }
    ImGui::SameLine();
    bool re = false;
    re |= ImGui::Checkbox("switches", &tl.want_switches);
    ImGui::SameLine();
    re |= ImGui::Checkbox("lamps / flags", &tl.want_bits);
    ImGui::SameLine();
    re |= ImGui::Checkbox("bit tests", &tl.want_tests);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Also record the bit routines that only test a bit (very frequent)");
    if (re && tl.recording)
        timeline_points(project, doc, state);
    ImGui::SameLine();
    if (ImGui::SmallButton("clear"))
        tl.events.clear();

    /* watched routines */
    ImGui::TextDisabled("watched routines:");
    for (size_t k = 0; k < tl.watches.size(); k++) {
        ImGui::SameLine();
        ImGui::PushID((int)k);
        if (ImGui::SmallButton((tl.watches[k].name + "  x").c_str())) {
            tl.watches.erase(tl.watches.begin() + (long)k);
            if (tl.recording)
                timeline_points(project, doc, state);
            ImGui::PopID();
            break;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("remove");
        ImGui::PopID();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ routine at cursor")) {
        CardWindowState &cs = state->card_state;
        if (!state->show_card) {
            state->show_card = true; /* the card finds the routine at the cursor */
            set_status(state, "Routine Card opened: click + again, or 'watch' on the card");
        } else if (cs.valid) {
            timeline_watch_card(doc, state);
            if (tl.recording)
                timeline_points(project, doc, state);
        } else {
            set_status(state, "open the Routine Card on the routine first");
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Record every call of the Routine Card's routine (with A, B, X), e.g. "
                          "a sound or display routine");

    /* lanes: switches / bits / routines over the recording */
    double span = 1.0;
    for (const auto &e : tl.events)
        span = std::max(span, e.t);
    float w = ImGui::GetContentRegionAvail().x, lane = 12.0f;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + lane * 3 + 4), IM_COL32(25, 25, 30, 255));
    for (const auto &e : tl.events) {
        if (e.kind > 2 || (e.kind == 1 && tl.hide_matrix[e.matrix & 0xff]))
            continue;
        float x = o.x + (float)(e.t / span) * (w - 2) + 1;
        float y = o.y + 2 + lane * (float)e.kind;
        dl->AddLine(ImVec2(x, y), ImVec2(x, y + lane - 2), event_color(e.kind));
    }
    ImGui::Dummy(ImVec2(w, lane * 3 + 4));
    if (ImGui::IsItemHovered()) {
        double tm = (ImGui::GetIO().MousePos.x - o.x) / (double)w * span;
        ImGui::SetTooltip("%.1f s  (rows: switches, lamps / flags, watched routines)", tm);
    }

    /* matrices seen: show / hide their bit events (e.g. the attract lamp show) */
    bool seen[256] = {};
    for (const auto &e : tl.events)
        if (e.kind == 1)
            seen[e.matrix & 0xff] = true;
    bool any = false;
    for (int m = 0; m < 256; m++) {
        if (!seen[m])
            continue;
        if (!any)
            ImGui::TextDisabled("matrices:");
        any = true;
        ImGui::SameLine();
        char lb[16];
        snprintf(lb, sizeof(lb), "0x%02x", m);
        bool show = !tl.hide_matrix[m];
        if (ImGui::Checkbox(lb, &show))
            tl.hide_matrix[m] = !show;
    }
    ImGui::SetNextItemWidth(ui_w(220));
    ImGui::InputTextWithHint("##tlfilter", "filter", tl.filter, sizeof(tl.filter));
    ImGui::SameLine();
    ImGui::Checkbox("follow", &tl.follow);
    ImGui::SameLine();
    ImGui::TextDisabled("%lu event(s)", (unsigned long)tl.events.size());
    if (!ImGui::BeginTable("timeline", 2,
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY,
                           ImVec2(0, -1)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, ui_w(64.0f));
    ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < tl.events.size(); i++) {
        const TimelineEvent &e = tl.events[i];
        if (tl.filter[0] && !strcasestr(e.text.c_str(), tl.filter))
            continue;
        if (e.kind == 1 && tl.hide_matrix[e.matrix & 0xff])
            continue;
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%7.2f", e.t);
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, event_color(e.kind));
        bool click = ImGui::Selectable(e.text.c_str(), false,
                                       ImGuiSelectableFlags_SpanAllColumns |
                                           ImGuiSelectableFlags_AllowOverlap);
        ImGui::PopStyleColor();
        if (click && e.kind == 1) {
            /* show the bit in the Bit Map: its sites are where the game does it */
            BitsWindowState &bs = state->bits_state;
            bs.matrix = e.matrix;
            bs.selected = -1;
            for (size_t q = 0; q < bs.map.site_count; q++)
                if (bs.map.sites[q].matrix == e.matrix && (int)bs.map.sites[q].bit == e.bit) {
                    bs.selected = (int)q;
                    break;
                }
            state->show_bits = true;
        } else if (click && e.kind == 2) {
            jump_to(doc, state, e.bank, e.addr);
        }
        if (ImGui::IsItemHovered() && e.kind == 1)
            ImGui::SetTooltip("Click: the bit in the Bit Map, with the code that does it");
        ImGui::PopID();
    }
    if (tl.follow && tl.recording)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndTable();
}

/* ---- adjustment / audit names (Types window) ---- */

/* Add the values of `generated` ("0xNN:NAME, ...") that `type` does not have
   yet (a value already named keeps its name); make the type if needed.
   Returns the number added, -1 on failure. */
static int merge_type_values(ApexProject *p, const char *type, const char *generated) {
    const ConfigType *ct = find_config_type(&p->config_types, type);
    std::string vs;
    char tmp[16];
    for (size_t i = 0; ct && i < ct->value_count; i++) {
        snprintf(tmp, sizeof(tmp), "0x%02x:", (unsigned)ct->values[i].value);
        vs += (vs.empty() ? "" : ", ") + std::string(tmp) + ct->values[i].name;
    }
    int added = 0;
    for (const char *g = generated; *g;) {
        const char *e = strstr(g, ", ");
        std::string item(g, e ? (size_t)(e - g) : strlen(g));
        g += item.size() + (e ? 2 : 0);
        unsigned long v = strtoul(item.c_str(), nullptr, 16);
        bool have = false;
        for (size_t i = 0; ct && i < ct->value_count && !have; i++)
            have = ct->values[i].value == v;
        /* a name another value already uses would clash: skip it */
        size_t colon = item.find(':');
        std::string nm = colon == std::string::npos ? "" : item.substr(colon + 1);
        for (size_t i = 0; ct && i < ct->value_count && !have; i++)
            have = nm == ct->values[i].name;
        if (have)
            continue;
        vs += (vs.empty() ? "" : ", ") + item;
        added++;
    }
    if (apex_project_set_type(p, type, ct ? ct->kind == TABLE_WORD : 0, vs.c_str()) != 0)
        return -1;
    return added;
}

void render_adjnames_section(ApexProject *p, UiState *s) {
    static ApexAdjTables t{};
    static bool loaded = false;
    static int feature = -2; /* -2 = the detected one */
    if (!ImGui::CollapsingHeader("Adjustment / audit names from the ROM"))
        return;
    if (!loaded) {
        apex_adj_find_tables(p, &t);
        loaded = true;
        feature = -2;
    }
    if (t.std_adj < 0) {
        ImGui::TextWrapped("No standard adjustment table found (a NULL-led string table whose "
                           "entry 1 is BALLS PER GAME).");
        return;
    }
    const ApexNameTable &sa = t.tables[t.std_adj];
    ImGui::Text("standard adjustments: B%02x_A%04x, %lu names (index 0x80 + n)", sa.bank,
                (unsigned)sa.addr, (unsigned long)sa.count - 1);
    if (t.std_aud >= 0)
        ImGui::Text("standard audits:      B%02x_A%04x, %lu names", t.tables[t.std_aud].bank,
                    (unsigned)t.tables[t.std_aud].addr,
                    (unsigned long)t.tables[t.std_aud].count - 1);
    if (feature == -2)
        feature = t.feat_adj;
    /* the feature table: the detected guess or any other NULL-led table */
    char cur[96];
    if (feature >= 0)
        snprintf(cur, sizeof(cur), "B%02x_A%04x  %s", t.tables[feature].bank,
                 (unsigned)t.tables[feature].addr,
                 t.tables[feature].count > 1 ? t.tables[feature].names[1] : "");
    else
        snprintf(cur, sizeof(cur), "(none)");
    ImGui::SetNextItemWidth(ui_w(320));
    if (ImGui::BeginCombo("feature adjustments", cur)) {
        if (ImGui::Selectable("(none)", feature < 0))
            feature = -1;
        for (size_t i = 0; i < t.count; i++) {
            if ((int)i == t.std_adj || (int)i == t.std_aud)
                continue;
            char lb[128];
            snprintf(lb, sizeof(lb), "B%02x_A%04x  %3lu  %s%s", t.tables[i].bank,
                     (unsigned)t.tables[i].addr, (unsigned long)t.tables[i].count - 1,
                     t.tables[i].count > 1 ? t.tables[i].names[1] : "",
                     (int)i == t.feat_adj ? "   (detected)" : "");
            if (ImGui::Selectable(lb, (int)i == feature))
                feature = (int)i;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The game's own adjustments (index n); detected as the settings-like "
                          "(adjustments, audits) pair -- pick another table if the guess is "
                          "wrong");
    if (feature >= 0 && feature + 1 < (int)t.count &&
        t.tables[feature + 1].bank == t.tables[feature].bank)
        ImGui::TextDisabled("feature audits: B%02x_A%04x (the next table)",
                            t.tables[feature + 1].bank, (unsigned)t.tables[feature + 1].addr);
    if (ImGui::Button("Name TYPE_ADJ_INDEX and the audit types")) {
        int keep_fa = t.feat_adj, keep_fu = t.feat_aud;
        t.feat_adj = feature;
        t.feat_aud = feature >= 0 && feature + 1 < (int)t.count &&
                             t.tables[feature + 1].bank == t.tables[feature].bank
                         ? feature + 1
                         : -1;
        char *v = apex_adj_type_values(&t, 1, -1);
        int n = merge_type_values(p, "TYPE_ADJ_INDEX", v);
        free(v);
        if (n >= 0 && t.std_aud >= 0) {
            v = apex_adj_type_values(&t, 0, t.std_aud);
            int k = merge_type_values(p, "TYPE_STD_AUDIT", v);
            n = k < 0 ? -1 : n + k;
            free(v);
        }
        if (n >= 0 && t.feat_aud >= 0) {
            v = apex_adj_type_values(&t, 0, t.feat_aud);
            int k = merge_type_values(p, "TYPE_FEATURE_AUDIT", v);
            n = k < 0 ? -1 : n + k;
            free(v);
        }
        t.feat_adj = keep_fa;
        t.feat_aud = keep_fu;
        char m[96];
        if (n < 0)
            snprintf(m, sizeof(m), "%s", apex_project_last_error(p));
        else
            snprintf(m, sizeof(m), "%d name(s) added (existing names kept)", n);
        set_status(s, m);
        s->overlay_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("TYPE_ADJ_INDEX: 0x80 + n = standard adjustment n, n = feature "
                          "adjustment n (as the OS's adjustment routines take them);\n"
                          "TYPE_STD_AUDIT / TYPE_FEATURE_AUDIT: the audit names by index.\n"
                          "Only values without a name are added.");
}
