#include "apeximgui_core.h"
#include "apeximgui_views_internal.h"
#include "apex_rominfo.h"
#include "backends/imgui_impl_opengl3.h"
#include <SDL_opengl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cfloat>

void render_dmd_list_window(const ApexProject *p, const ApexRenderedDocument *d, UiState *s)
{
    size_t dmd_count = 0;
    for (size_t i = 0; i < p->data_ranges.count; i++)
        if (p->data_ranges.items[i].kind == DATA_DMD_FULLFRAME)
            dmd_count++;
    ImGui::TextDisabled("(%zu frames)", dmd_count);

    if (ImGui::BeginTable("dmd_list", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,  110.0f, 0);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                                0.0f, 1);
        ImGui::TableHeadersRow();

        std::vector<size_t> dmd_rows;
        for (size_t i = 0; i < p->data_ranges.count; i++)
            if (p->data_ranges.items[i].kind == DATA_DMD_FULLFRAME)
                dmd_rows.push_back(i);
        {
            int sort_col; bool sort_asc;
            if (ui_table_sort(&sort_col, &sort_asc)) {
                std::stable_sort(dmd_rows.begin(), dmd_rows.end(), [&](size_t ia, size_t ib) {
                    const DataRange *a = &p->data_ranges.items[ia];
                    const DataRange *b = &p->data_ranges.items[ib];
                    int c = ui_cmp_u32(((uint32_t)a->bank<<16)|(a->addr&0xffffu),
                                       ((uint32_t)b->bank<<16)|(b->addr&0xffffu));
                    return sort_asc ? c < 0 : c > 0;
                });
            }
        }

        /* Clip to on-screen rows: with hundreds/thousands of frames, resolving
           every row's label + document line each frame is O(rows * doc) and
           makes the window crawl.  The clipper limits it to what is visible. */
        ImGuiListClipper clipper;
        clipper.Begin((int)dmd_rows.size());
        while (clipper.Step()) {
            for (int oi = clipper.DisplayStart; oi < clipper.DisplayEnd; oi++) {
                const DataRange *dr = &p->data_ranges.items[dmd_rows[oi]];

                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x",
                         (unsigned)dr->bank, (unsigned)dr->addr & 0xffffu);
                std::string lbl = label_at_address(d, s, dr->bank, dr->addr);
                size_t li = 0;
                bool found = apex_render_find_line_by_address(d, dr->bank, dr->addr, &li) != NULL;
                bool sel = found && s->selected_line == li;

                ImGui::PushID(oi);
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
/* Classify one VSI sub-table's descriptor + pointer array, no re-analysis.
   The image data ranges are produced later by inject_sprite_table_data_ranges
   (a full analysis).  Caller wraps this in an edit group and triggers the
   re-analysis / rescan. */
static void classify_vsi_table_core(ApexProject *p, UiState *s, int table_idx)
{
    const UiState::VsiSubTableInfo *st = nullptr;
    for (auto &t : s->vsi_sub_tables) {
        if (t.table_idx == table_idx) { st = &t; break; }
    }
    if (!st) return;

    /* Sub-table header bytes (min/max pairs + 0x00 + height + spacing).  The raw
       bytes[] carry no semantics on their own, so attach a doc comment that
       explains the descriptor layout and what the table holds. */
    if (st->header_len > 0) {
        char spec[32];
        snprintf(spec, sizeof(spec), "bytes[%zu]", st->header_len);
        apex_project_set_kind(p, 1, st->bank, st->cpu_addr, APEX_KIND_DATA, spec);

        char doc[128];
        snprintf(doc, sizeof(doc),
                 "VSI sub-table header: %d image(s), height %u px "
                 "[index-range pairs, 0x00, height, spacing]",
                 st->num_images, (unsigned)st->table_height);
        apex_project_set_doc(p, 1, st->bank, st->cpu_addr, doc);
    }

    /* Pointer array: rows[N](ptr16_sprite(H)).  The height parameter lets the
       analyser auto-classify the pointed-to images (header-format images are
       self-describing; no-header ones need this height). */
    if (st->num_images > 0) {
        uint32_t ptr_addr = st->cpu_addr + (uint32_t)st->header_len;
        char spec[64];
        snprintf(spec, sizeof(spec), "rows[%d](ptr16_sprite(%d))",
                 st->num_images, (int)st->table_height);
        apex_project_set_kind(p, 1, st->bank, ptr_addr, APEX_KIND_TABLE, spec);
    }
}

static void classify_vsi_table(ApexProject *p, const ApexRenderedDocument **dp,
                               UiState *s, int table_idx)
{
    uint8_t b = 0xffu; uint32_t a = 0;
    for (auto &t : s->vsi_sub_tables) {
        if (t.table_idx == table_idx) { b = t.bank; a = t.cpu_addr; break; }
    }
    /* One logical action -> one undo step. */
    apex_project_begin_edit_group(p, "classify VSI table");
    classify_vsi_table_core(p, s, table_idx);
    apex_project_end_edit_group(p);

    apex_project_invalidate(p, APEX_DIRTY_ANALYSIS);
    rerender_and_reselect(p, dp, s, b, a);
    scan_vsi_table_candidates(p, s);
}

/* Classify every detected VSI sub-table in one undo step / one re-analysis. */
static void classify_all_vsi_tables(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    /* Snapshot the table indices first: classify_vsi_table_core doesn't rescan,
       so vsi_sub_tables is stable during the loop, but copying is robust. */
    std::vector<int> idxs;
    for (auto &t : s->vsi_sub_tables) idxs.push_back(t.table_idx);
    if (idxs.empty()) return;

    uint8_t keep_b = 0xffu; uint32_t keep_a = 0;
    selected_address(*dp, s, &keep_b, &keep_a);

    apex_project_begin_edit_group(p, "classify all VSI tables");
    for (int idx : idxs) classify_vsi_table_core(p, s, idx);
    apex_project_end_edit_group(p);

    apex_project_invalidate(p, APEX_DIRTY_ANALYSIS);
    rerender_and_reselect(p, dp, s, keep_b, keep_a);
    scan_vsi_table_candidates(p, s);
    set_status(s, "classified all VSI tables");
}

/* Build a per-byte block-kind map of the ROM from the rendered document, the
   same forward-fill used by the coverage panel.  Used to restrict the sprite
   scan to not-yet-classified bytes. */
static void build_kind_map(const ApexProject *p, const ApexRenderedDocument *d,
                           std::vector<uint8_t> &kinds)
{
    size_t rom_size = p->rom.size, i, fill = 0;
    uint8_t cur = (uint8_t)APEX_RENDER_BLOCK_UNKNOWN;

    kinds.assign(rom_size, (uint8_t)APEX_RENDER_BLOCK_UNKNOWN);
    if (!d) return;
    for (i = 0; i < d->line_count && fill < rom_size; i++) {
        const ApexRenderedLine *l = &d->lines[i];
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

static void scan_sprite_candidates(const ApexProject *p, const ApexRenderedDocument *d,
                                   UiState *s)
{
    s->sprite_candidates.clear();
    if (!p->rom.data || p->rom.size == 0) {
        s->sprite_scan_done = true;
        return;
    }
    /* Only scan bytes that aren't already classified — this drops the bulk of
       false hits that land inside known code/data/table/sprite regions. */
    std::vector<uint8_t> kinds;
    build_kind_map(p, d, kinds);
    uint8_t tmp[APEX_SPRITE_MAX_BYTES];
    for (size_t off = 0; off < p->rom.size; off++) {
        uint8_t k = kinds[off];
        if (k != (uint8_t)APEX_RENDER_BLOCK_UNKNOWN &&
            k != (uint8_t)APEX_RENDER_BLOCK_UNCLASSIFIED)
            continue;
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

/* Jump the disassembly and hex views to a sprite at (bank, addr).  The hex
   cursor lands on the exact sprite offset even when the sprite sits inside a
   larger data block whose disassembly line starts earlier. */
static void sprite_navigate(ApexProject *p, const ApexRenderedDocument *d, UiState *s,
                            uint8_t bank, uint32_t addr)
{
    const uint8_t *src;
    size_t len = 0, off = 0;
    size_t li = 0;

    if (!project_locate_rom_bytes(p, bank, addr, &src, &len, &off)) {
        return;
    }
    if (d && find_line_by_rom_offset(d, off, &li)) {
        select_line(s, li, 1);
    }
    /* Point the hex cursor at the exact sprite offset and suppress the
       disasm->hex re-sync that would otherwise snap it to the line start. */
    s->hex_selected_offset    = off;
    s->hex_active             = true;
    s->hex_is_edit_target     = true;
    s->hex_request_follow     = 1;
    s->hex_prev_selected_line = s->selected_line;
    /* Make both views visible so the jump is observable. */
    s->show_disasm = true;
    s->show_hex    = true;
}

static bool sprite_addr_classified(const ApexProject *p, uint8_t bank, uint32_t addr)
{
    for (size_t i = 0; i < p->data_ranges.count; i++) {
        const DataRange *dr = &p->data_ranges.items[i];
        if ((dr->kind == DATA_SPRITE || dr->kind == DATA_SPRITE_NOHEADER) &&
            dr->bank == bank && dr->addr == addr) {
            return true;
        }
    }
    return false;
}

/* Re-derive the cached "classified" flags from the live config so the sprite
   windows reflect classify/clear edits — including undo/redo — without a
   re-scan. */
static void refresh_sprite_classified(const ApexProject *p, UiState *s)
{
    for (auto &e : s->vsi_table_entries)
        e.classified = sprite_addr_classified(p, e.bank, e.cpu_addr);
    for (auto &e : s->sprite_candidates)
        e.classified = sprite_addr_classified(p, e.bank, e.cpu_addr);
}

void render_sprite_list_window(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    /* Auto-run the (cheap, structured) VSI table scan the first time the window
       is shown so sprites appear without a manual click; the ROM byte scan stays
       on its button. */
    if (!s->vsi_table_scan_done) {
        scan_vsi_table_candidates(p, s);
    }
    refresh_sprite_classified(p, s);
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
        scan_sprite_candidates(p, d, s);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Find sprite candidates in not-yet-classified ROM regions.\n"
                          "Review each (hover/Gallery) and click Classify to apply.");
    ImGui::SameLine();
    if (ImGui::Button("Gallery")) {
        s->show_sprite_gallery = true;
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

    /* Filter controls. InputInt's width is the whole widget (field + the two
       step buttons), so size it to fit a 3-digit field plus both buttons; derive
       it from font metrics so it stays correct under font zoom. */
    const float sp_field_w = ImGui::CalcTextSize("000").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                             (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x) * 2.0f;
    ImGui::SetNextItemWidth(sp_field_w); ImGui::InputInt("W min", &s->sprite_filter_min_w);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sp_field_w); ImGui::InputInt("W max", &s->sprite_filter_max_w);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sp_field_w); ImGui::InputInt("H min", &s->sprite_filter_min_h);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sp_field_w); ImGui::InputInt("H max", &s->sprite_filter_max_h);
    s->sprite_filter_min_w = std::max(1, std::min(s->sprite_filter_min_w, 128));
    s->sprite_filter_max_w = std::max(s->sprite_filter_min_w, std::min(s->sprite_filter_max_w, 128));
    s->sprite_filter_min_h = std::max(1, std::min(s->sprite_filter_min_h, 32));
    s->sprite_filter_max_h = std::max(s->sprite_filter_min_h, std::min(s->sprite_filter_max_h, 32));

    /* --- VSI Sub-Table overview (shown when scan produced results) --- */
    if (s->vsi_table_scan_done && !s->vsi_sub_tables.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("VSI sub-tables — click Classify to apply all entries to config");
        ImGui::SameLine();
        if (ImGui::SmallButton("Classify all##vsi")) {
            classify_all_vsi_tables(p, dp, s);
            d = *dp; /* document + sub-table scan rebuilt; refresh local pointer */
        }

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

            /* Precompute per-table (pass, done) counts in a single pass instead
               of re-scanning every entry for every sub-table row. */
            std::map<int, std::pair<int, int>> tbl_counts;
            for (auto &e : s->vsi_table_entries) {
                if (e.width  < s->sprite_filter_min_w || e.width  > s->sprite_filter_max_w) continue;
                if (e.height < s->sprite_filter_min_h || e.height > s->sprite_filter_max_h) continue;
                auto &c = tbl_counts[e.table_idx];
                c.first++;
                if (e.classified) c.second++;
            }

            bool vsi_reclassified = false;
            for (auto &st : s->vsi_sub_tables) {
                int pass = tbl_counts[st.table_idx].first;
                int done = tbl_counts[st.table_idx].second;

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

    /* Build a flat list of rows to display (cheap metadata only — no sprite
       decoding here).  The table is then virtualised with ImGuiListClipper so
       per-row work (label lookup, line lookup, and sprite decode for size/hover)
       only happens for the handful of rows actually on screen.  Decoding every
       sprite every frame was what made this window crawl with many sprites. */
    enum RowSrc { ROW_VSI, ROW_CLASS, ROW_SCAN };
    struct SpriteRow {
        RowSrc   src;
        uint8_t  bank;
        uint32_t addr;
        uint16_t w, h;        /* known for VSI/SCAN; 0 for CLASS (decode on demand) */
        bool     is_noheader;
        bool     classified;  /* VSI: already applied to config */
        int      table_idx, image_idx;  /* VSI */
        bool     class_noheader;         /* CLASS: DATA_SPRITE_NOHEADER */
    };
    std::vector<SpriteRow> rows;

    if (s->vsi_table_scan_done) {
        for (auto &e : s->vsi_table_entries) {
            if (e.width  < s->sprite_filter_min_w || e.width  > s->sprite_filter_max_w) continue;
            if (e.height < s->sprite_filter_min_h || e.height > s->sprite_filter_max_h) continue;
            rows.push_back({ROW_VSI, e.bank, e.cpu_addr, e.width, e.height,
                            e.is_noheader, e.classified, e.table_idx, e.image_idx, false});
        }
    }
    for (size_t i = 0; i < p->data_ranges.count; i++) {
        const DataRange *dr = &p->data_ranges.items[i];
        if (dr->kind != DATA_SPRITE && dr->kind != DATA_SPRITE_NOHEADER) continue;
        rows.push_back({ROW_CLASS, dr->bank, dr->addr, 0, 0, false, false, 0, 0,
                        dr->kind == DATA_SPRITE_NOHEADER});
    }
    if (s->sprite_scan_done) {
        for (auto &e : s->sprite_candidates) {
            if (e.classified) continue;
            if (e.width  < s->sprite_filter_min_w || e.width  > s->sprite_filter_max_w) continue;
            if (e.height < s->sprite_filter_min_h || e.height > s->sprite_filter_max_h) continue;
            rows.push_back({ROW_SCAN, e.bank, e.cpu_addr, e.width, e.height,
                            false, false, 0, 0, false});
        }
    }

    /* Deferred classify: applied after the table loop so we never mutate the
       config / rerender while iterating the clipped rows. */
    bool     do_classify = false;
    uint8_t  cls_bank = 0;
    uint32_t cls_addr = 0;
    bool     cls_noheader = false;

    if (ImGui::BeginTable("sprite_list", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | APEX_TABLE_SORT_FLAGS)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,   110.0f, 0);
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed,    60.0f, 1);
        ImGui::TableSetupColumn("Kind",    ImGuiTableColumnFlags_WidthFixed,    90.0f, 2);
        ImGui::TableSetupColumn("Tbl/Img", ImGuiTableColumnFlags_WidthFixed,    60.0f, 3);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                                0.0f, 4);
        ImGui::TableHeadersRow();

        int sort_col; bool sort_asc;
        if (ui_table_sort(&sort_col, &sort_asc)) {
            std::stable_sort(rows.begin(), rows.end(),
                [&](const SpriteRow &a, const SpriteRow &b) {
                    int c = 0;
                    switch (sort_col) {
                    case 0: c = ui_cmp_u32(((uint32_t)a.bank<<16)|a.addr,
                                           ((uint32_t)b.bank<<16)|b.addr); break;
                    case 1: c = ui_cmp_int(a.w*a.h, b.w*b.h); break;
                    case 2: c = ui_cmp_int(a.src, b.src); break;
                    case 3: c = a.table_idx != b.table_idx ? ui_cmp_int(a.table_idx, b.table_idx)
                                                           : ui_cmp_int(a.image_idx, b.image_idx); break;
                    }
                    return sort_asc ? c < 0 : c > 0;
                });
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)rows.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                const SpriteRow &r = rows[(size_t)row];
                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x",
                         (unsigned)r.bank, (unsigned)r.addr & 0xffffu);
                std::string lbl = label_at_address(d, s, r.bank, r.addr);
                size_t li = 0;
                bool found = apex_render_find_line_by_address(d, r.bank, r.addr, &li) != NULL;
                bool sel = found && s->selected_line == li;

                /* CLASS rows need a decode to show their size; reuse it for the
                   hover preview.  VSI/SCAN already carry dimensions. */
                SpritePreviewInfo pr = {};
                bool have_pr = false;
                if (r.src == ROW_CLASS)
                    have_pr = decode_sprite_preview_at(p, r.bank, r.addr, &pr) != 0;

                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(addrstr, sel,
                        ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowOverlap)) {
                    sprite_navigate(p, d, s, r.bank, r.addr);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem)) {
                    if (r.src != ROW_CLASS)
                        have_pr = decode_sprite_preview_at(p, r.bank, r.addr, &pr) != 0;
                    if (have_pr) {
                        ImGui::BeginTooltip();
                        render_sprite_preview(pr, 6.0f);
                        if (r.src == ROW_VSI) {
                            char info[64];
                            snprintf(info, sizeof(info), "T%d I%d  %ux%u  %s",
                                     r.table_idx, r.image_idx,
                                     (unsigned)r.w, (unsigned)r.h,
                                     r.is_noheader ? "no-hdr" : "hdr");
                            ImGui::TextUnformatted(info);
                        }
                        ImGui::EndTooltip();
                    }
                }
                ImGui::TableSetColumnIndex(1);
                if (r.src == ROW_CLASS) {
                    if (have_pr) ImGui::Text("%ux%u", (unsigned)pr.width, (unsigned)pr.height);
                    else         ImGui::TextDisabled("?");
                } else {
                    ImGui::Text("%ux%u", (unsigned)r.w, (unsigned)r.h);
                }
                ImGui::TableSetColumnIndex(2);
                if (r.src == ROW_VSI) {
                    if (r.classified)
                        ImGui::TextColored(ImVec4(0.47f, 0.86f, 1.00f, 1.0f),
                                           r.is_noheader ? "vsi_nh*" : "vsi*");
                    else
                        ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
                                           r.is_noheader ? "vsi_nh" : "vsi");
                } else if (r.src == ROW_CLASS) {
                    ImGui::TextColored(ImVec4(0.47f, 0.86f, 1.00f, 1.0f),
                                       r.class_noheader ? "spr_nh" : "sprite");
                } else {
                    /* Manual qualification: apply this scan candidate as a
                       sprite only on explicit click (undoable single edit). */
                    if (ImGui::SmallButton("Classify")) {
                        do_classify  = true;
                        cls_bank     = r.bank;
                        cls_addr     = r.addr;
                        cls_noheader = false;
                    }
                }
                ImGui::TableSetColumnIndex(3);
                if (r.src == ROW_VSI)
                    ImGui::Text("T%d I%d", r.table_idx, r.image_idx);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(lbl.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (do_classify) {
        if (apex_project_set_kind(p, 1, cls_bank, cls_addr, APEX_KIND_DATA,
                                  cls_noheader ? "sprite_noheader" : "sprite") == 0) {
            rerender_and_reselect(p, dp, s, cls_bank, cls_addr);
        }
    }
}

/* Draw a decoded sprite image into a fixed (box_w × box_h) cell, scaled to fit
   while preserving aspect ratio.  Reserves exactly the box so gallery table rows
   stay uniform height (required by ImGuiListClipper).  Only lit pixels are
   drawn over a dark background to keep the rect count down. */
static void draw_sprite_thumbnail(const SpritePreviewInfo &pr, float box_w, float box_h, int zoom)
{
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(box_w, box_h)); /* fixed-size cell keeps clipper rows uniform */
    if (!pr.valid || pr.width == 0 || pr.height == 0) {
        return;
    }
    /* Render at original size times the zoom factor (top-left aligned), clipped
       to the box, so sprites of different proportions are shown undistorted
       rather than stretched to the row height. */
    float z = zoom < 1 ? 1.0f : (float)zoom;
    float img_w = (float)pr.width  * z < box_w ? (float)pr.width  * z : box_w;
    float img_h = (float)pr.height * z < box_h ? (float)pr.height * z : box_h;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + img_w, origin.y + img_h),
                        IM_COL32(6, 18, 28, 255));
    uint8_t row_bytes = (uint8_t)((pr.width + 7u) / 8u);
    for (uint8_t row = 0; row < pr.height && (float)row * z < box_h; row++) {
        for (uint8_t col_byte = 0; col_byte < row_bytes; col_byte++) {
            /* OR both planes for a clear silhouette. */
            uint8_t bits = pr.pixels[row * row_bytes + col_byte] |
                           (pr.two_plane ? pr.pixels1[row * row_bytes + col_byte] : 0u);
            if (!bits) continue;
            for (size_t bit = 0; bit < 8u; bit++) {
                int px = (int)(col_byte * 8u + bit);
                if (px >= (int)pr.width || (float)px * z >= box_w) break;
                if (!((bits >> bit) & 1u)) continue;
                ImVec2 p0(origin.x + (float)px * z, origin.y + (float)row * z);
                draw->AddRectFilled(p0, ImVec2(p0.x + z, p0.y + z),
                                    IM_COL32(120, 220, 255, 255));
            }
        }
    }
}

/* Gallery view: every detected/defined sprite as a thumbnail image alongside its
   address and label.  Sources (classified data ranges, VSI table entries, ROM
   scan candidates) are merged and de-duplicated by address.  Virtualised with a
   clipper so only the on-screen thumbnails are decoded and drawn each frame. */
void render_sprite_gallery_window(ApexProject *p, const ApexRenderedDocument **dp, UiState *s)
{
    const ApexRenderedDocument *d = *dp;

    if (!s->vsi_table_scan_done) {
        scan_vsi_table_candidates(p, s);
    }
    refresh_sprite_classified(p, s);

    /* Merge + de-dup the three sources by (bank, addr); classified always shown,
       VSI/scan respect the dimension filter (same as the Sprites list). */
    std::vector<std::pair<uint8_t, uint32_t>> items;
    for (size_t i = 0; i < p->data_ranges.count; i++) {
        const DataRange *dr = &p->data_ranges.items[i];
        if (dr->kind == DATA_SPRITE || dr->kind == DATA_SPRITE_NOHEADER)
            items.push_back({dr->bank, dr->addr});
    }
    if (s->vsi_table_scan_done) {
        for (auto &e : s->vsi_table_entries) {
            if (e.width  < s->sprite_filter_min_w || e.width  > s->sprite_filter_max_w) continue;
            if (e.height < s->sprite_filter_min_h || e.height > s->sprite_filter_max_h) continue;
            items.push_back({e.bank, e.cpu_addr});
        }
    }
    if (s->sprite_scan_done) {
        for (auto &e : s->sprite_candidates) {
            if (e.classified) continue;
            if (e.width  < s->sprite_filter_min_w || e.width  > s->sprite_filter_max_w) continue;
            if (e.height < s->sprite_filter_min_h || e.height > s->sprite_filter_max_h) continue;
            items.push_back({e.bank, e.cpu_addr});
        }
    }
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());

    ImGui::TextDisabled("%zu sprite(s) — scan VSI/ROM in the Sprites window to add more",
                        items.size());
    if (items.empty()) {
        return;
    }

    if (s->sprite_gallery_zoom < 1) s->sprite_gallery_zoom = 1;
    ImGui::TextUnformatted("Zoom:");
    ImGui::SameLine();
    ImGui::RadioButton("1x", &s->sprite_gallery_zoom, 1); ImGui::SameLine();
    ImGui::RadioButton("2x", &s->sprite_gallery_zoom, 2); ImGui::SameLine();
    ImGui::RadioButton("4x", &s->sprite_gallery_zoom, 4);

    /* Rows sized to the DMD/sprite maximum (32px) times the zoom; sprites render
       at original size * zoom inside, top-left aligned. */
    const int   zoom = s->sprite_gallery_zoom;
    const float kImgW = 132.0f * (float)zoom;
    const float kImgH = 32.0f * (float)zoom;

    bool     do_classify = false;
    uint8_t  cls_bank = 0;
    uint32_t cls_addr = 0;

    if (ImGui::BeginTable("sprite_gallery", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Image",   ImGuiTableColumnFlags_WidthFixed, kImgW + 6.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed,  55.0f);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)items.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                uint8_t  bank = items[(size_t)row].first;
                uint32_t addr = items[(size_t)row].second;

                SpritePreviewInfo pr = {};
                bool have_pr = decode_sprite_preview_at(p, bank, addr, &pr) != 0;

                char addrstr[32];
                snprintf(addrstr, sizeof(addrstr), "B%02x_A%04x",
                         (unsigned)bank, (unsigned)addr & 0xffffu);
                std::string lbl = label_at_address(d, s, bank, addr);
                size_t li = 0;
                bool found = apex_render_find_line_by_address(d, bank, addr, &li) != NULL;
                bool sel = found && s->selected_line == li;

                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                draw_sprite_thumbnail(pr, kImgW, kImgH, zoom);
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Selectable(addrstr, sel,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    sprite_navigate(p, d, s, bank, addr);
                }
                ImGui::TableSetColumnIndex(2);
                if (have_pr) ImGui::Text("%ux%u", (unsigned)pr.width, (unsigned)pr.height);
                else         ImGui::TextDisabled("?");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(lbl.c_str());
                if (!sprite_addr_classified(p, bank, addr)) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Classify")) {
                        do_classify = true;
                        cls_bank = bank;
                        cls_addr = addr;
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (do_classify) {
        if (apex_project_set_kind(p, 1, cls_bank, cls_addr, APEX_KIND_DATA, "sprite") == 0) {
            rerender_and_reselect(p, dp, s, cls_bank, cls_addr);
        }
    }
}

// ============================================================
// ROM Info panel
// ============================================================

void render_rom_info(ApexProject *p, const ApexRenderedDocument *document, UiState *state)
{
    RomInfoState &ri = state->rom_info;

    if (!ri.computed) {
        if (p->rom.data && p->rom.size >= 32768u) {
            ApexRomInfo info;
            apex_rominfo_compute(p->rom.data, p->rom.size, &info);
            ri.os_valid    = info.os_valid;
            ri.os_major    = info.os_major;
            ri.os_minor    = info.os_minor;
            ri.reset_addr  = info.reset_addr;
            memcpy(ri.game_version, info.game_version, sizeof(ri.game_version));
            ri.game_id_found    = info.game_id_found;
            ri.game_id_ptr_addr = info.game_id_ptr_addr;
            memcpy(ri.game_name,   info.game_name,   sizeof(ri.game_name));
            memcpy(ri.game_number, info.game_number, sizeof(ri.game_number));
            memcpy(ri.game_date,   info.game_date,   sizeof(ri.game_date));
            ri.game_name_bank   = info.game_name_bank;   ri.game_name_addr   = info.game_name_addr;
            ri.game_number_bank = info.game_number_bank; ri.game_number_addr = info.game_number_addr;
            ri.game_date_bank   = info.game_date_bank;   ri.game_date_addr   = info.game_date_addr;
            ri.stored_csum  = info.stored_csum;
            ri.computed_csum = info.computed_csum;
            ri.stored_delta  = info.stored_delta;
            ri.crc32_val     = info.crc32_val;
            memcpy(ri.sha1,   info.sha1,   20);
            memcpy(ri.sha256, info.sha256, 32);
            ri.computed = true;
        } else {
            ImGui::TextDisabled("No ROM loaded.");
            return;
        }
    }

    const char *rom_name = p->rom_path ? p->rom_path : "(unknown)";
    /* Show just the filename */
    const char *base = strrchr(rom_name, '/');
    if (!base) base = strrchr(rom_name, '\\');
    if (!base) base = rom_name - 1;
    ImGui::TextUnformatted(base + 1);

    size_t rom_size = p->rom.size;
    if (rom_size >= 1048576u)
        ImGui::Text("%zu bytes (%zu MB)", rom_size, rom_size / 1048576u);
    else
        ImGui::Text("%zu bytes (%zu KB)", rom_size, rom_size / 1024u);

    ImGui::Separator();

    if (ImGui::BeginTable("##rominfo", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

        auto row = [&](const char *label, const char *value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(value);
        };
        auto rowf = [&](const char *label, const char *fmt, ...) {
            char buf[128]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
            row(label, buf);
        };

        /* OS version */
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("OS Version");
        ImGui::TableSetColumnIndex(1);
        if (ri.os_valid)
            ImGui::Text("%u.%u", (unsigned)ri.os_major, (unsigned)ri.os_minor);
        else
            ImGui::TextDisabled("unknown (reset 0x%04X)", ri.reset_addr);

        /* Game version */
        row("Game Version", ri.game_version[0] ? ri.game_version : "(not found)");

        /* Game identity (name / number / date), resolved via three far pointers
           in the system bank.  Each value jumps to the target string; the pointer
           address next to it jumps to the far pointer itself. */
        if (ri.game_id_found) {
            auto jump_to = [&](uint8_t bk, uint16_t ad) {
                size_t li;
                const uint8_t *src; size_t len, off;
                if (apex_render_find_line_by_address(document, bk, ad, &li)) {
                    select_line(state, li, 1);
                } else if (project_locate_rom_bytes(p, bk, ad, &src, &len, &off) &&
                           find_line_by_rom_offset(document, off, &li)) {
                    /* address falls inside a block (e.g. an unclassified far-pointer
                       table or string) — select the line that contains it. */
                    select_line(state, li, 1);
                }
            };
            auto id_row = [&](const char *label, const char *value, uint16_t ptr_addr,
                              uint8_t tb, uint16_t ta) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", label);
                ImGui::TableSetColumnIndex(1);
                ImGui::PushID(label);
                if (ImGui::SmallButton(value[0] ? value : "(empty)")) jump_to(tb, ta);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("go to string  B%02x_A%04x", (unsigned)tb, (unsigned)ta & 0xffffu);
                ImGui::SameLine();
                char pbuf[16];
                snprintf(pbuf, sizeof(pbuf), "Bff_A%04x", (unsigned)ptr_addr);
                if (ImGui::SmallButton(pbuf)) jump_to(0xffu, ptr_addr);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("go to far pointer");
                ImGui::PopID();
            };
            /* "Game ID" header jumps to the first (name) far pointer. */
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Game ID");
            ImGui::TableSetColumnIndex(1);
            char gbuf[28];
            snprintf(gbuf, sizeof(gbuf), "far ptrs @ Bff_A%04x", (unsigned)ri.game_id_ptr_addr);
            if (ImGui::SmallButton(gbuf)) jump_to(0xffu, ri.game_id_ptr_addr);
            id_row("Name",   ri.game_name,   ri.game_id_ptr_addr,                  ri.game_name_bank,   ri.game_name_addr);
            id_row("Number", ri.game_number, (uint16_t)(ri.game_id_ptr_addr + 3u), ri.game_number_bank, ri.game_number_addr);
            id_row("Date",   ri.game_date,   (uint16_t)(ri.game_id_ptr_addr + 6u), ri.game_date_bank,   ri.game_date_addr);

            /* One-click: label + classify the OS-version bytes, the three far
               pointers (far_data), and the three target strings. */
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            if (ImGui::SmallButton("Classify + label metadata")) {
                uint16_t base = ri.game_id_ptr_addr;
                /* Set our label only where the user has no custom label yet (an
                   auto-generated Bxx_Ayyyy/STRING_ name, or none, may be replaced). */
                auto set_label_pref = [&](uint8_t bank, uint16_t addr, const char *name) {
                    std::string cur = label_at_address(document, state, bank, addr);
                    if (!cur.empty() && !generated_any_label_name(cur.c_str())) return;
                    apex_project_set_label(p, 1, bank, addr, name);
                };
                if (ri.os_valid && ri.reset_addr >= 0x8002u) {
                    uint16_t osa = (uint16_t)(ri.reset_addr - 2u);
                    set_label_pref(0xffu, osa, "APPLE_OS_VERSION");
                    apex_project_set_kind(p, 1, 0xffu, osa, APEX_KIND_DATA, "bytes[2]");
                }
                /* far pointers first, so the explicit string typing of their
                   targets below wins over any data seeding they trigger. */
                set_label_pref(0xffu, base,             "GAME_NAME_PTR");
                apex_project_set_kind(p, 1, 0xffu, base,             APEX_KIND_DATA, "far_data");
                set_label_pref(0xffu, (uint16_t)(base + 3u), "GAME_NUMBER_PTR");
                apex_project_set_kind(p, 1, 0xffu, (uint16_t)(base + 3u), APEX_KIND_DATA, "far_data");
                set_label_pref(0xffu, (uint16_t)(base + 6u), "GAME_DATE_PTR");
                apex_project_set_kind(p, 1, 0xffu, (uint16_t)(base + 6u), APEX_KIND_DATA, "far_data");
                /* Name/Date are NUL-terminated strings; the Number is exactly five
                   digits and often NOT terminated, so give it a fixed length. */
                set_label_pref(ri.game_name_bank,   ri.game_name_addr,   "GAME_NAME");
                apex_project_set_kind(p, 1, ri.game_name_bank,   ri.game_name_addr,   APEX_KIND_STRING, "string");
                set_label_pref(ri.game_number_bank, ri.game_number_addr, "GAME_NUMBER");
                apex_project_set_kind(p, 1, ri.game_number_bank, ri.game_number_addr, APEX_KIND_DATA, "string[5]");
                set_label_pref(ri.game_date_bank,   ri.game_date_addr,   "GAME_DATE");
                apex_project_set_kind(p, 1, ri.game_date_bank,   ri.game_date_addr,   APEX_KIND_STRING, "string");
                rerender_and_reselect(p, &document, state, 0xffu, base);
                set_status(state, "game metadata labelled + classified");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Add APPLE_OS_VERSION, GAME_*_PTR (far_data) and GAME_* (string) "
                                  "labels/types");
        } else {
            row("Game ID", "(not found)");
        }

        ImGui::TableNextRow(); /* spacer */

        /* Checksum */
        rowf("Checksum", "0x%04X (stored)  0x%04X (computed)",
             ri.stored_csum, ri.computed_csum);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Status");
        ImGui::TableSetColumnIndex(1);
        if (ri.computed_csum == ri.stored_csum)
            ImGui::TextColored(ImVec4(0.47f, 0.86f, 0.47f, 1.0f), "VALID");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "INVALID");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Delta");
        ImGui::TableSetColumnIndex(1);
        if (ri.stored_delta == APEX_ROMINFO_DISABLE_DELTA)
            ImGui::Text("0x%04X  (check disabled)", ri.stored_delta);
        else
            ImGui::Text("0x%04X", ri.stored_delta);

        ImGui::TableNextRow(); /* spacer */

        /* Hashes */
        rowf("CRC-32", "%08X", ri.crc32_val);

        { char buf[48]; int i;
          for (i = 0; i < 20; i++) snprintf(buf + i*2, 3, "%02x", ri.sha1[i]);
          row("SHA-1", buf); }

        { char buf[72]; int i;
          for (i = 0; i < 32; i++) snprintf(buf + i*2, 3, "%02x", ri.sha256[i]);
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("SHA-256");
          ImGui::TableSetColumnIndex(1);
          /* Split across two lines for readability */
          char lo[36], hi[36];
          memcpy(lo, buf,      32); lo[32] = '\0';
          memcpy(hi, buf + 32, 32); hi[32] = '\0';
          ImGui::TextUnformatted(lo);
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(hi);
        }

        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::Button("Refresh")) {
        ri.computed = false;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Recompute hashes (takes ~1s for 1MB ROM)");
}
