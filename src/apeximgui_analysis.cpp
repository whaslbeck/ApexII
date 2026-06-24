#include "apeximgui_core.h"
#include <algorithm>
#include <cstring>
#include <strings.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>

static int is_symbol_boundary_char(char ch)
{
    return !std::isalnum((unsigned char)ch) && ch != '_' && ch != '.' && ch != '$';
}

static std::string line_target_search_text(const ApexRenderedLine *line)
{
    std::string text = line_to_string(line);
    size_t cp = text.find(';');
    if (cp == std::string::npos) {
        return text;
    }
    // Keep the full text for "; referenced_by ..." comment lines so that the
    // label tokens inside (e.g. "code:B20_A4007") are reachable by double-click.
    const char *after = text.c_str() + cp + 1;
    while (*after == ' ' || *after == '\t') after++;
    if (strncmp(after, "referenced_by", 13) == 0) {
        return text;
    }
    text.resize(cp);
    return text;
}

static void update_label_index(const ApexRenderedDocument *document, UiState *state)
{
    state->cached_labels.clear();
    for (size_t i = 0; i < document->line_count; i++) {
        const ApexRenderedLine *l = &document->lines[i];
        if (l->kind != APEX_RENDER_LINE_LABEL || !l->has_location) {
            continue;
        }
        state->cached_labels.push_back({i, l->bank, l->cpu_addr, label_name(l), l->block_kind});
    }
    std::sort(state->cached_labels.begin(), state->cached_labels.end(),
        [](const LabelIndexEntry &a, const LabelIndexEntry &b) {
            if (a.bank != b.bank) {
                return a.bank < b.bank;
            }
            if (a.cpu_addr != b.cpu_addr) {
                return a.cpu_addr < b.cpu_addr;
            }
            return a.name < b.name;
        });
    state->labels_valid = true;
}

static int get_or_create_graph_node(UiState *state, const ApexRenderedDocument *document,
                                    uint8_t bank, uint32_t addr, int layer)
{
    size_t lidx;
    if (!find_routine_start(document, bank, addr, &lidx)) {
        return -1;
    }
    const ApexRenderedLine *l = &document->lines[lidx];
    for (size_t i = 0; i < state->graph_nodes.size(); i++) {
        if (state->graph_nodes[i].bank == l->bank && state->graph_nodes[i].addr == l->cpu_addr) {
            return (int)i;
        }
    }
    GraphNode node = {};
    node.bank = l->bank;
    node.addr = l->cpu_addr;
    node.name = label_name(l);
    node.layer = layer;
    if (node.name.empty()) {
        char buf[32];
        snprintf(buf, 32, "B%02x_%04x", node.bank, node.addr);
        node.name = buf;
    }
    state->graph_nodes.push_back(node);
    return (int)state->graph_nodes.size() - 1;
}

static void traverse_graph_incoming(ApexProject *project, const ApexRenderedDocument *document,
                                    UiState *state, int node_idx, int depth)
{
    if (depth <= 0) {
        return;
    }
    auto refs = find_incoming_refs(project, document, state,
                                   state->graph_nodes[node_idx].bank,
                                   state->graph_nodes[node_idx].addr);
    for (auto &ref : refs) {
        int p_idx = get_or_create_graph_node(state, document, ref.bank, ref.cpu_addr,
                                             state->graph_nodes[node_idx].layer - 1);
        if (p_idx >= 0 && p_idx != node_idx) {
            bool exists = false;
            for (auto idx : state->graph_nodes[p_idx].callee_indices) {
                if (idx == (size_t)node_idx) {
                    exists = true;
                }
            }
            if (!exists) {
                state->graph_nodes[p_idx].callee_indices.push_back(node_idx);
                state->graph_nodes[node_idx].caller_indices.push_back(p_idx);
                traverse_graph_incoming(project, document, state, p_idx, depth - 1);
            }
        }
    }
}

static void traverse_graph_outgoing(ApexProject *project, const ApexRenderedDocument *document,
                                    UiState *state, int node_idx, int depth)
{
    if (depth <= 0) {
        return;
    }
    size_t lidx;
    if (!apex_render_find_line_by_address(document, state->graph_nodes[node_idx].bank,
                                          state->graph_nodes[node_idx].addr, &lidx)) {
        return;
    }
    for (size_t i = lidx; i < document->line_count; i++) {
        const ApexRenderedLine *l = &document->lines[i];
        if (i > lidx && l->kind == APEX_RENDER_LINE_LABEL) {
            break;
        }
        auto refs = find_outgoing_refs(project, document, state, l->bank, l->cpu_addr);
        for (auto &ref : refs) {
            if (ref.kind == "JSR" || ref.kind == "JMP" ||
                (ref.kind.size() > 0 && ref.kind[0] == 'B')) {
                int c_idx = get_or_create_graph_node(state, document, ref.bank, ref.cpu_addr,
                                                     state->graph_nodes[node_idx].layer + 1);
                if (c_idx >= 0 && c_idx != node_idx) {
                    bool exists = false;
                    for (auto idx : state->graph_nodes[node_idx].callee_indices) {
                        if (idx == (size_t)c_idx) {
                            exists = true;
                        }
                    }
                    if (!exists) {
                        state->graph_nodes[node_idx].callee_indices.push_back(c_idx);
                        state->graph_nodes[c_idx].caller_indices.push_back(node_idx);
                        traverse_graph_outgoing(project, document, state, c_idx, depth - 1);
                    }
                }
            }
        }
        if (l->kind == APEX_RENDER_LINE_INSTRUCTION &&
            (strstr(l->text, "RTS") || strstr(l->text, "RTI"))) {
            break;
        }
    }
}

static const SnapshotLabel *find_snapshot_label(const OriginalSnapshot *s, int hb, uint8_t b, uint32_t a)
{
    for (auto &i : s->labels) {
        if (i.has_bank == hb && i.bank == b && i.addr == a) {
            return &i;
        }
    }
    return NULL;
}

static const SnapshotEntry *find_snapshot_entry(const OriginalSnapshot *s, int hb, uint8_t b, uint32_t a)
{
    for (auto &i : s->entries) {
        if (i.has_bank == hb && i.bank == b && i.addr == a) {
            return &i;
        }
    }
    return NULL;
}

static const SnapshotData *find_snapshot_data(const OriginalSnapshot *s, uint8_t b, uint32_t a)
{
    for (auto &i : s->data) {
        if (i.bank == b && i.addr == a) {
            return &i;
        }
    }
    return NULL;
}

static const SnapshotTable *find_snapshot_table(const OriginalSnapshot *s, uint8_t b, uint32_t a)
{
    for (auto &i : s->tables) {
        if (i.bank == b && i.addr == a) {
            return &i;
        }
    }
    return NULL;
}

static const SnapshotDoc *find_snapshot_doc(const std::vector<SnapshotDoc> &ds, int hb, uint8_t b, uint32_t a)
{
    for (auto &i : ds) {
        if (i.has_bank == hb && i.bank == b && i.addr == a) {
            return &i;
        }
    }
    return NULL;
}

static const char *basename_ptr(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void write_config_address(FILE *o, int hb, uint8_t b, uint32_t a)
{
    if (hb) {
        fprintf(o, "B%02x_A%04x", b, (unsigned)a & 0xffff);
    } else if ((a & 0xffff) >= 0x8000u) {
        /* System bank: deterministically bank 0xFF even without explicit has_bank. */
        fprintf(o, "Bff_A%04x", (unsigned)a & 0xffff);
    } else {
        /* RAM (<0x4000) or ambiguous paged address — keep legacy format. */
        fprintf(o, "0x%04x", (unsigned)a & 0xffff);
    }
}

static void write_escaped_value(FILE *o, const char *v)
{
    int q = 0;
    for (const char *p = v; *p; p++) {
        if (*p == '\n' || *p == ';' || *p == '#' || *p == '\\' || *p == '"' ||
            isspace((unsigned char)*p)) {
            q = 1;
            break;
        }
    }
    if (!q) {
        fputs(v, o);
        return;
    }
    fputc('"', o);
    for (const char *p = v; *p; p++) {
        switch (*p) {
        case '\n': fputs("\\n", o);  break;
        case ';':  fputs("\\;", o);  break;
        case '#':  fputs("\\#", o);  break;
        case '\\': fputs("\\\\", o); break;
        case '"':  fputs("\\\"", o); break;
        default:   fputc(*p, o);     break;
        }
    }
    fputc('"', o);
}

static const char *table_field_kind_name(TableFieldKind k)
{
    switch (k) {
    case TABLE_PTR16_STRING:      return "ptr16_string";
    case TABLE_PTR16_DATA:        return "ptr16_data";
    case TABLE_PTR16_CODE:        return "ptr16_code";
    case TABLE_PTR16_TABLE:       return "ptr16_table";
    case TABLE_PTR16_DMD_FULLFRAME: return "ptr16_dmd_fullframe";
    case TABLE_PTR16_SPRITE:      return "ptr16_sprite";
    case TABLE_FAR_SPRITE:        return "far_sprite";
    case TABLE_FAR_STRING:        return "far_string";
    case TABLE_FAR_DATA:          return "far_data";
    case TABLE_FAR_TABLE:         return "far_table";
    case TABLE_FAR_CODE:          return "far_code";
    case TABLE_FAR_DMD_FULLFRAME: return "far_dmd_fullframe";
    case TABLE_BYTE:              return "byte";
    case TABLE_WORD:              return "word";
    default:                      return "byte";
    }
}

static std::string table_schema_to_string(const TableSchema *s)
{
    std::string t;
    for (size_t i = 0; i < s->count; i++) {
        if (i != 0) {
            t += ", ";
        }
        const char *name = s->items[i].type_name ? s->items[i].type_name
                                                  : table_field_kind_name(s->items[i].kind);
        t += name;
        if (!s->items[i].type_name && s->items[i].param &&
            (s->items[i].kind == TABLE_PTR16_SPRITE ||
             s->items[i].kind == TABLE_FAR_SPRITE)) {
            t += "(" + std::to_string(s->items[i].param) + ")";
        }
        if (s->items[i].count != 1u) {
            t += "[" + std::to_string(s->items[i].count) + "]";
        }
    }
    return t;
}

static std::string data_range_spec_string(const DataRange *r)
{
    if (r->kind == DATA_BYTES) {
        return "bytes[" + std::to_string(r->length) + "]";
    }
    switch (r->kind) {
    case DATA_STRING:          return "string";
    case DATA_STRING_LP:       return "string_lp";
    case DATA_STRING_FIXED:    return "string[" + std::to_string(r->length) + "]";
    case DATA_PTR16_STRING:    return "ptr16_string";
    case DATA_PTR16_DATA:      return "ptr16_data";
    case DATA_PTR16_CODE:      return "ptr16_code";
    case DATA_PTR16_TABLE:     return "ptr16_table";
    case DATA_FAR_STRING:      return "far_string";
    case DATA_FAR_DATA:        return "far_data";
    case DATA_FAR_TABLE:       return "far_table";
    case DATA_FAR_CODE:        return "far_code";
    case DATA_DMD_FULLFRAME:   return "dmd_fullframe";
    case DATA_FAR_DMD_FULLFRAME: return "far_dmd_fullframe";
    case DATA_SPRITE:          return "sprite";
    case DATA_SPRITE_NOHEADER: return "sprite_noheader[" + std::to_string(r->length) + "]";
    case DATA_PTR16_SPRITE:    return "ptr16_sprite";
    case DATA_FAR_SPRITE:      return "far_sprite";
    default:                   return "bytes[1]";
    }
}

std::string table_def_spec_string(const TableDef *t)
{
    std::string s = table_schema_to_string(&t->schema);
    if (t->has_header) {
        return "counted(" + s + ")";
    }
    return "rows[" + std::to_string(t->rows) + "](" + s + ")";
}

std::string inline_sig_spec_string(const InlineSignature *s)
{
    if (!s) {
        return "";
    }
    return table_schema_to_string(&s->schema);
}

void select_line(UiState *s, size_t i, int r)
{
    if (r) {
        s->request_scroll_to_selection = 1;
    }
    if (s->selected_line == i) {
        return;
    }
    if (!s->suppress_history_push) {
        s->history_back.push_back(s->selected_line);
        s->history_forward.clear();
    }
    s->selected_line = i;
    s->selection_end = i;
    if (!s->graph_pinned)
        s->graph_needs_rebuild = true;
}

void handle_line_selection(UiState *s, size_t i, bool sh)
{
    if (sh) {
        s->selection_end = i;
    } else {
        select_line(s, i, 0);
    }
}

void history_jump(UiState *s, int b)
{
    size_t t;
    if (b) {
        if (s->history_back.empty()) {
            return;
        }
        s->history_forward.push_back(s->selected_line);
        t = s->history_back.back();
        s->history_back.pop_back();
    } else {
        if (s->history_forward.empty()) {
            return;
        }
        s->history_back.push_back(s->selected_line);
        t = s->history_forward.back();
        s->history_forward.pop_back();
    }
    s->suppress_history_push = 1;
    s->selected_line = t;
    s->request_scroll_to_selection = 1;
    if (!s->graph_pinned)
        s->graph_needs_rebuild = true;
    s->suppress_history_push = 0;
}

void set_status(UiState *s, const char *m)
{
    snprintf(s->status_message, sizeof(s->status_message), "%s", m);
}

int selected_address(const ApexRenderedDocument *d, const UiState *s, uint8_t *b, uint32_t *a)
{
    if (!d || s->selected_line >= d->line_count) {
        return 0;
    }
    const auto *l = &d->lines[s->selected_line];
    if (!l->has_location) {
        return 0;
    }
    *b = l->bank;
    *a = l->cpu_addr;
    return 1;
}

LineByteSpan selected_line_span(const ApexProject *p, const ApexRenderedDocument *d, const UiState *s)
{
    LineByteSpan sp = {0, 0, 0};
    if (!p || !d || s->selected_line >= d->line_count || !d->lines[s->selected_line].has_location) {
        return sp;
    }
    size_t st = d->lines[s->selected_line].rom_addr;
    if (st >= p->rom.size) {
        return sp;
    }
    size_t en = st + 1;
    for (size_t i = s->selected_line + 1; i < d->line_count; i++) {
        if (d->lines[i].has_location && d->lines[i].rom_addr > st) {
            en = d->lines[i].rom_addr;
            break;
        }
    }
    if (en > p->rom.size) {
        en = p->rom.size;
    }
    sp.valid = 1;
    sp.start = st;
    sp.end = en;
    return sp;
}

int project_locate_rom_bytes(const ApexProject *p, uint8_t b, uint32_t a,
                             const uint8_t **s, size_t *l, size_t *ro)
{
    size_t o;
    if (b == 0xffu) {
        if (a < 0x8000 || a >= 0x10000) {
            return 0;
        }
        o = p->paged_size + (a - 0x8000);
    } else {
        if (a < 0x4000 || a >= 0x8000) {
            return 0;
        }
        int bi = bank_index_for_far_ref(p->rom.data, p->banks, b);
        if (bi < 0) {
            return 0;
        }
        o = (size_t)bi * 0x4000 + (a - 0x4000);
    }
    if (o >= p->rom.size) {
        return 0;
    }
    *s = p->rom.data + o;
    *l = p->rom.size - o;
    if (ro) {
        *ro = o;
    }
    return 1;
}

std::string line_to_string(const ApexRenderedLine *l)
{
    return std::string(l->text, l->length);
}

std::string label_name(const ApexRenderedLine *l)
{
    std::string n;
    for (size_t i = 0; i < l->length; i++) {
        char c = l->text[i];
        if (c == ':' || c == ';' || isspace((unsigned char)c)) {
            break;
        }
        n += c;
    }
    return n;
}

void ensure_label_index(const ApexRenderedDocument *d, UiState *s)
{
    if (!s->labels_valid) {
        update_label_index(d, s);
    }
}

int label_entry_matches_filter(const LabelIndexEntry &e, const char *f)
{
    if (!f || !*f) {
        return 1;
    }
    size_t fl = strlen(f);
    if (e.name.size() < fl) {
        return 0;
    }
    for (size_t i = 0; i + fl <= e.name.size(); i++) {
        size_t j;
        for (j = 0; j < fl; j++) {
            if (tolower((unsigned char)e.name[i+j]) != tolower((unsigned char)f[j])) {
                break;
            }
        }
        if (j == fl) {
            return 1;
        }
    }
    return 0;
}

std::string label_at_address(const ApexRenderedDocument *d, UiState *s, uint8_t b, uint32_t a)
{
    ensure_label_index(d, s);
    /* Binary search — cached_labels is sorted by (bank, cpu_addr). */
    const auto &cl = s->cached_labels;
    size_t lo = 0, hi = cl.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cl[mid].bank < b || (cl[mid].bank == b && cl[mid].cpu_addr < a))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < cl.size() && cl[lo].bank == b && cl[lo].cpu_addr == a)
        return cl[lo].name;
    return "";
}

std::vector<LineTargetEntry> find_line_targets(const ApexRenderedDocument *d, UiState *s,
                                               const ApexRenderedLine *l)
{
    std::vector<LineTargetEntry> ts;
    std::string t = line_target_search_text(l);
    size_t p = 0;
    ensure_label_index(d, s);
    while (p < t.size()) {
        while (p < t.size() && is_symbol_boundary_char(t[p])) {
            p++;
        }
        if (p >= t.size()) {
            break;
        }
        size_t st = p;
        while (p < t.size() && !is_symbol_boundary_char(t[p])) {
            p++;
        }
        size_t en = p;
        bool matched = false;
        for (auto &e : s->cached_labels) {
            if (e.name.size() != en - st ||
                strncasecmp(t.data() + st, e.name.data(), en - st) != 0) {
                continue;
            }
            if (l->kind == APEX_RENDER_LINE_LABEL && e.line_index == (size_t)(l - d->lines)) {
                continue;
            }
            bool dupe = false;
            for (auto &ti : ts) {
                if (ti.line_index == e.line_index) {
                    dupe = true;
                }
            }
            if (!dupe) {
                ts.push_back({e.line_index, e.bank, e.cpu_addr, e.name, st});
                matched = true;
            }
        }
        /* Fallback for Bxx_Ayyyy address tokens not in the label index (e.g. table
           row addresses embedded in "; referenced_by ... line:N[Bxx_Ayyyy]"). */
        if (!matched && en - st == 9) {
            unsigned tok_bank = 0, tok_addr = 0;
            char tok_buf[10];
            memcpy(tok_buf, t.data() + st, 9);
            tok_buf[9] = '\0';
            if (sscanf(tok_buf, "B%2x_A%4x", &tok_bank, &tok_addr) == 2) {
                size_t tli = 0;
                if (apex_render_find_line_by_address(d, (uint8_t)tok_bank,
                                                     (uint32_t)tok_addr, &tli)) {
                    bool dupe = false;
                    for (auto &ti : ts) {
                        if (ti.line_index == tli) { dupe = true; break; }
                    }
                    if (!dupe) {
                        ts.push_back({tli, (uint8_t)tok_bank, (uint32_t)tok_addr,
                                      std::string(tok_buf), st});
                    }
                }
            }
        }
    }
    std::sort(ts.begin(), ts.end(), [](const LineTargetEntry &a, const LineTargetEntry &b) {
        if (a.match_pos != b.match_pos) {
            return a.match_pos < b.match_pos;
        }
        return a.name.size() > b.name.size();
    });
    return ts;
}

int resolve_pointer_target(const ApexProject *p, const ApexRenderedLine *l,
                           uint8_t *tb, uint32_t *ta, int *f)
{
    const uint8_t *src;
    size_t len;
    if (!l->has_location) {
        return 0;
    }
    if (strstr(l->text, "PTR") || strstr(l->text, "CODE")) {
        if (!project_locate_rom_bytes(p, l->bank, l->cpu_addr, &src, &len, NULL) || len < 2u) {
            return 0;
        }
        *ta = read_be16(src);
        *tb = l->bank;
        *f = 0;
        if (strstr(l->text, "FAR")) {
            if (len < 3u) {
                return 0;
            }
            *tb = src[2];
            *f = 1;
        }
        return 1;
    }
    return 0;
}

int jump_to_first_line_target(const ApexRenderedDocument *d, UiState *s, const ApexRenderedLine *l)
{
    auto ts = find_line_targets(d, s, l);
    if (ts.empty()) {
        return 0;
    }
    select_line(s, ts[0].line_index, 1);
    return 1;
}

void follow_selected_link(const ApexRenderedDocument *d, UiState *s)
{
    if (d && s->selected_line < d->line_count) {
        jump_to_first_line_target(d, s, &d->lines[s->selected_line]);
    }
}

std::vector<RefEntry> find_incoming_refs(const ApexProject *p, const ApexRenderedDocument *d,
                                         UiState *s, uint8_t b, uint32_t a)
{
    std::vector<RefEntry> rs;
    /* Use binary search when sorted (always true after analysis). */
    size_t start = p->refs.sorted ? refs_lower_bound(&p->refs, b, a) : 0;
    for (size_t i = start; i < p->refs.count; i++) {
        const auto &r = p->refs.items[i];
        if (p->refs.sorted && (r.bank != b || r.addr != a)) {
            break; /* refs are sorted by (bank, addr) — past the range */
        }
        if (!p->refs.sorted && (r.bank != b || r.addr != a)) {
            continue;
        }
        size_t li;
        if (apex_render_find_line_by_address(d, r.source_bank, r.source_addr, &li)) {
            rs.push_back({li, r.source_bank, r.source_addr,
                          label_at_address(d, s, r.source_bank, r.source_addr),
                          r.kind ? r.kind : "",
                          r.row_index, r.row_cpu_addr});
        }
    }
    std::sort(rs.begin(), rs.end(), [](const RefEntry &a, const RefEntry &b) {
        if (a.bank != b.bank) return a.bank < b.bank;
        if (a.cpu_addr != b.cpu_addr) return a.cpu_addr < b.cpu_addr;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.row_index < b.row_index;
    });
    /* Collapse multiple references from the same source+kind into one (e.g. a
       text table that points at the same string from several rows) — they all
       navigate to the same source line, so distinct row indices are just noise
       and would also produce duplicate-looking popup rows. */
    rs.erase(std::unique(rs.begin(), rs.end(), [](const RefEntry &a, const RefEntry &b) {
        return a.bank == b.bank && a.cpu_addr == b.cpu_addr && a.kind == b.kind;
    }), rs.end());
    return rs;
}

std::vector<RefEntry> find_outgoing_refs(const ApexProject *p, const ApexRenderedDocument *d,
                                         UiState *s, uint8_t b, uint32_t a)
{
    std::vector<RefEntry> rs;
    for (size_t i = 0; i < p->refs.count; i++) {
        const auto &r = p->refs.items[i];
        size_t li;
        if (r.source_bank != b || r.source_addr != a) {
            continue;
        }
        if (apex_render_find_line_by_address(d, r.bank, r.addr, &li)) {
            rs.push_back({li, r.bank, r.addr,
                          label_at_address(d, s, r.bank, r.addr),
                          r.kind ? r.kind : "", -1, 0u});
        }
    }
    std::sort(rs.begin(), rs.end(), [](const RefEntry &a, const RefEntry &b) {
        if (a.bank != b.bank) return a.bank < b.bank;
        if (a.cpu_addr != b.cpu_addr) return a.cpu_addr < b.cpu_addr;
        return a.kind < b.kind;
    });
    rs.erase(std::unique(rs.begin(), rs.end(), [](const RefEntry &a, const RefEntry &b) {
        return a.bank == b.bank && a.cpu_addr == b.cpu_addr && a.kind == b.kind;
    }), rs.end());
    return rs;
}

int find_routine_start(const ApexRenderedDocument *d, uint8_t b, uint32_t a, size_t *o)
{
    size_t li;
    if (!apex_render_find_line_by_address(d, b, a, &li)) {
        return 0;
    }
    /* apex_render_find_line_by_address returns the first line at address a,
       which may be a doc-comment or transition line rendered before the actual
       label (both share the same cpu_addr).  Scan forward within the same
       address block first to find the label, before falling back to the
       backward walk — otherwise the backward walk overshoots into the
       preceding function. */
    /* Scan forward, skipping non-located lines (doc-comments with
       has_location=false that sit between the LOCATION marker and the label).
       Stop when hitting a located line with a different address. */
    for (size_t fwd = li; fwd < d->line_count; fwd++) {
        const ApexRenderedLine *fl = &d->lines[fwd];
        if (fl->has_location) {
            if (fl->bank != b || fl->cpu_addr != a) break;
            if (fl->kind == APEX_RENDER_LINE_LABEL) {
                *o = fwd;
                return 1;
            }
        }
        /* has_location=false: skip without stopping */
    }
    /* Fallback: walk backward to the nearest label (mid-function cursor). */
    while (li > 0) {
        if (d->lines[li].kind == APEX_RENDER_LINE_LABEL && d->lines[li].has_location) {
            *o = li;
            return 1;
        }
        li--;
    }
    *o = 0;
    return 1;
}

void rebuild_call_graph(ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    uint8_t b;
    uint32_t a;
    if (s->graph_pinned) {
        b = s->graph_pinned_bank;
        a = s->graph_pinned_addr;
    } else {
        if (!d || s->selected_line >= d->line_count) return;
        size_t li = s->selected_line;

        /* Step 1: find the nearest has_location line at or before the cursor.
           This resolves the reference address even when the cursor sits on a
           non-located doc-comment line (has_location=false). */
        size_t ref = li;
        while (ref > 0 && !d->lines[ref].has_location) ref--;

        bool found = false;
        if (d->lines[ref].has_location) {
            uint8_t  cb = d->lines[ref].bank;
            uint32_t ca = d->lines[ref].cpu_addr;

            /* Step 2: scan forward from ref looking for a LABEL at address
               (cb, ca).  Non-located lines (doc-comments between LOCATION
               marker and the actual label) are skipped; a located line with
               a different address stops the scan. */
            for (size_t fwd = ref; fwd < d->line_count; fwd++) {
                const ApexRenderedLine *fl = &d->lines[fwd];
                if (fl->has_location) {
                    if (fl->bank != cb || fl->cpu_addr != ca) break;
                    if (fl->kind == APEX_RENDER_LINE_LABEL) {
                        b = fl->bank;
                        a = fl->cpu_addr;
                        found = true;
                        break;
                    }
                }
                /* has_location=false: skip without stopping */
            }
        }

        if (!found) {
            /* Step 3: cursor is mid-function — walk backward to the nearest
               preceding label. */
            while (!(d->lines[li].kind == APEX_RENDER_LINE_LABEL &&
                     d->lines[li].has_location)) {
                if (li == 0) return;
                li--;
            }
            b = d->lines[li].bank;
            a = d->lines[li].cpu_addr;
        }
    }
    s->graph_nodes.clear();
    s->graph_root_idx = get_or_create_graph_node(s, d, b, a, 0);
    if (s->graph_root_idx < 0) {
        return;
    }
    traverse_graph_incoming(p, d, s, s->graph_root_idx, s->graph_depth_in);
    traverse_graph_outgoing(p, d, s, s->graph_root_idx, s->graph_depth_out);
    s->graph_needs_rebuild = false;
}

bool select_line_by_address(const ApexRenderedDocument *d, UiState *s)
{
    uint8_t b;
    uint32_t a;
    size_t li;
    if (!parse_target_address(s->goto_input, &b, &a)) {
        return false;
    }
    if (apex_render_find_line_by_address(d, b, a, &li)) {
        select_line(s, li, 1);
        return true;
    }
    /* No line starts at this exact address (e.g. mid-instruction or inside a
       multi-byte data block). Find the last line in the same bank with
       cpu_addr <= a — that is the line whose bytes span this address. */
    size_t best = (size_t)-1;
    for (size_t i = 0; i < d->line_count; i++) {
        const auto *l = &d->lines[i];
        if (!l->has_location || l->bank != b) {
            continue;
        }
        if (l->cpu_addr <= a) {
            best = i;
        } else {
            break;
        }
    }
    if (best != (size_t)-1) {
        select_line(s, best, 1);
        return true;
    }
    return false;
}

void jump_to_transition(const ApexRenderedDocument *d, UiState *s,
                        ApexRenderedTransitionKind k, int f)
{
    size_t li = s->selected_line;
    const auto *l = f ? apex_render_find_next_transition(d, li, k, &li)
                      : apex_render_find_prev_transition(d, li, k, &li);
    if (l) {
        select_line(s, li, 1);
    }
}

void move_selection_relative(const ApexRenderedDocument *d, UiState *s, int delta)
{
    if (!d || d->line_count == 0) {
        return;
    }
    size_t n;
    if (delta < 0) {
        size_t st = (size_t)-delta;
        n = (s->selected_line > st) ? s->selected_line - st : 0;
    } else {
        n = s->selected_line + (size_t)delta;
        if (n >= d->line_count) {
            n = d->line_count - 1;
        }
    }
    select_line(s, n, 1);
}

/* First located line of a distinct classification region — the anchor the N/P
   buttons stop at.  A boundary is any change in classification (block kind:
   code / data / table / sprite / unclassified / free) or the start of a new
   bank, so every region (incl. data sitting between a sprite and a table, and
   the first byte of each bank after an end-of-bank fill) is reachable. */
static bool nav_is_boundary(const ApexRenderedDocument *d, size_t i)
{
    const ApexRenderedLine *l = &d->lines[i];
    if (!l->has_location) {
        return false; /* anchors are real addresses so the hex view can follow */
    }
    for (size_t j = i; j > 0; j--) {
        const ApexRenderedLine *pv = &d->lines[j - 1];
        if (!pv->has_location) {
            continue;
        }
        if (pv->bank != l->bank) {
            return true; /* first located line of a new bank */
        }
        return pv->block_kind != l->block_kind; /* classification changed */
    }
    return true; /* very first located line = start of the first bank */
}

void jump_primary_transition(const ApexRenderedDocument *d, UiState *s, int f)
{
    if (!d || d->line_count == 0) {
        return;
    }
    if (f) {
        for (size_t i = s->selected_line + 1; i < d->line_count; i++) {
            if (nav_is_boundary(d, i)) {
                select_line(s, i, 1);
                return;
            }
        }
    } else {
        for (size_t i = s->selected_line + 1; i > 0; i--) {
            if (nav_is_boundary(d, i - 1)) {
                select_line(s, i - 1, 1);
                return;
            }
        }
    }
}

void sync_editor_state(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    uint8_t b;
    uint32_t a;
    if (s->editor_bound_line == s->selected_line) {
        return;
    }
    s->editor_bound_line = s->selected_line;
    s->edit_label_input[0] = '\0';
    s->edit_inline_count = 0;
    s->edit_inline_flow_stop = false;
    s->edit_schema_count = 0;
    s->edit_data_length = 1;
    s->edit_table_rows = 1;
    s->edit_table_is_rows = 0;
    if (!selected_address(d, s, &b, &a)) {
        s->edit_doc_input[0] = '\0';
        return;
    }
    std::string l = label_at_address(d, s, b, a);
    if (!l.empty()) {
        snprintf(s->edit_label_input, 128, "%s", l.c_str());
    }
    const auto *is = inline_signature_for(&p->inline_sigs, b, a);
    if (is) {
        std::string spec = inline_sig_spec_string(is);
        spec_to_fields(spec.c_str(), s->edit_inline_fields, &s->edit_inline_count,
                       APEX_MAX_EDIT_FIELDS, p);
        s->edit_inline_flow_stop = is->flow_stop != 0;
    }
    for (size_t i = 0; i < p->tables.count; i++) {
        const TableDef *td = &p->tables.items[i];
        if (td->bank == b && td->addr == a) {
            std::string spec = table_schema_to_string(&td->schema);
            spec_to_fields(spec.c_str(), s->edit_schema_fields, &s->edit_schema_count,
                           APEX_MAX_EDIT_FIELDS, p);
            s->edit_table_is_rows = td->has_header ? 0 : 1;
            s->edit_table_rows = (int)td->rows;
            break;
        }
    }
    load_doc_editor_buffer(p, s, b, a);
    const char *m = strstr(d->lines[s->selected_line].text, "; data type=");
    if (m) {
        m += 12;
        const char *end = m;
        while (*end && *end != '\n') end++;
        char sp[128];
        size_t len = (size_t)(end - m);
        if (len >= sizeof(sp)) len = sizeof(sp) - 1;
        memcpy(sp, m, len);
        sp[len] = '\0';
        if (strcmp(sp, "string") == 0) {
            s->edit_data_length = 0;
        } else if (strncmp(sp, "bytes[", 6) == 0) {
            s->edit_data_length = atoi(sp + 6);
            if (s->edit_data_length < 1) s->edit_data_length = 1;
        }
    }
}

void rerender_and_reselect(ApexProject *p, const ApexRenderedDocument **dp, UiState *s,
                           uint8_t b, uint32_t a)
{
    size_t li = 0;
    apex_project_analyze(p);
    *dp = apex_project_render(p, 0, 0);
    s->labels_valid = false;
    s->code_candidates_stale   = true;
    s->inline_candidates_stale = true;
    if (*dp && apex_render_find_line_by_address(*dp, b, a, &li)) {
        s->suppress_history_push = 1;
        s->selected_line = li;
        s->selection_end = li;
        s->request_scroll_to_selection = 1;
        s->editor_bound_line = (size_t)-1;
        s->suppress_history_push = 0;
    }
    s->overlay_dirty = true;
    sync_editor_state(p, *dp, s);
}

ApexRenderedBlockKind get_offset_kind(const ApexProject *project,
                                      const ApexRenderedDocument *document, size_t offset)
{
    (void)project;
    size_t line_idx;
    if (find_line_by_rom_offset(document, offset, &line_idx)) {
        return document->lines[line_idx].block_kind;
    }
    return APEX_RENDER_BLOCK_DATA;
}

void apply_code_at_selection(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    uint8_t b;
    uint32_t a;
    if (s->hex_is_edit_target) {
        if (!rom_offset_to_cpu_address(p, s->hex_selected_offset, &b, &a)) {
            return;
        }
    } else {
        if (!selected_address(*dp, s, &b, &a)) {
            set_status(s, "no selection");
            return;
        }
    }
    if (apex_project_set_kind(p, 1, b, a, APEX_KIND_CODE, NULL) == 0) {
        rerender_and_reselect(p, dp, s, b, a);
    } else {
        set_status(s, "set code failed (conflict with existing range?)");
    }
}

void apply_data_at_selection(ApexProject *p, const ApexRenderedDocument **dp, UiState *s,
                             const char *sp)
{
    uint8_t b;
    uint32_t a;
    if (s->hex_is_edit_target) {
        size_t base = s->hex_has_range
            ? std::min(s->hex_anchor_offset, s->hex_selected_offset)
            : s->hex_selected_offset;
        if (!rom_offset_to_cpu_address(p, base, &b, &a)) {
            return;
        }
    } else {
        if (!selected_address(*dp, s, &b, &a)) {
            return;
        }
    }
    if (apex_project_set_kind(p, 1, b, a, APEX_KIND_DATA, sp) == 0) {
        rerender_and_reselect(p, dp, s, b, a);
    } else {
        set_status(s, "set data failed");
    }
}

void apply_string_at_selection(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    uint8_t b;
    uint32_t a;
    if (s->hex_is_edit_target) {
        size_t base = s->hex_has_range
            ? std::min(s->hex_anchor_offset, s->hex_selected_offset)
            : s->hex_selected_offset;
        if (!rom_offset_to_cpu_address(p, base, &b, &a)) {
            return;
        }
    } else {
        if (!selected_address(*dp, s, &b, &a)) {
            return;
        }
    }
    if (apex_project_set_kind(p, 1, b, a, APEX_KIND_STRING, "string") == 0) {
        rerender_and_reselect(p, dp, s, b, a);
    }
}

void apply_string_lp_at_selection(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    uint8_t b;
    uint32_t a;
    if (s->hex_is_edit_target) {
        size_t base = s->hex_has_range
            ? std::min(s->hex_anchor_offset, s->hex_selected_offset)
            : s->hex_selected_offset;
        if (!rom_offset_to_cpu_address(p, base, &b, &a)) {
            return;
        }
    } else {
        if (!selected_address(*dp, s, &b, &a)) {
            return;
        }
    }
    if (apex_project_set_kind(p, 1, b, a, APEX_KIND_DATA, "string_lp") == 0) {
        rerender_and_reselect(p, dp, s, b, a);
    }
}

void apply_table_at_selection(ApexProject *p, const ApexRenderedDocument **dp, UiState *s,
                              const char *sp)
{
    uint8_t b;
    uint32_t a;
    if (s->hex_is_edit_target) {
        if (!rom_offset_to_cpu_address(p, s->hex_selected_offset, &b, &a)) {
            return;
        }
    } else {
        if (!selected_address(*dp, s, &b, &a)) {
            return;
        }
    }
    if (apex_project_set_kind(p, 1, b, a, APEX_KIND_TABLE, sp) == 0) {
        rerender_and_reselect(p, dp, s, b, a);
    }
}

void clear_kind_at_selection(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    uint8_t b;
    uint32_t a;
    if (s->hex_is_edit_target) {
        if (!rom_offset_to_cpu_address(p, s->hex_selected_offset, &b, &a)) {
            return;
        }
    } else {
        if (!selected_address(*dp, s, &b, &a)) {
            return;
        }
    }
    if (apex_project_clear_kind(p, 1, b, a) == 0) {
        rerender_and_reselect(p, dp, s, b, a);
    }
}

const ApexRenderedLine *find_first_line_in_bank(const ApexRenderedDocument *d, uint8_t b, size_t *li)
{
    for (size_t i = 0; i < d->line_count; i++) {
        if (d->lines[i].has_location && d->lines[i].bank == b) {
            if (li) {
                *li = i;
            }
            return &d->lines[i];
        }
    }
    return NULL;
}

int find_line_by_rom_offset(const ApexRenderedDocument *d, size_t o, size_t *li)
{
    for (size_t i = 0; i < d->line_count; i++) {
        if (!d->lines[i].has_location) {
            continue;
        }
        if (o == d->lines[i].rom_addr) {
            *li = i;
            return 1;
        }
        if (o < d->lines[i].rom_addr && i > 0 && d->lines[i-1].has_location) {
            *li = i - 1;
            return 1;
        }
    }
    if (d->line_count > 0 && d->lines[d->line_count-1].has_location) {
        *li = d->line_count - 1;
        return 1;
    }
    return 0;
}

int rom_offset_to_cpu_address(const ApexProject *project, size_t offset,
                              uint8_t *bank, uint32_t *cpu_addr)
{
    if (offset >= project->rom.size) {
        return 0;
    }
    if (offset >= project->paged_size) {
        *bank = 0xffu;
        *cpu_addr = 0x8000u + (uint32_t)(offset - project->paged_size);
        return 1;
    }
    int bank_idx = (int)(offset / 0x4000);
    *bank = bank_id_for_index(project->banks, bank_idx);
    *cpu_addr = 0x4000u + (uint32_t)(offset % 0x4000);
    return 1;
}

void save_session(const char *rp, const char *cp, const UiState *s, const ApexRenderedDocument *d)
{
    FILE *f = fopen(".apeximgui_session", "w");
    if (!f) {
        return;
    }
    fprintf(f, "[Global]\nlast_rom=%s\nlast_config=%s\n\n[%s]\n", rp, cp, rp);
    uint8_t b;
    uint32_t a;
    if (selected_address(d, s, &b, &a)) {
        fprintf(f, "selected=0x%02x:%04x\n", b, a);
    }
    fprintf(f, "filter=%s\nlabel_filter=%s\ndmd_scrub=%d\ngraph_depth_in=%d\ngraph_depth_out=%d\nhistory=",
            s->filter_input, s->label_filter_input, s->dmd_scrub_offset,
            s->graph_depth_in, s->graph_depth_out);
    for (size_t i = 0; i < s->history_back.size(); i++) {
        if (s->history_back[i] < d->line_count && d->lines[s->history_back[i]].has_location) {
            fprintf(f, "0x%02x:%04x,", d->lines[s->history_back[i]].bank,
                    (unsigned)d->lines[s->history_back[i]].cpu_addr);
        }
    }
    fprintf(f, "\n");
    for (auto &bm : s->bookmarks) {
        fprintf(f, "bookmark=0x%02x:%04x:%s\n", bm.bank, bm.addr, bm.name.c_str());
    }
    fprintf(f,
            "show_navigator=%d\nshow_disasm=%d\nshow_labels=%d\nshow_banks=%d\n"
            "show_bookmarks=%d\nshow_transitions=%d\nshow_details=%d\nshow_refs=%d\n"
            "show_dmd=%d\nshow_edit=%d\nshow_hex=%d\nshow_call_graph=%d\n"
            "show_hardware=%d\nshow_tables=%d\nshow_types=%d\nshow_inline_list=%d\n"
            "show_entries_list=%d\nshow_pattern_search=%d\nshow_ram_refs=%d\n"
            "show_ref_exclusions=%d\nshow_search_window=%d\nshow_rom_map=%d\n"
            "show_dmd_list=%d\nshow_sprite_list=%d\n"
            "show_flow_arrows=%d\nshow_symbols=%d\n"
            "show_code_candidates=%d\nshow_inline_candidates=%d\n"
            "show_strings_list=%d\nshow_sprite_gallery=%d\n"
            "show_rom_info=%d\nshow_match_window=%d\nshow_rom_compare=%d\n"
            "show_coverage=%d\n",
            s->show_navigator, s->show_disasm, s->show_labels, s->show_banks,
            s->show_bookmarks, s->show_transitions, s->show_details, s->show_refs,
            s->show_dmd, s->show_edit, s->show_hex, s->show_call_graph,
            s->show_hardware, s->show_tables, s->show_types_editor, s->show_inline_list,
            s->show_entries_list, s->show_pattern_search, s->show_ram_refs,
            s->show_ref_exclusions, s->show_search_window, s->show_rom_map,
            s->show_dmd_list, s->show_sprite_list,
            s->show_flow_arrows, s->show_symbols_editor,
            s->show_code_candidates, s->show_inline_candidates,
            s->show_strings_list, s->show_sprite_gallery,
            s->show_rom_info, s->show_match_window, s->show_rom_compare,
            s->show_coverage);
    fclose(f);
}

int load_global_session(char *rp, char *cp)
{
    FILE *f = fopen(".apeximgui_session", "r");
    char l[1024];
    int fo = 0;
    if (!f) {
        return 0;
    }
    while (fgets(l, 1024, f)) {
        if (strncmp(l, "last_rom=", 9) == 0) {
            strcpy(rp, l + 9);
            rp[strcspn(rp, "\r\n")] = 0;
            fo++;
        } else if (strncmp(l, "last_config=", 12) == 0) {
            strcpy(cp, l + 12);
            cp[strcspn(cp, "\r\n")] = 0;
            fo++;
        }
        if (fo == 2) {
            break;
        }
    }
    fclose(f);
    return fo == 2;
}

void load_rom_session(const char *rp, UiState *s, const ApexRenderedDocument *d)
{
    FILE *f = fopen(".apeximgui_session", "r");
    char l[1024], sec[1024];
    int in = 0;
    if (!f) {
        return;
    }
    snprintf(sec, 1024, "[%s]", rp);
    while (fgets(l, 1024, f)) {
        l[strcspn(l, "\r\n")] = 0;
        if (l[0] == '[') {
            in = (strcmp(l, sec) == 0);
            continue;
        }
        if (!in) {
            continue;
        }
        if (strncmp(l, "selected=", 9) == 0) {
            unsigned b, a;
            size_t li;
            if (sscanf(l + 9, "0x%x:%x", &b, &a) == 2 &&
                apex_render_find_line_by_address(d, (uint8_t)b, a, &li)) {
                s->selected_line = li;
                s->request_scroll_to_selection = 1;
            }
        } else if (strncmp(l, "filter=", 7) == 0) {
            strncpy(s->filter_input, l + 7, 127);
            s->filter_input[127] = '\0';
        } else if (strncmp(l, "label_filter=", 13) == 0) {
            strncpy(s->label_filter_input, l + 13, 127);
            s->label_filter_input[127] = '\0';
        } else if (strncmp(l, "dmd_scrub=", 10) == 0) {
            s->dmd_scrub_offset = atoi(l + 10);
        } else if (strncmp(l, "graph_depth_in=", 15) == 0) {
            s->graph_depth_in = atoi(l + 15);
        } else if (strncmp(l, "graph_depth_out=", 16) == 0) {
            s->graph_depth_out = atoi(l + 16);
        } else if (strncmp(l, "history=", 8) == 0) {
            char *p = l + 8;
            unsigned b, a;
            size_t li;
            s->history_back.clear();
            while (sscanf(p, "0x%x:%x", &b, &a) == 2) {
                if (apex_render_find_line_by_address(d, (uint8_t)b, a, &li)) {
                    s->history_back.push_back(li);
                }
                p = strchr(p, ',');
                if (!p) {
                    break;
                }
                p++;
            }
        } else if (strncmp(l, "bookmark=", 9) == 0) {
            unsigned b, a;
            char n[256];
            if (sscanf(l + 9, "0x%x:%x:%255[^\r\n]", &b, &a, n) == 3) {
                s->bookmarks.push_back({(uint8_t)b, (uint32_t)a, n});
            }
        } else if (strncmp(l, "show_navigator=", 15) == 0) {
            s->show_navigator = atoi(l + 15) != 0;
        } else if (strncmp(l, "show_disasm=", 12) == 0) {
            s->show_disasm = atoi(l + 12) != 0;
        } else if (strncmp(l, "show_labels=", 12) == 0) {
            s->show_labels = atoi(l + 12) != 0;
        } else if (strncmp(l, "show_banks=", 11) == 0) {
            s->show_banks = atoi(l + 11) != 0;
        } else if (strncmp(l, "show_bookmarks=", 15) == 0) {
            s->show_bookmarks = atoi(l + 15) != 0;
        } else if (strncmp(l, "show_transitions=", 17) == 0) {
            s->show_transitions = atoi(l + 17) != 0;
        } else if (strncmp(l, "show_details=", 13) == 0) {
            s->show_details = atoi(l + 13) != 0;
        } else if (strncmp(l, "show_refs=", 10) == 0) {
            s->show_refs = atoi(l + 10) != 0;
        } else if (strncmp(l, "show_dmd=", 9) == 0) {
            s->show_dmd = atoi(l + 9) != 0;
        } else if (strncmp(l, "show_edit=", 10) == 0) {
            s->show_edit = atoi(l + 10) != 0;
        } else if (strncmp(l, "show_hex=", 9) == 0) {
            s->show_hex = atoi(l + 9) != 0;
        } else if (strncmp(l, "show_call_graph=", 16) == 0) {
            s->show_call_graph = atoi(l + 16) != 0;
        } else if (strncmp(l, "show_hardware=", 14) == 0) {
            s->show_hardware = atoi(l + 14) != 0;
        } else if (strncmp(l, "show_tables=", 12) == 0) {
            s->show_tables = atoi(l + 12) != 0;
        } else if (strncmp(l, "show_types=", 11) == 0) {
            s->show_types_editor = atoi(l + 11) != 0;
        } else if (strncmp(l, "show_inline_list=", 17) == 0) {
            s->show_inline_list = atoi(l + 17) != 0;
        } else if (strncmp(l, "show_entries_list=", 18) == 0) {
            s->show_entries_list = atoi(l + 18) != 0;
        } else if (strncmp(l, "show_strings_list=", 18) == 0) {
            s->show_strings_list = atoi(l + 18) != 0;
        } else if (strncmp(l, "show_sprite_gallery=", 20) == 0) {
            s->show_sprite_gallery = atoi(l + 20) != 0;
        } else if (strncmp(l, "show_rom_info=", 14) == 0) {
            s->show_rom_info = atoi(l + 14) != 0;
        } else if (strncmp(l, "show_match_window=", 18) == 0) {
            s->show_match_window = atoi(l + 18) != 0;
        } else if (strncmp(l, "show_rom_compare=", 17) == 0) {
            s->show_rom_compare = atoi(l + 17) != 0;
        } else if (strncmp(l, "show_coverage=", 14) == 0) {
            s->show_coverage = atoi(l + 14) != 0;
        } else if (strncmp(l, "show_pattern_search=", 20) == 0) {
            s->show_pattern_search = atoi(l + 20) != 0;
        } else if (strncmp(l, "show_ram_refs=", 14) == 0) {
            s->show_ram_refs = atoi(l + 14) != 0;
        } else if (strncmp(l, "show_ref_exclusions=", 20) == 0) {
            s->show_ref_exclusions = atoi(l + 20) != 0;
        } else if (strncmp(l, "show_search_window=", 19) == 0) {
            s->show_search_window = atoi(l + 19) != 0;
        } else if (strncmp(l, "show_rom_map=", 13) == 0) {
            s->show_rom_map = atoi(l + 13) != 0;
        } else if (strncmp(l, "show_dmd_list=", 14) == 0) {
            s->show_dmd_list = atoi(l + 14) != 0;
        } else if (strncmp(l, "show_sprite_list=", 17) == 0) {
            s->show_sprite_list = atoi(l + 17) != 0;
        } else if (strncmp(l, "show_flow_arrows=", 17) == 0) {
            s->show_flow_arrows = atoi(l + 17) != 0;
        } else if (strncmp(l, "show_symbols=", 13) == 0) {
            s->show_symbols_editor = atoi(l + 13) != 0;
        } else if (strncmp(l, "show_code_candidates=", 21) == 0) {
            s->show_code_candidates = atoi(l + 21) != 0;
        } else if (strncmp(l, "show_inline_candidates=", 23) == 0) {
            s->show_inline_candidates = atoi(l + 23) != 0;
        }
    }
    fclose(f);
}

void clear_session()
{
    remove(".apeximgui_session");
}

// --- Table Search (from apextab.c) ---

static bool is_valid_string_internal(const uint8_t *rom, uint32_t size, uint32_t offset)
{
    if (offset >= size) {
        return false;
    }
    uint32_t len = 0;
    while (offset + len < size && len <= 50) {
        uint8_t c = rom[offset + len];
        if (c == 0x00) {
            return len > 0;
        }
        if (c < 0x20 || c > 0x7F) {
            return false;
        }
        len++;
    }
    return false;
}

static uint32_t translate_ptr16_internal(uint16_t ptr, int phys_bank, int total_banks)
{
    if (ptr >= 0x4000 && ptr <= 0x7FFF) {
        return (uint32_t)phys_bank * 0x4000 + (ptr - 0x4000);
    }
    if (ptr >= 0x8000) {
        int sys_st = total_banks - 2;
        if (sys_st < 0) {
            sys_st = 0;
        }
        if (ptr < 0xC000) {
            return (uint32_t)sys_st * 0x4000 + (ptr - 0x8000);
        }
        return (uint32_t)(sys_st + 1) * 0x4000 + (ptr - 0xC000);
    }
    return 0xFFFFFFFF;
}

static bool is_text_table_at_internal(const uint8_t *rom, uint32_t size, uint32_t offset,
                                      int total_banks)
{
    if (offset + 3 > size) {
        return false;
    }
    uint16_t rows = (rom[offset] << 8) | rom[offset + 1];
    uint8_t width = rom[offset + 2];
    if (width != 2 || rows == 0 || rows > 1000) {
        return false;
    }
    uint32_t data_off = offset + 3;
    if (data_off + rows * 2 > size) {
        return false;
    }
    int phys_bank = offset / 0x4000;
    for (int i = 0; i < rows; i++) {
        uint16_t ptr = (rom[data_off + i * 2] << 8) | rom[data_off + i * 2 + 1];
        uint32_t str_off = translate_ptr16_internal(ptr, phys_bank, total_banks);
        if (str_off == 0xFFFFFFFF || !is_valid_string_internal(rom, size, str_off)) {
            return false;
        }
    }
    return true;
}

/* Resolve a 3-byte far pointer (CPU addr + bank byte) to a ROM file offset,
   mirroring the resolution used for far_data tables above. */
static uint32_t resolve_far_off_internal(const ApexProject *p, uint16_t addr, uint8_t bid,
                                         int total_banks)
{
    for (int b = 0; b < total_banks; b++) {
        if (p->rom.data[b * 0x4000] == bid && addr >= 0x4000 && addr <= 0x7FFF) {
            return (uint32_t)b * 0x4000 + (addr - 0x4000);
        }
    }
    if (addr >= 0x8000 && bid == 0xFF) {
        return translate_ptr16_internal(addr, 0, total_banks);
    }
    return 0xFFFFFFFFu;
}

/* True if the 3-byte far pointer at ROM offset `off` targets a decodable DMD
   full-frame.  Used to discover far_dmd pointer tables. */
static bool far_ptr_is_dmd_internal(const ApexProject *p, uint32_t off, int total_banks)
{
    if ((size_t)off + 3u > p->rom.size) return false;
    uint16_t addr = (uint16_t)((p->rom.data[off] << 8) | p->rom.data[off + 1]);
    uint8_t  bid  = p->rom.data[off + 2];
    uint32_t toff = resolve_far_off_internal(p, addr, bid, total_banks);
    if (toff == 0xFFFFFFFFu || (size_t)toff >= p->rom.size) return false;
    uint8_t plane[APEX_DMD_PAGE_BYTES];
    size_t consumed = 0;
    uint8_t type = 0;
    return apexdmd_decode_fullframe(p->rom.data + toff, p->rom.size - (size_t)toff,
                                    plane, &consumed, &type) != 0;
}

/* Given a ROM offset, compute the (bank_id, cpu_addr) the disassembler uses. */
static void off_to_bank_addr_internal(const ApexProject *p, uint32_t off, int total_banks,
                                      uint8_t *bank_id, uint16_t *addr)
{
    int phys_bank = (int)(off / 0x4000);
    if (phys_bank >= total_banks - 2) {
        *bank_id = 0xFF;
        *addr = (uint16_t)((phys_bank == total_banks - 2) ? 0x8000 + (off % 0x4000)
                                                          : 0xC000 + (off % 0x4000));
    } else {
        *bank_id = p->rom.data[phys_bank * 0x4000];
        *addr = (uint16_t)(0x4000 + (off % 0x4000));
    }
}

void auto_search_tables(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    int total_banks = (int)(p->rom.size / 0x4000);
    std::vector<uint32_t> found_text_table_offsets;
    int count = 0;

    // Step 1: Text Tables
    for (uint32_t i = 0; i <= p->rom.size - 3; i++) {
        if (is_text_table_at_internal(p->rom.data, (uint32_t)p->rom.size, i, total_banks)) {
            int phys_bank = i / 0x4000;
            uint8_t bank_id = (phys_bank >= total_banks - 2) ? 0xFF : p->rom.data[phys_bank * 0x4000];
            uint16_t addr = 0x4000 + (i % 0x4000);
            if (phys_bank >= total_banks - 2) {
                addr = (phys_bank == total_banks - 2) ? 0x8000 + (i % 0x4000)
                                                       : 0xC000 + (i % 0x4000);
            }
            if (apex_project_set_kind(p, 0, bank_id, addr, APEX_KIND_TABLE,
                                      "counted(ptr16_string)") == 0) {
                found_text_table_offsets.push_back(i);
                count++;
            }
        }
    }

    // Step 2: Far-Pointer Tables (simplified: look for 3 pointers to text tables)
    for (uint32_t i = 0; i <= p->rom.size - 9; i++) {
        bool match = true;
        for (int j = 0; j < 3; j++) {
            uint16_t addr = (p->rom.data[i + j * 3] << 8) | p->rom.data[i + j * 3 + 1];
            uint8_t bid = p->rom.data[i + j * 3 + 2];
            uint32_t target_off = 0xFFFFFFFF;
            for (int b = 0; b < total_banks; b++) {
                if (p->rom.data[b * 0x4000] == bid) {
                    if (addr >= 0x4000 && addr <= 0x7FFF) {
                        target_off = (uint32_t)b * 0x4000 + (addr - 0x4000);
                        break;
                    }
                }
            }
            if (addr >= 0x8000 && bid == 0xFF) {
                target_off = translate_ptr16_internal(addr, 0, total_banks);
            }
            bool known = false;
            if (target_off != 0xFFFFFFFF) {
                for (auto to : found_text_table_offsets) {
                    if (to == target_off) {
                        known = true;
                        break;
                    }
                }
            }
            if (!known) {
                match = false;
                break;
            }
        }
        if (match) {
            int phys_bank = i / 0x4000;
            uint8_t bank_id = (phys_bank >= total_banks - 2) ? 0xFF : p->rom.data[phys_bank * 0x4000];
            uint16_t addr = 0x4000 + (i % 0x4000);
            if (phys_bank >= total_banks - 2) {
                addr = (phys_bank == total_banks - 2) ? 0x8000 + (i % 0x4000)
                                                       : 0xC000 + (i % 0x4000);
            }
            if (apex_project_set_kind(p, 0, bank_id, addr, APEX_KIND_TABLE,
                                      "rows[3](far_data)") == 0) {
                count++;
                i += 8;
            }
        }
    }

    // Step 3: DMD tables — lists of far pointers to DMD full-frames.
    //   (a) counted(far_dmd_fullframe): DW count, DB width=3, then `count` rows.
    //   (b) rows[N](far_dmd_fullframe): a headerless run of far_dmd pointers (the
    //       common case).  Requiring several consecutive decodable targets keeps
    //       random byte sequences from matching.
    static const int kMinDmdRun = 3;
    if (p->rom.size >= 6) {
        for (uint32_t i = 0; i + 3u <= p->rom.size; ) {
            // (a) counted header: [count_hi count_lo width=3]
            uint16_t hdr_count = (uint16_t)((p->rom.data[i] << 8) | p->rom.data[i + 1]);
            uint8_t  hdr_width = p->rom.data[i + 2];
            if (hdr_width == 3u && hdr_count >= 2u &&
                (size_t)i + 3u + (size_t)hdr_count * 3u <= p->rom.size) {
                bool all_dmd = true;
                for (uint16_t r = 0; r < hdr_count && all_dmd; r++) {
                    if (!far_ptr_is_dmd_internal(p, i + 3u + (uint32_t)r * 3u, total_banks))
                        all_dmd = false;
                }
                if (all_dmd) {
                    uint8_t bank_id; uint16_t addr;
                    off_to_bank_addr_internal(p, i, total_banks, &bank_id, &addr);
                    if (apex_project_set_kind(p, 0, bank_id, addr, APEX_KIND_TABLE,
                                              "counted(far_dmd_fullframe)") == 0) {
                        count++;
                    }
                    i += 3u + (uint32_t)hdr_count * 3u;
                    continue;
                }
            }
            // (b) headerless run of far_dmd pointers
            int n = 0;
            uint32_t k = i;
            while (k + 3u <= p->rom.size && far_ptr_is_dmd_internal(p, k, total_banks)) {
                n++;
                k += 3u;
            }
            if (n >= kMinDmdRun) {
                uint8_t bank_id; uint16_t addr;
                off_to_bank_addr_internal(p, i, total_banks, &bank_id, &addr);
                char spec[40];
                snprintf(spec, sizeof(spec), "rows[%d](far_dmd_fullframe)", n);
                if (apex_project_set_kind(p, 0, bank_id, addr, APEX_KIND_TABLE, spec) == 0) {
                    count++;
                }
                i = k;
            } else {
                i++;
            }
        }
    }

    if (count > 0) {
        rerender_and_reselect(p, dp, s, 0, 0);
        char msg[64];
        snprintf(msg, 64, "Found %d tables", count);
        set_status(s, msg);
    } else {
        set_status(s, "No new tables found");
    }
}

std::vector<HardwareAccess> find_hardware_accesses(const ApexProject *project,
                                                    const ApexRenderedDocument *document)
{
    std::vector<HardwareAccess> accesses;
    std::map<uint32_t, std::vector<size_t>> mapping;

    // 1. Scan analyzer references (fast, covers explicit absolute accesses)
    for (size_t i = 0; i < project->refs.count; i++) {
        const Reference &ref = project->refs.items[i];
        if (ref.addr >= 0x3F00 && ref.addr <= 0x3FFF) {
            size_t lidx;
            if (apex_render_find_line_by_address(document, ref.source_bank, ref.source_addr, &lidx)) {
                bool exists = false;
                for (size_t ex : mapping[ref.addr]) {
                    if (ex == lidx) {
                        exists = true;
                    }
                }
                if (!exists) {
                    mapping[ref.addr].push_back(lidx);
                }
            }
        }
    }

    // 2. Heuristic: Deep Instruction Scan
    // Scan all identified instructions for any mention of $3Fxx in their bytes.
    // This catches cases where the instruction wasn't correctly linked as a reference.
    for (size_t i = 0; i < document->line_count; i++) {
        const auto *line = &document->lines[i];
        if (line->kind != APEX_RENDER_LINE_INSTRUCTION || !line->has_location) {
            continue;
        }

        // We only need to check the immediate/operand bytes of the instruction.
        // A simple way is to check every possible 16-bit big-endian word in the first 5 bytes.
        const uint8_t *rom_ptr;
        size_t rom_len;
        if (project_locate_rom_bytes(project, line->bank, line->cpu_addr, &rom_ptr, &rom_len, NULL)) {
            size_t scan_len = (rom_len < 5) ? rom_len : 5;
            for (size_t b = 0; b + 1 < scan_len; b++) {
                uint16_t word = (uint16_t)((rom_ptr[b] << 8) | rom_ptr[b+1]);
                if (word >= 0x3F00 && word <= 0x3FFF) {
                    bool exists = false;
                    for (size_t ex : mapping[word]) {
                        if (ex == i) {
                            exists = true;
                        }
                    }
                    if (!exists) {
                        mapping[word].push_back(i);
                    }
                }
            }
        }
    }

    size_t reg_count = hardware_register_count();
    for (size_t i = 0; i < reg_count; i++) {
        const auto *reg = get_hardware_register(i);
        std::sort(mapping[reg->addr].begin(), mapping[reg->addr].end());
        HardwareAccess acc = { reg, mapping[reg->addr] };
        accesses.push_back(acc);
    }

    return accesses;
}

void copy_selection_to_clipboard(const ApexRenderedDocument *d, const UiState *s)
{
    size_t st = std::min(s->selected_line, s->selection_end);
    size_t en = std::max(s->selected_line, s->selection_end);
    std::string t;
    if (st >= d->line_count) {
        return;
    }
    if (en >= d->line_count) {
        en = d->line_count - 1;
    }
    for (size_t i = st; i <= en; i++) {
        const auto *l = &d->lines[i];
        if (l->has_location) {
            char buf[32];
            snprintf(buf, 32, "B%02x_A%04x  ", l->bank, (unsigned)l->cpu_addr & 0xffff);
            t += buf;
        } else {
            t += "             ";
        }
        t.append(l->text, l->length);
        t += "\n";
    }
    ImGui::SetClipboardText(t.c_str());
}

int address_is_dmd_fullframe_start(const ApexProject *p, uint8_t b, uint32_t a)
{
    if (data_range_at(b, a, &p->data_ranges) &&
        (data_range_at(b, a, &p->data_ranges)->kind == DATA_DMD_FULLFRAME ||
         data_range_at(b, a, &p->data_ranges)->kind == DATA_FAR_DMD_FULLFRAME)) {
        return 1;
    }
    const auto *ls = (b == 0xffu) ? &p->system_labels : NULL;
    if (!ls) {
        int bi = bank_index_for_far_ref(p->rom.data, p->banks, b);
        if (bi >= 0) {
            ls = &p->bank_labels[bi];
        }
    }
    if (!ls) {
        return 0;
    }
    for (size_t i = 0; i < ls->count; i++) {
        if (ls->items[i].addr == a &&
            ((ls->items[i].kind_explain && strstr(ls->items[i].kind_explain, "dmd_fullframe")) ||
             (ls->items[i].explain && strstr(ls->items[i].explain, "dmd_fullframe")))) {
            return 1;
        }
    }
    return 0;
}

int decode_dmd_preview_at(const ApexProject *p, uint8_t b, uint32_t a, DmdPreviewInfo *pr)
{
    const uint8_t *src;
    size_t len, con, ro;
    uint8_t type;
    if (!project_locate_rom_bytes(p, b, a, &src, &len, &ro)) {
        return 0;
    }
    if (!apexdmd_decode_fullframe(src, len, pr->plane, &con, &type)) {
        return 0;
    }
    pr->valid = true;
    pr->bank = b;
    pr->cpu_addr = a;
    pr->rom_offset = ro;
    pr->decoder_type = type;
    pr->consumed = con;
    return 1;
}

DmdPreviewInfo find_dmd_preview(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    DmdPreviewInfo pr = {};
    uint8_t b;
    uint32_t a;
    if (!selected_address(d, s, &b, &a)) {
        return pr;
    }
    if (s->dmd_scrub_offset != 0) {
        int64_t sa = (int64_t)a + s->dmd_scrub_offset;
        if (sa >= 0 && sa <= 0xffff && decode_dmd_preview_at(p, b, (uint32_t)sa, &pr)) {
            snprintf(pr.title, 128, "Scrubbed DMD (Offset %d)", s->dmd_scrub_offset);
            return pr;
        }
    }
    if (decode_dmd_preview_at(p, b, a, &pr)) {
        snprintf(pr.title, 128, "Selected DMD");
        return pr;
    }
    if (s->selected_line < d->line_count) {
        auto ts = find_line_targets(d, s, &d->lines[s->selected_line]);
        for (auto &t : ts) {
            if (address_is_dmd_fullframe_start(p, t.bank, t.cpu_addr) &&
                decode_dmd_preview_at(p, t.bank, t.cpu_addr, &pr)) {
                pr.from_target = true;
                snprintf(pr.title, 128, "Target DMD: %s", t.name.c_str());
                return pr;
            }
        }
    }
    return pr;
}

int address_is_sprite_start(const ApexProject *p, uint8_t b, uint32_t a)
{
    if (data_range_at(b, a, &p->data_ranges) &&
        (data_range_at(b, a, &p->data_ranges)->kind == DATA_SPRITE ||
         data_range_at(b, a, &p->data_ranges)->kind == DATA_FAR_SPRITE ||
         data_range_at(b, a, &p->data_ranges)->kind == DATA_SPRITE_NOHEADER)) {
        return 1;
    }
    const auto *ls = (b == 0xffu) ? &p->system_labels : NULL;
    if (!ls) {
        int bi = bank_index_for_far_ref(p->rom.data, p->banks, b);
        if (bi >= 0) {
            ls = &p->bank_labels[bi];
        }
    }
    if (!ls) {
        return 0;
    }
    for (size_t i = 0; i < ls->count; i++) {
        if (ls->items[i].addr == a &&
            ((ls->items[i].kind_explain && strstr(ls->items[i].kind_explain, "sprite")) ||
             (ls->items[i].explain && strstr(ls->items[i].explain, "sprite")))) {
            return 1;
        }
    }
    return 0;
}

int decode_sprite_preview_at(const ApexProject *p, uint8_t b, uint32_t a, SpritePreviewInfo *pr)
{
    const uint8_t *src;
    size_t len, ro;
    uint8_t hdr = 0, vert = 0, horiz = 0, width = 0, height = 0, enc_type = 0;
    size_t consumed = 0;

    if (!project_locate_rom_bytes(p, b, a, &src, &len, &ro)) {
        return 0;
    }
    pr->two_plane = false;

    /* Check if this is a classified no-header sprite */
    const DataRange *dr = data_range_at(b, a, &p->data_ranges);
    if (dr && dr->kind == DATA_SPRITE_NOHEADER && dr->length > 0) {
        uint8_t nh_height = (uint8_t)dr->length;
        if (!apexsprite_decode_noheader(src, len, pr->pixels, nh_height, &width, &consumed)) {
            return 0;
        }
        pr->valid        = true;
        pr->bank         = b;
        pr->cpu_addr     = a;
        pr->rom_offset   = ro;
        pr->header_type  = 0;
        pr->enc_type     = 0;
        pr->consumed     = consumed;
        pr->vert_offset  = 0;
        pr->horiz_offset = 0;
        pr->width        = width;
        pr->height       = nh_height;
        return 1;
    }

    if (!apexsprite_decode(src, len, pr->pixels, &hdr, &vert, &horiz, &width, &height, &enc_type, &consumed)) {
        return 0;
    }
    pr->valid       = true;
    pr->bank        = b;
    pr->cpu_addr    = a;
    pr->rom_offset  = ro;
    pr->header_type = hdr;
    pr->enc_type    = enc_type;
    pr->consumed    = consumed;
    pr->vert_offset  = vert;
    pr->horiz_offset = horiz;
    pr->width       = width;
    pr->height      = height;

    /* Bicolor: fetch plane 1 so the preview can show 4 grey levels. */
    if (enc_type == APEX_SPRITE_ENC_BICOLOR_DIRECT ||
        enc_type == APEX_SPRITE_ENC_BICOLOR_INDIRECT) {
        uint8_t rb = (uint8_t)((width + 7u) / 8u);
        size_t page = (size_t)rb * (size_t)height;
        memset(pr->pixels1, 0, sizeof(pr->pixels1));
        if (enc_type == APEX_SPRITE_ENC_BICOLOR_DIRECT) {
            if (len >= 5u + page) {
                memcpy(pr->pixels1, src + 5u, page); /* plane 1 inline before plane 0 */
                pr->two_plane = true;
            }
        } else if (len >= 7u) {
            uint32_t p1cpu  = ((uint32_t)src[5] << 8) | src[6];
            uint8_t  p1bank = (p1cpu >= 0x8000u) ? 0xffu : b;
            const uint8_t *p1; size_t p1len, p1ro;
            if (project_locate_rom_bytes(p, p1bank, (uint32_t)p1cpu, &p1, &p1len, &p1ro) &&
                p1len >= page) {
                memcpy(pr->pixels1, p1, page);
                pr->two_plane = true;
            }
        }
    }
    return 1;
}

/* Like decode_sprite_preview_at, but for a target that is not classified yet:
   header-format images self-describe and decode directly; a no-header image
   (first byte = width) needs the height, which is supplied by the caller from
   the sprite table field.  Used by the table-row tooltip preview. */
int decode_sprite_preview_with_height(const ApexProject *p, uint8_t b, uint32_t a,
                                      unsigned hint_height, SpritePreviewInfo *pr)
{
    const uint8_t *src;
    size_t len, ro;
    uint8_t b0;

    /* If it is already classified (or a header sprite), reuse the full decoder. */
    if (decode_sprite_preview_at(p, b, a, pr)) {
        return 1;
    }
    if (hint_height == 0u || !project_locate_rom_bytes(p, b, a, &src, &len, &ro)) {
        return 0;
    }
    b0 = src[0];
    /* A header byte means it is not a no-header image; nothing more to try. */
    if (b0 == 0x00u || b0 == 0xFDu || b0 == 0xFEu || b0 == 0xFFu || b0 == 0u || b0 > 128u) {
        return 0;
    }
    {
        uint8_t width = 0;
        size_t consumed = 0;
        if (!apexsprite_decode_noheader(src, len, pr->pixels, (uint8_t)hint_height,
                                        &width, &consumed)) {
            return 0;
        }
        pr->two_plane    = false;
        pr->valid        = true;
        pr->bank         = b;
        pr->cpu_addr     = a;
        pr->rom_offset   = ro;
        pr->header_type  = 0;
        pr->enc_type     = 0;
        pr->consumed     = consumed;
        pr->vert_offset  = 0;
        pr->horiz_offset = 0;
        pr->width        = width;
        pr->height       = (uint8_t)hint_height;
    }
    return 1;
}

SpritePreviewInfo find_sprite_preview(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    SpritePreviewInfo pr = {};
    uint8_t b;
    uint32_t a;
    if (!selected_address(d, s, &b, &a)) {
        return pr;
    }
    if (decode_sprite_preview_at(p, b, a, &pr)) {
        snprintf(pr.title, sizeof(pr.title), "Selected Sprite");
        return pr;
    }
    if (s->selected_line < d->line_count) {
        auto ts = find_line_targets(d, s, &d->lines[s->selected_line]);
        for (auto &t : ts) {
            if (address_is_sprite_start(p, t.bank, t.cpu_addr) &&
                decode_sprite_preview_at(p, t.bank, t.cpu_addr, &pr)) {
                pr.from_target = true;
                snprintf(pr.title, sizeof(pr.title), "Target Sprite: %s", t.name.c_str());
                return pr;
            }
        }
    }
    return pr;
}

int parse_target_address(const char *i, uint8_t *b, uint32_t *a)
{
    unsigned pb, pa;
    while (*i == ' ' || *i == '\t') {
        i++;
    }
    if (sscanf(i, "B%x_A%x", &pb, &pa) == 2) {
        *b = (uint8_t)pb;
        *a = pa;
        return 1;
    }
    if (strncasecmp(i, "0x", 2) == 0 && sscanf(i + 2, "%x", &pa) == 1) {
        *b = 0xffu;
        *a = pa;
        return 1;
    }
    return 0;
}

/* Wildcard match: * = any sequence, ? = any single char, case-insensitive.
   Matches pattern against the entire string (not substring). */
static int wildcard_match_ci(const char *pat, const char *text)
{
    const char *star = NULL;
    const char *mark = text;
    while (*text) {
        if (*pat == '*') {
            star = pat++; mark = text;
        } else if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*text)) {
            pat++; text++;
        } else if (star) {
            pat = star + 1; text = ++mark;
        } else {
            return 0;
        }
    }
    while (*pat == '*') pat++;
    return !*pat;
}

/* Returns true if the line text contains a substring matching pat
   (wildcards: * = any chars, ? = any single char, case-insensitive). */
static int line_matches_wildcard(const ApexRenderedLine *l, const char *pat)
{
    /* Build null-terminated line text */
    char buf[512];
    size_t len = l->length < sizeof(buf) - 1 ? l->length : sizeof(buf) - 1;
    memcpy(buf, l->text, len);
    buf[len] = '\0';

    /* Wrap pattern with implicit * on each side for substring semantics */
    char wrapped[512];
    snprintf(wrapped, sizeof(wrapped), "*%s*", pat);
    return wildcard_match_ci(wrapped, buf);
}

int line_matches_filter(const ApexRenderedLine *l, const char *f)
{
    if (!f || !*f) {
        return 1;
    }

    /* Wildcard mode when pattern contains * or ? */
    if (strchr(f, '*') || strchr(f, '?')) {
        return line_matches_wildcard(l, f);
    }

    /* Plain substring match (case-insensitive) */
    size_t fl = strlen(f);
    if (l->length < fl) {
        return 0;
    }
    for (size_t i = 0; i + fl <= l->length; i++) {
        size_t j;
        for (j = 0; j < fl; j++) {
            if (tolower((unsigned char)l->text[i+j]) != tolower((unsigned char)f[j])) {
                break;
            }
        }
        if (j == fl) {
            return 1;
        }
    }
    return 0;
}

/* Sequence search: split query on literal \n, match each part against
   consecutive instruction lines (skipping labels/blanks between them).
   Pushes the line index of the first instruction in each match into results. */
static void run_sequence_search(const ApexRenderedDocument *d,
                                 const char *const parts[], int nparts,
                                 std::vector<size_t> &results)
{
    for (size_t i = 0; i < d->line_count; i++) {
        if (d->lines[i].kind != APEX_RENDER_LINE_INSTRUCTION) continue;

        size_t cur = i;
        int p;
        for (p = 0; p < nparts; p++) {
            /* advance to next instruction line (skip labels/blanks) */
            while (cur < d->line_count &&
                   d->lines[cur].kind != APEX_RENDER_LINE_INSTRUCTION)
                cur++;
            if (cur >= d->line_count) break;
            if (!line_matches_filter(&d->lines[cur], parts[p])) break;
            cur++;
        }
        if (p == nparts)
            results.push_back(i);
    }
}

/* Top-level search: handles plain, wildcard, and multi-line (\n-separated)
   patterns.  Clears and fills results. */
void run_global_search(const ApexRenderedDocument *d,
                       const char *query,
                       std::vector<size_t> &results)
{
    results.clear();
    if (!query || !*query) return;

    /* Split on literal \n (backslash + n) */
    /* Max 5 parts for sequence search */
    const int MAX_PARTS = 5;
    char parts_buf[MAX_PARTS][128];
    const char *parts[MAX_PARTS];
    int nparts = 0;

    const char *p = query;
    while (*p && nparts < MAX_PARTS) {
        const char *next = strstr(p, "\\n");
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len >= sizeof(parts_buf[0])) len = sizeof(parts_buf[0]) - 1;
        memcpy(parts_buf[nparts], p, len);
        parts_buf[nparts][len] = '\0';
        parts[nparts] = parts_buf[nparts];
        nparts++;
        if (!next) break;
        p = next + 2;
    }

    if (nparts <= 1) {
        /* Single-line: search all line types */
        for (size_t i = 0; i < d->line_count; i++) {
            if (line_matches_filter(&d->lines[i], query))
                results.push_back(i);
        }
    } else {
        /* Multi-line: instruction-only sequence search */
        run_sequence_search(d, parts, nparts, results);
    }
}

const char *block_name(ApexRenderedBlockKind k)
{
    switch (k) {
    case APEX_RENDER_BLOCK_CODE:         return "code";
    case APEX_RENDER_BLOCK_DATA:         return "data";
    case APEX_RENDER_BLOCK_SPRITE:       return "sprite";
    case APEX_RENDER_BLOCK_TABLE:        return "table";
    case APEX_RENDER_BLOCK_UNCLASSIFIED: return "?";
    case APEX_RENDER_BLOCK_FREE:         return "free";
    default:                             return "-";
    }
}

const char *transition_name(ApexRenderedTransitionKind k)
{
    switch (k) {
    case APEX_RENDER_TRANSITION_CODE_TO_DATA:           return "code_to_data";
    case APEX_RENDER_TRANSITION_DATA_TO_CODE:           return "data_to_code";
    case APEX_RENDER_TRANSITION_CODE_TO_TABLE:          return "code_to_table";
    case APEX_RENDER_TRANSITION_TABLE_TO_CODE:          return "table_to_code";
    case APEX_RENDER_TRANSITION_TABLE_TO_DATA:          return "table_to_data";
    case APEX_RENDER_TRANSITION_DATA_TO_TABLE:          return "data_to_table";
    case APEX_RENDER_TRANSITION_CODE_TO_UNCLASSIFIED:   return "code_to_unclassified";
    case APEX_RENDER_TRANSITION_UNCLASSIFIED_TO_CODE:   return "unclassified_to_code";
    case APEX_RENDER_TRANSITION_TABLE_TO_UNCLASSIFIED:  return "table_to_unclassified";
    case APEX_RENDER_TRANSITION_UNCLASSIFIED_TO_TABLE:  return "unclassified_to_table";
    case APEX_RENDER_TRANSITION_DATA_TO_UNCLASSIFIED:   return "data_to_unclassified";
    case APEX_RENDER_TRANSITION_UNCLASSIFIED_TO_DATA:   return "unclassified_to_data";
    default:                                            return "-";
    }
}

/* Canonical kind names in the same order as TableFieldKind. */
static const struct { int kind; const char *name; } kKindNames[] = {
    { TABLE_BYTE,              "byte"         },
    { TABLE_WORD,              "word"         },
    { TABLE_PTR16_STRING,      "ptr16_string" },
    { TABLE_PTR16_DATA,        "ptr16_data"   },
    { TABLE_PTR16_CODE,        "ptr16_code"   },
    { TABLE_PTR16_TABLE,       "ptr16_table"  },
    { TABLE_PTR16_DMD_FULLFRAME,"ptr16_dmd_fullframe" },
    { TABLE_PTR16_SPRITE,      "ptr16_sprite" },
    { TABLE_FAR_STRING,        "far_string"   },
    { TABLE_FAR_DATA,          "far_data"     },
    { TABLE_FAR_TABLE,         "far_table"    },
    { TABLE_FAR_CODE,          "far_code"     },
    { TABLE_FAR_DMD_FULLFRAME, "far_dmd_fullframe" },
    { TABLE_FAR_SPRITE,        "far_sprite"   },
};
static const int kKindCount = (int)(sizeof(kKindNames) / sizeof(kKindNames[0]));

static const char *kind_to_name(int kind)
{
    for (int i = 0; i < kKindCount; i++) {
        if (kKindNames[i].kind == kind) {
            return kKindNames[i].name;
        }
    }
    return "byte";
}

void fields_to_spec(char *buf, size_t cap, const ApexEditField *fields, int count)
{
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0 && pos + 2 < cap) {
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
        const char *name = (fields[i].kind < 0) ? fields[i].type_name
                                                 : kind_to_name(fields[i].kind);
        size_t nlen = strlen(name);
        if (pos + nlen >= cap) { break; }
        memcpy(buf + pos, name, nlen);
        pos += nlen;
        if (fields[i].param > 0 &&
            (fields[i].kind == TABLE_PTR16_SPRITE || fields[i].kind == TABLE_FAR_SPRITE)) {
            int written = snprintf(buf + pos, cap - pos, "(%d)", fields[i].param);
            if (written > 0) { pos += (size_t)written; }
        }
        if (fields[i].count > 1) {
            int written = snprintf(buf + pos, cap - pos, "[%d]", fields[i].count);
            if (written > 0) { pos += (size_t)written; }
        }
    }
    if (pos < cap) { buf[pos] = '\0'; }
}

void spec_to_fields(const char *spec, ApexEditField *fields, int *count, int max,
                    const ApexProject *p)
{
    *count = 0;
    if (!spec || !*spec) { return; }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *tok = buf;
    while (tok && *tok && *count < max) {
        char *comma = strchr(tok, ',');
        if (comma) { *comma = '\0'; }
        while (*tok == ' ') { tok++; }
        char *end = tok + strlen(tok);
        while (end > tok && end[-1] == ' ') { *--end = '\0'; }
        int cnt = 1;
        char *bracket = strchr(tok, '[');
        if (bracket) {
            char *close = strchr(bracket, ']');
            if (close) {
                *bracket = '\0';
                cnt = atoi(bracket + 1);
                if (cnt < 1) { cnt = 1; }
                end = bracket;
                while (end > tok && end[-1] == ' ') { *--end = '\0'; }
            }
        }
        int param = 0;
        char *paren = strchr(tok, '(');
        if (paren) {
            char *close = strchr(paren, ')');
            if (close) {
                *paren = '\0';
                param = atoi(paren + 1);
                if (param < 0) { param = 0; }
                end = paren;
                while (end > tok && end[-1] == ' ') { *--end = '\0'; }
            }
        }
        if (*tok) {
            ApexEditField f;
            f.count = cnt;
            f.param = param;
            f.kind = -1;
            f.type_name[0] = '\0';
            for (int i = 0; i < kKindCount; i++) {
                if (strcmp(kKindNames[i].name, tok) == 0) {
                    f.kind = kKindNames[i].kind;
                    break;
                }
            }
            if (f.kind < 0) {
                /* named type — verify it exists in config */
                if (p) {
                    for (size_t ti = 0; ti < p->config_types.count; ti++) {
                        if (strcmp(p->config_types.items[ti].name, tok) == 0) {
                            snprintf(f.type_name, sizeof(f.type_name), "%s", tok);
                            break;
                        }
                    }
                } else {
                    snprintf(f.type_name, sizeof(f.type_name), "%s", tok);
                }
            }
            if (f.kind >= 0 || f.type_name[0]) {
                fields[(*count)++] = f;
            }
        }
        tok = comma ? comma + 1 : NULL;
    }
}

void load_doc_editor_buffer(const ApexProject *p, UiState *s, uint8_t b, uint32_t a)
{
    const char *d = config_doc_at(&p->docs, b, a);
    s->edit_doc_input[0] = '\0';
    if (d) {
        snprintf(s->edit_doc_input, 1024, "%s", d);
    }
}

OriginalSnapshot build_original_snapshot(const ApexProject *p)
{
    OriginalSnapshot s;
    for (size_t i = 0; i < p->config_labels.count; i++) {
        s.labels.push_back({p->config_labels.items[i].has_bank,
                            p->config_labels.items[i].bank,
                            p->config_labels.items[i].addr,
                            p->config_labels.items[i].name});
    }
    for (size_t i = 0; i < p->config_entries.count; i++) {
        s.entries.push_back({p->config_entries.items[i].has_bank,
                             p->config_entries.items[i].bank,
                             p->config_entries.items[i].addr});
    }
    for (size_t i = 0; i < p->data_ranges.count; i++) {
        s.data.push_back({p->data_ranges.items[i].bank,
                          p->data_ranges.items[i].addr,
                          data_range_spec_string(&p->data_ranges.items[i])});
    }
    for (size_t i = 0; i < p->tables.count; i++) {
        s.tables.push_back({p->tables.items[i].bank,
                            p->tables.items[i].addr,
                            table_def_spec_string(&p->tables.items[i])});
    }
    for (size_t i = 0; i < p->docs.count; i++) {
        s.docs.push_back({p->docs.items[i].has_bank,
                          p->docs.items[i].bank,
                          p->docs.items[i].addr,
                          p->docs.items[i].text});
    }
    for (size_t i = 0; i < p->inline_sigs.count; i++) {
        s.inline_sigs.push_back({p->inline_sigs.items[i].has_bank,
                                 p->inline_sigs.items[i].bank,
                                 p->inline_sigs.items[i].addr,
                                 inline_sig_spec_string(&p->inline_sigs.items[i])});
    }
    for (size_t i = 0; i < p->ref_exclusions.count; i++) {
        s.ref_exclusions.push_back({p->ref_exclusions.items[i].has_bank,
                                    p->ref_exclusions.items[i].bank,
                                    p->ref_exclusions.items[i].addr});
    }
    for (size_t i = 0; i < p->literals.count; i++) {
        s.literals.push_back({p->literals.items[i].has_bank,
                              p->literals.items[i].bank,
                              p->literals.items[i].addr});
    }
    for (size_t i = 0; i < p->config_types.count; i++) {
        const ConfigType *ct = &p->config_types.items[i];
        SnapshotType st;
        st.name    = ct->name;
        st.is_word = (ct->kind == TABLE_WORD) ? 1 : 0;
        for (size_t j = 0; j < ct->value_count; j++) {
            st.values.push_back({ct->values[j].value, ct->values[j].name});
        }
        s.types.push_back(std::move(st));
    }
    for (size_t i = 0; i < p->symbols.count; i++) {
        s.symbols.push_back({p->symbols.items[i].name, p->symbols.items[i].value});
    }
    return s;
}

OriginalSnapshot build_config_snapshot(const char *config_path)
{
    OriginalSnapshot s;
    if (!config_path || !*config_path) {
        return s;
    }
    InlineSignatures sigs = {};
    ConfigLabels labels = {};
    ConfigEntries entries = {};
    TableDefs tables = {};
    SchemaDefs schemas = {};
    ConfigDocs docs = {};
    ConfigSymbols symbols = {};
    DataRanges data_ranges = {};
    ConfigOptions options = {};
    options.labels_are_entries = 0;
    ConfigTypes types = {};
    ConfigEntries ref_exclusions = {};
    ConfigEntries literals = {};
    load_config(config_path, &sigs, &labels, &entries, &tables, &schemas,
                &docs, &symbols, &data_ranges, &options, &types,
                &ref_exclusions, &literals);
    for (size_t i = 0; i < labels.count; i++) {
        s.labels.push_back({labels.items[i].has_bank,
                            labels.items[i].bank,
                            labels.items[i].addr,
                            labels.items[i].name});
    }
    for (size_t i = 0; i < entries.count; i++) {
        s.entries.push_back({entries.items[i].has_bank,
                             entries.items[i].bank,
                             entries.items[i].addr});
    }
    for (size_t i = 0; i < data_ranges.count; i++) {
        s.data.push_back({data_ranges.items[i].bank,
                          data_ranges.items[i].addr,
                          data_range_spec_string(&data_ranges.items[i])});
    }
    for (size_t i = 0; i < tables.count; i++) {
        s.tables.push_back({tables.items[i].bank,
                            tables.items[i].addr,
                            table_def_spec_string(&tables.items[i])});
    }
    for (size_t i = 0; i < docs.count; i++) {
        s.docs.push_back({docs.items[i].has_bank,
                          docs.items[i].bank,
                          docs.items[i].addr,
                          docs.items[i].text});
    }
    for (size_t i = 0; i < sigs.count; i++) {
        s.inline_sigs.push_back({sigs.items[i].has_bank,
                                 sigs.items[i].bank,
                                 sigs.items[i].addr,
                                 inline_sig_spec_string(&sigs.items[i])});
    }
    for (size_t i = 0; i < ref_exclusions.count; i++) {
        s.ref_exclusions.push_back({ref_exclusions.items[i].has_bank,
                                    ref_exclusions.items[i].bank,
                                    ref_exclusions.items[i].addr});
    }
    for (size_t i = 0; i < literals.count; i++) {
        s.literals.push_back({literals.items[i].has_bank,
                              literals.items[i].bank,
                              literals.items[i].addr});
    }
    for (size_t i = 0; i < types.count; i++) {
        const ConfigType *ct = &types.items[i];
        SnapshotType stype;
        stype.name    = ct->name;
        stype.is_word = (ct->kind == TABLE_WORD) ? 1 : 0;
        for (size_t j = 0; j < ct->value_count; j++) {
            stype.values.push_back({ct->values[j].value, ct->values[j].name});
        }
        s.types.push_back(std::move(stype));
    }
    free(ref_exclusions.items);
    free(literals.items);
    free_config_types(&types);
    return s;
}

int write_delta_overlay(const ApexProject *p, const OriginalSnapshot *s, const char *path,
                        const char *include_path, std::string *st)
{
    std::vector<SnapshotLabel> cl;
    std::vector<SnapshotEntry> ce;
    std::vector<SnapshotEntry> cexcl;
    std::vector<SnapshotEntry> clits;
    std::vector<SnapshotData> cd;
    std::vector<SnapshotTable> ct;
    std::vector<SnapshotDoc> cdocs;
    std::vector<SnapshotInline> ci;
    std::vector<SnapshotType> ctype;

    for (auto &i : s->labels) {
        int sp = 0;
        for (size_t j = 0; j < p->config_labels.count; j++) {
            if (p->config_labels.items[j].has_bank == i.has_bank &&
                p->config_labels.items[j].bank == i.bank &&
                p->config_labels.items[j].addr == i.addr) {
                sp = 1;
                break;
            }
        }
        if (!sp) {
            *st = "deletion needs full snapshot";
            return 0;
        }
    }
    for (size_t i = 0; i < p->config_labels.count; i++) {
        const auto *o = find_snapshot_label(s, p->config_labels.items[i].has_bank,
                                            p->config_labels.items[i].bank,
                                            p->config_labels.items[i].addr);
        if (!o || o->name != p->config_labels.items[i].name) {
            cl.push_back({p->config_labels.items[i].has_bank,
                          p->config_labels.items[i].bank,
                          p->config_labels.items[i].addr,
                          p->config_labels.items[i].name});
        }
    }
    for (size_t i = 0; i < p->config_entries.count; i++) {
        if (!find_snapshot_entry(s, p->config_entries.items[i].has_bank,
                                 p->config_entries.items[i].bank,
                                 p->config_entries.items[i].addr)) {
            ce.push_back({p->config_entries.items[i].has_bank,
                          p->config_entries.items[i].bank,
                          p->config_entries.items[i].addr});
        }
    }
    for (size_t i = 0; i < p->data_ranges.count; i++) {
        std::string spec = data_range_spec_string(&p->data_ranges.items[i]);
        const auto *o = find_snapshot_data(s, p->data_ranges.items[i].bank,
                                           p->data_ranges.items[i].addr);
        if (!o || o->spec != spec) {
            cd.push_back({p->data_ranges.items[i].bank, p->data_ranges.items[i].addr, spec});
        }
    }
    for (size_t i = 0; i < p->tables.count; i++) {
        std::string spec = table_def_spec_string(&p->tables.items[i]);
        const auto *o = find_snapshot_table(s, p->tables.items[i].bank, p->tables.items[i].addr);
        if (!o || o->spec != spec) {
            ct.push_back({p->tables.items[i].bank, p->tables.items[i].addr, spec});
        }
    }
    for (size_t i = 0; i < p->docs.count; i++) {
        const auto *o = find_snapshot_doc(s->docs, p->docs.items[i].has_bank,
                                          p->docs.items[i].bank,
                                          p->docs.items[i].addr);
        if (!o || o->text != p->docs.items[i].text) {
            cdocs.push_back({p->docs.items[i].has_bank,
                             p->docs.items[i].bank,
                             p->docs.items[i].addr,
                             p->docs.items[i].text});
        }
    }

    for (size_t i = 0; i < p->inline_sigs.count; i++) {
        std::string spec = inline_sig_spec_string(&p->inline_sigs.items[i]);
        int found = 0;
        for (auto &o : s->inline_sigs) {
            if (o.has_bank == p->inline_sigs.items[i].has_bank &&
                o.bank == p->inline_sigs.items[i].bank &&
                o.addr == p->inline_sigs.items[i].addr) {
                if (o.spec != spec) {
                    ci.push_back({o.has_bank, o.bank, o.addr, spec});
                }
                found = 1;
                break;
            }
        }
        if (!found) {
            ci.push_back({p->inline_sigs.items[i].has_bank,
                          p->inline_sigs.items[i].bank,
                          p->inline_sigs.items[i].addr,
                          spec});
        }
    }

    /* collect new ref exclusions; deletions require full snapshot */
    for (auto &o : s->ref_exclusions) {
        bool still_there = false;
        for (size_t j = 0; j < p->ref_exclusions.count; j++) {
            if (p->ref_exclusions.items[j].has_bank == o.has_bank &&
                p->ref_exclusions.items[j].bank == o.bank &&
                p->ref_exclusions.items[j].addr == o.addr) {
                still_there = true;
                break;
            }
        }
        if (!still_there) {
            *st = "deletion needs full snapshot";
            return 0;
        }
    }
    for (size_t i = 0; i < p->ref_exclusions.count; i++) {
        bool found = false;
        for (auto &o : s->ref_exclusions) {
            if (o.has_bank == p->ref_exclusions.items[i].has_bank &&
                o.bank == p->ref_exclusions.items[i].bank &&
                o.addr == p->ref_exclusions.items[i].addr) {
                found = true;
                break;
            }
        }
        if (!found) {
            cexcl.push_back({p->ref_exclusions.items[i].has_bank,
                             p->ref_exclusions.items[i].bank,
                             p->ref_exclusions.items[i].addr});
        }
    }

    /* collect new literals; deletions require full snapshot */
    for (auto &o : s->literals) {
        bool still_there = false;
        for (size_t j = 0; j < p->literals.count; j++) {
            if (p->literals.items[j].has_bank == o.has_bank &&
                p->literals.items[j].bank == o.bank &&
                p->literals.items[j].addr == o.addr) {
                still_there = true;
                break;
            }
        }
        if (!still_there) {
            *st = "deletion needs full snapshot";
            return 0;
        }
    }
    for (size_t i = 0; i < p->literals.count; i++) {
        bool found = false;
        for (auto &o : s->literals) {
            if (o.has_bank == p->literals.items[i].has_bank &&
                o.bank == p->literals.items[i].bank &&
                o.addr == p->literals.items[i].addr) {
                found = true;
                break;
            }
        }
        if (!found) {
            clits.push_back({p->literals.items[i].has_bank,
                             p->literals.items[i].bank,
                             p->literals.items[i].addr});
        }
    }

    /* collect new or changed types */
    for (size_t i = 0; i < p->config_types.count; i++) {
        const ConfigType *ct = &p->config_types.items[i];
        const SnapshotType *orig = nullptr;
        for (auto &snt : s->types) {
            if (snt.name == ct->name) { orig = &snt; break; }
        }
        bool changed = !orig || (int)(ct->kind == TABLE_WORD) != orig->is_word ||
                       ct->value_count != orig->values.size();
        if (!changed && orig) {
            for (size_t j = 0; j < ct->value_count && !changed; j++) {
                if (ct->values[j].value != orig->values[j].value ||
                    orig->values[j].name  != ct->values[j].name) {
                    changed = true;
                }
            }
        }
        if (changed) {
            SnapshotType stype;
            stype.name    = ct->name;
            stype.is_word = (ct->kind == TABLE_WORD) ? 1 : 0;
            for (size_t j = 0; j < ct->value_count; j++) {
                stype.values.push_back({ct->values[j].value, ct->values[j].name});
            }
            ctype.push_back(std::move(stype));
        }
    }
    /* deletion of a type needs full save */
    for (auto &snt : s->types) {
        bool still_there = false;
        for (size_t j = 0; j < p->config_types.count; j++) {
            if (p->config_types.items[j].name == snt.name) { still_there = true; break; }
        }
        if (!still_there) {
            *st = "type deletion needs full snapshot";
            return 0;
        }
    }

    /* collect new or changed symbols; deletions require full save */
    std::vector<SnapshotSymbol> csym;
    for (size_t i = 0; i < p->symbols.count; i++) {
        const char *sname = p->symbols.items[i].name;
        uint32_t    sval  = p->symbols.items[i].value;
        bool found = false;
        for (auto &ss : s->symbols) {
            if (ss.name == sname) { found = true; if (ss.value != sval) csym.push_back({sname, sval}); break; }
        }
        if (!found) csym.push_back({sname, sval});
    }
    for (auto &ss : s->symbols) {
        bool still_there = false;
        for (size_t j = 0; j < p->symbols.count; j++) {
            if (p->symbols.items[j].name == ss.name) { still_there = true; break; }
        }
        if (!still_there) { *st = "symbol deletion needs full snapshot"; return 0; }
    }

    FILE *o = fopen(path, "w");
    if (!o) {
        *st = "open failed";
        return -1;
    }
    fputs("; Apex ImGui overlay\n", o);
    {
        const char *inc = (include_path && *include_path) ? include_path : p->config_path;
        if (inc && *inc) {
            fprintf(o, "include = %s\n", basename_ptr(inc));
        }
    }
    if (!ctype.empty()) {
        fputs("\n[types]\n", o);
        for (auto &t : ctype) {
            fprintf(o, "%s:%s =\n", t.name.c_str(), t.is_word ? "word" : "byte");
            for (size_t vi = 0; vi < t.values.size(); vi++) {
                fprintf(o, t.is_word ? "\t0x%04x:%s\n" : "\t0x%02x:%s\n",
                        t.values[vi].value, t.values[vi].name.c_str());
            }
        }
    }
    if (!cl.empty()) {
        fputs("\n[labels]\n", o);
        for (auto &i : cl) {
            write_config_address(o, i.has_bank, i.bank, i.addr);
            fputs(" = ", o);
            write_escaped_value(o, i.name.c_str());
            fputc('\n', o);
        }
    }
    if (!ce.empty()) {
        fputs("\n[entries]\n", o);
        for (auto &i : ce) {
            write_config_address(o, i.has_bank, i.bank, i.addr);
            fputs(" = code\n", o);
        }
    }
    if (!cexcl.empty()) {
        fputs("\n[exclude_refs]\n", o);
        for (auto &i : cexcl) {
            write_config_address(o, i.has_bank, i.bank, i.addr);
            fputs(" = 1\n", o);
        }
    }
    if (!clits.empty()) {
        fputs("\n[literals]\n", o);
        for (auto &i : clits) {
            write_config_address(o, i.has_bank, i.bank, i.addr);
            fputs(" = literal\n", o);
        }
    }
    if (!ci.empty()) {
        fputs("\n[inline]\n", o);
        for (auto &i : ci) {
            write_config_address(o, i.has_bank, i.bank, i.addr);
            fprintf(o, " = %s\n", i.spec.c_str());
        }
    }
    if (!cd.empty()) {
        fputs("\n[data]\n", o);
        for (auto &i : cd) {
            write_config_address(o, 1, i.bank, i.addr);
            fputs(" = ", o);
            fputs(i.spec.c_str(), o);
            fputc('\n', o);
        }
    }
    if (!ct.empty()) {
        fputs("\n[tables]\n", o);
        for (auto &i : ct) {
            write_config_address(o, 1, i.bank, i.addr);
            fputs(" = ", o);
            fputs(i.spec.c_str(), o);
            fputc('\n', o);
        }
    }
    if (!cdocs.empty()) {
        fputs("\n[docs]\n", o);
        for (auto &i : cdocs) {
            write_config_address(o, i.has_bank, i.bank, i.addr);
            fputs(" = ", o);
            write_escaped_value(o, i.text.c_str());
            fputc('\n', o);
        }
    }
    if (!csym.empty()) {
        fputs("\n[symbols]\n", o);
        for (auto &sym : csym) {
            fprintf(o, "%s = 0x%04x\n", sym.name.c_str(), sym.value & 0xffffu);
        }
    }
    fclose(o);
    *st = "saved delta";
    return 1;
}

int write_full_config(ApexProject *p, const char *path, std::string *st)
{
    apex_project_sort_config(p);
    FILE *o = fopen(path, "w");
    if (!o) {
        *st = "open failed";
        return -1;
    }
    fputs("; Apex ImGui config\n", o);
    if (p->options.labels_are_entries)
        fputs("\n[options]\nlabels_are_entries = true\n", o);
    if (p->config_types.count > 0) {
        fputs("\n[types]\n", o);
        for (size_t i = 0; i < p->config_types.count; i++) {
            const ConfigType *ct = &p->config_types.items[i];
            fprintf(o, "%s:%s =\n", ct->name, (ct->kind == TABLE_WORD) ? "word" : "byte");
            for (size_t j = 0; j < ct->value_count; j++) {
                fprintf(o, (ct->kind == TABLE_WORD) ? "\t0x%04x:%s\n" : "\t0x%02x:%s\n",
                        ct->values[j].value, ct->values[j].name);
            }
        }
    }
    if (p->schemas.count > 0) {
        fputs("\n[schemas]\n", o);
        for (size_t i = 0; i < p->schemas.count; i++) {
            fprintf(o, "%s = %s\n", p->schemas.items[i].name,
                    table_schema_to_string(&p->schemas.items[i].schema).c_str());
        }
    }
    if (p->symbols.count > 0) {
        fputs("\n[symbols]\n", o);
        for (size_t i = 0; i < p->symbols.count; i++)
            fprintf(o, "%s = 0x%04x\n", p->symbols.items[i].name, p->symbols.items[i].value);
    }
    if (p->config_labels.count > 0) {
        fputs("\n[labels]\n", o);
        for (size_t i = 0; i < p->config_labels.count; i++) {
            write_config_address(o, p->config_labels.items[i].has_bank,
                                 p->config_labels.items[i].bank,
                                 p->config_labels.items[i].addr);
            fputs(" = ", o);
            write_escaped_value(o, p->config_labels.items[i].name);
            fputc('\n', o);
        }
    }
    if (p->config_entries.count > 0) {
        fputs("\n[entries]\n", o);
        for (size_t i = 0; i < p->config_entries.count; i++) {
            write_config_address(o, p->config_entries.items[i].has_bank,
                                 p->config_entries.items[i].bank,
                                 p->config_entries.items[i].addr);
            fputs(" = code\n", o);
        }
    }
    if (p->ref_exclusions.count > 0) {
        fputs("\n[exclude_refs]\n", o);
        for (size_t i = 0; i < p->ref_exclusions.count; i++) {
            write_config_address(o, p->ref_exclusions.items[i].has_bank,
                                 p->ref_exclusions.items[i].bank,
                                 p->ref_exclusions.items[i].addr);
            fputs(" = 1\n", o);
        }
    }
    if (p->literals.count > 0) {
        fputs("\n[literals]\n", o);
        for (size_t i = 0; i < p->literals.count; i++) {
            write_config_address(o, p->literals.items[i].has_bank,
                                 p->literals.items[i].bank,
                                 p->literals.items[i].addr);
            fputs(" = literal\n", o);
        }
    }
    if (p->inline_sigs.count > 0) {
        fputs("\n[inline]\n", o);
        for (size_t i = 0; i < p->inline_sigs.count; i++) {
            write_config_address(o, p->inline_sigs.items[i].has_bank,
                                 p->inline_sigs.items[i].bank,
                                 p->inline_sigs.items[i].addr);
            fprintf(o, " = %s", inline_sig_spec_string(&p->inline_sigs.items[i]).c_str());
            if (p->inline_sigs.items[i].flow_stop) fputs(", flow_stop", o);
            fputc('\n', o);
        }
    }
    if (p->data_ranges.count > 0) {
        fputs("\n[data]\n", o);
        for (size_t i = 0; i < p->data_ranges.count; i++) {
            write_config_address(o, 1, p->data_ranges.items[i].bank,
                                 p->data_ranges.items[i].addr);
            fprintf(o, " = %s\n", data_range_spec_string(&p->data_ranges.items[i]).c_str());
        }
    }
    if (p->tables.count > 0) {
        fputs("\n[tables]\n", o);
        for (size_t i = 0; i < p->tables.count; i++) {
            write_config_address(o, 1, p->tables.items[i].bank, p->tables.items[i].addr);
            fprintf(o, " = %s\n", table_def_spec_string(&p->tables.items[i]).c_str());
        }
    }
    if (p->docs.count > 0) {
        fputs("\n[docs]\n", o);
        for (size_t i = 0; i < p->docs.count; i++) {
            write_config_address(o, p->docs.items[i].has_bank,
                                 p->docs.items[i].bank,
                                 p->docs.items[i].addr);
            fputs(" = ", o);
            write_escaped_value(o, p->docs.items[i].text);
            fputc('\n', o);
        }
    }
    fclose(o);
    *st = "saved full config";
    return 1;
}

std::vector<size_t> search_hex_pattern(const ApexProject *project, const char *input)
{
    /* Parse space-separated hex tokens; '??' is a wildcard byte. */
    std::vector<std::pair<uint8_t, bool>> pattern;
    const char *p = input;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (p[0] == '?' && p[1] == '?') {
            pattern.push_back({0, true});
            p += 2;
        } else if (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
            unsigned v = 0;
            char hex[3] = {p[0], p[1], '\0'};
            sscanf(hex, "%x", &v);
            pattern.push_back({(uint8_t)v, false});
            p += 2;
        } else {
            p++;
        }
    }

    std::vector<size_t> results;
    if (pattern.empty() || !project || !project->rom.data || project->rom.size < pattern.size()) {
        return results;
    }

    const uint8_t *rom = project->rom.data;
    size_t rom_size = project->rom.size;
    size_t plen = pattern.size();

    for (size_t i = 0; i + plen <= rom_size; i++) {
        bool match = true;
        for (size_t j = 0; j < plen; j++) {
            if (!pattern[j].second && rom[i + j] != pattern[j].first) {
                match = false;
                break;
            }
        }
        if (match) {
            results.push_back(i);
            if (results.size() >= 500) {
                break;
            }
        }
    }
    return results;
}

std::vector<size_t> find_ram_refs(const ApexRenderedDocument *document, const char *addr_input)
{
    std::vector<size_t> results;
    if (!document || !addr_input || !*addr_input) {
        return results;
    }

    /* Parse address: accept $XX, $XXXX, 0xXX, 0xXXXX, or plain hex. */
    unsigned addr_val = 0;
    const char *p = addr_input;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '$') {
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (sscanf(p, "%x", &addr_val) != 1) {
        return results;
    }
    addr_val &= 0xFFFF;

    /* Build canonical search strings matching the disassembler's output format.
       cpu6809_disassemble_info_ex uses:
         extended:   "0x%04x"   e.g. "0x037f"
         direct:     "<0x%02x"  e.g. "<0x7f"   (always has < prefix) */
    char ext4[10];
    snprintf(ext4, sizeof(ext4), "0x%04x", addr_val);
    size_t ext_len = strlen(ext4);

    /* For addr < 0x100 only: direct-page form <0xXX assumes DP=0x00. */
    char dir2[9];
    size_t dir_len = 0;
    bool check_direct = (addr_val < 0x100);
    if (check_direct) {
        snprintf(dir2, sizeof(dir2), "<0x%02x", addr_val);
        dir_len = strlen(dir2);
    }

    for (size_t i = 0; i < document->line_count; i++) {
        const auto *line = &document->lines[i];
        if (line->kind != APEX_RENDER_LINE_INSTRUCTION) {
            continue;
        }
        const char *text = line->text;
        size_t len = line->length;
        bool found = false;

        /* Search for extended address (0xXXXX), case-insensitive. */
        for (size_t pos = 0; pos + ext_len <= len && !found; pos++) {
            if (strncasecmp(text + pos, ext4, ext_len) == 0) {
                size_t end = pos + ext_len;
                if (end >= len || !isxdigit((unsigned char)text[end])) {
                    results.push_back(i);
                    found = true;
                }
            }
        }

        /* Search for direct-page address (<0xXX), case-insensitive. */
        if (!found && check_direct) {
            for (size_t pos = 0; pos + dir_len <= len && !found; pos++) {
                if (strncasecmp(text + pos, dir2, dir_len) == 0) {
                    size_t end = pos + dir_len;
                    if (end >= len || !isxdigit((unsigned char)text[end])) {
                        results.push_back(i);
                        found = true;
                    }
                }
            }
        }
    }
    return results;
}

void auto_label_targets(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    uint8_t cur_bank;
    uint32_t cur_addr;
    if (!selected_address(*dp, s, &cur_bank, &cur_addr)) {
        return;
    }
    const auto *line = &(*dp)->lines[s->selected_line];
    if (line->kind != APEX_RENDER_LINE_INSTRUCTION) {
        set_status(s, "select an instruction line first");
        return;
    }

    /* Only scan up to the ';' comment delimiter. */
    size_t text_len = line->length;
    for (size_t k = 0; k < line->length; k++) {
        if (line->text[k] == ';') {
            text_len = k;
            break;
        }
    }

    int labeled = 0;
    for (size_t pos = 0; pos < text_len; pos++) {
        if (line->text[pos] != '$') {
            continue;
        }
        /* Skip immediate-mode values: #$XX is a literal, not an address. */
        if (pos > 0 && line->text[pos - 1] == '#') {
            continue;
        }
        /* Collect hex digits. */
        size_t end = pos + 1;
        while (end < text_len && isxdigit((unsigned char)line->text[end])) {
            end++;
        }
        size_t hex_len = end - (pos + 1);
        if (hex_len < 1 || hex_len > 4) {
            pos = end - 1;
            continue;
        }
        unsigned addr_val = 0;
        sscanf(line->text + pos + 1, "%x", &addr_val);
        addr_val &= 0xFFFF;
        pos = end - 1;

        /* Determine addressing context. */
        uint8_t tgt_bank;
        int has_bank;
        if (addr_val < 0x4000) {
            /* RAM — non-banked label. */
            tgt_bank = 0;
            has_bank = 0;
        } else if (addr_val < 0x8000) {
            /* Paged ROM: inherits the current instruction's bank. */
            tgt_bank = cur_bank;
            has_bank = 1;
        } else {
            /* System ROM, bank 0xFF. */
            tgt_bank = 0xFF;
            has_bank = 1;
        }

        /* Skip if already labeled. */
        if (has_bank) {
            if (!label_at_address(*dp, s, tgt_bank, addr_val).empty()) {
                continue;
            }
        } else {
            bool found = false;
            for (size_t i = 0; i < p->config_labels.count; i++) {
                if (!p->config_labels.items[i].has_bank &&
                    p->config_labels.items[i].addr == addr_val) {
                    found = true;
                    break;
                }
            }
            if (found) {
                continue;
            }
        }

        /* Choose a prefix based on the block kind at the target. */
        const char *prefix = has_bank ? "Loc_" : "Ram_";
        if (has_bank) {
            size_t tli;
            if (apex_render_find_line_by_address(*dp, tgt_bank, addr_val, &tli)) {
                switch ((*dp)->lines[tli].block_kind) {
                case APEX_RENDER_BLOCK_CODE:         prefix = "Sub_"; break;
                case APEX_RENDER_BLOCK_DATA:         prefix = "Dat_"; break;
                case APEX_RENDER_BLOCK_SPRITE:       prefix = "Spr_"; break;
                case APEX_RENDER_BLOCK_TABLE:        prefix = "Tab_"; break;
                case APEX_RENDER_BLOCK_UNCLASSIFIED: prefix = "Unc_"; break;
                default:                             prefix = "Loc_"; break;
                }
            }
        }

        char name[32];
        snprintf(name, sizeof(name), "%s%04X", prefix, addr_val);
        apex_project_set_label(p, has_bank, tgt_bank, (uint32_t)addr_val, name);
        labeled++;
    }

    if (labeled > 0) {
        rerender_and_reselect(p, dp, s, cur_bank, cur_addr);
        char msg[64];
        snprintf(msg, sizeof(msg), "auto-labeled %d target(s)", labeled);
        set_status(s, msg);
    } else {
        set_status(s, "no unlabeled address targets found");
    }
}

