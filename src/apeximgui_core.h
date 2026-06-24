#ifndef APEXIMGUI_CORE_H
#define APEXIMGUI_CORE_H

#include "imgui.h"
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <map>

extern "C" {
#include "apexdmd.h"
#include "apexsprite.h"
#include "apex_project.h"
#include "apex_render.h"
#include "apex_analysis.h"
#include "apex_match.h"
#include "apex_compare.h"
}

// --- ROM Info State ---

struct RomInfoState {
    bool     computed;
    /* metadata */
    bool     os_valid;
    uint8_t  os_major;
    uint8_t  os_minor;
    uint32_t reset_addr;
    char     game_version[32];
    uint16_t stored_csum;
    uint16_t computed_csum;
    uint16_t stored_delta;
    /* hashes */
    uint32_t crc32_val;
    uint8_t  sha1[20];
    uint8_t  sha256[32];
};

// --- Match Window State ---

struct MatchWindowState {
    char ref_rom_path[1024];
    char ref_ini_path[1024];
    bool scan_enabled;
    int  min_confidence;
    bool has_results;
    std::string run_status;
    ApexProject *src_project;

    struct Result {
        std::string label_name;
        uint32_t    src_addr;
        uint8_t     src_bank;
        uint32_t    dst_addr;
        uint8_t     dst_bank;
        int         confidence;
        bool        accepted;
    };
    std::vector<Result> results;
    char filter[128];
    int  show_mode;  /* 0=all, 1=pending, 2=accepted */

    ~MatchWindowState() {
        if (src_project) { apex_project_free(src_project); src_project = nullptr; }
    }
};

struct CompareWindowState {
    char rom_b_path[1024] = {0};
    char ini_b_path[1024] = {0};
    int  min_instrs       = 5;
    bool inc_code         = true;
    bool inc_strings      = true;
    bool inc_tables       = true;
    bool inject_paged     = false;
    bool has_results      = false;
    std::string run_status;
    ApexProject       *b_project = nullptr;
    ApexFingerprintDB *a_db      = nullptr;  /* snapshot of A at run time */
    ApexFingerprintDB *b_db      = nullptr;
    std::vector<ApexCompareEntry> results;
    size_t n_identical = 0, n_moved = 0, n_changed = 0, n_removed = 0, n_added = 0;
    char filter[128] = {0};
    bool show_identical = false;
    bool show_moved     = true;
    bool show_changed   = true;
    bool show_removed   = true;
    bool show_added     = true;

    void reset() {
        if (b_db)      { apex_fingerprint_free(b_db);  b_db = nullptr; }
        if (a_db)      { apex_fingerprint_free(a_db);  a_db = nullptr; }
        if (b_project) { apex_project_free(b_project); b_project = nullptr; }
        results.clear();
        has_results = false;
    }
    ~CompareWindowState() { reset(); }
};

struct CoverageWindowState {
    bool        computed = false;
    const void *doc_ptr  = nullptr;   /* staleness check against the rendered doc */
    size_t      rom_size = 0;
    size_t      totals[7] = {0,0,0,0,0,0,0};  /* indexed by ApexRenderedBlockKind */
    bool        include_unknown = false;       /* also list never-reached UNKNOWN runs */
    int         min_gap = 2;                    /* ignore gaps shorter than this */
    struct Gap { size_t off; size_t len; uint8_t bank; uint32_t addr; int unknown; };
    std::vector<Gap> gaps;
    int         next_gap = 0;                   /* cursor for "jump to next gap" */
};

// --- Constants and Enums ---

#define APEX_MAX_EDIT_FIELDS 12

/* A single field in an inline or table-schema field list. */
typedef struct {
    int  kind;          /* TableFieldKind cast to int, or -1 for a named type */
    int  count;         /* repeat count (>= 1) */
    char type_name[32]; /* valid when kind == -1 */
    int  param;         /* *_sprite fields: no-header image height; 0 = none */
} ApexEditField;


// --- Structures ---

struct CpuHelpInfo {
    const char *mnemonic;
    const char *desc;
    const char *flags;
    const char *cycles;
};

struct HardwareRegister {
    uint32_t addr;
    const char *name;
    const char *desc;
};

struct Bookmark {
    uint8_t bank;
    uint32_t addr;
    std::string name;
};

struct GraphNode {
    uint8_t bank;
    uint32_t addr;
    std::string name;
    ImVec2 pos;
    ImVec2 size;
    std::vector<size_t> caller_indices;
    std::vector<size_t> callee_indices;
    int layer;
};

struct LabelIndexEntry {
    size_t line_index;
    uint8_t bank;
    uint32_t cpu_addr;
    std::string name;
    ApexRenderedBlockKind block_kind;
};

struct LineTargetEntry {
    size_t line_index;
    uint8_t bank;
    uint32_t cpu_addr;
    std::string name;
    size_t match_pos;
};

struct RefEntry {
    size_t line_index;
    uint8_t bank;
    uint32_t cpu_addr;
    std::string label;
    std::string kind;
    int row_index;       /* >= 0 for table row refs */
    uint32_t row_cpu_addr;
};

struct UiState {
    size_t selected_line;
    size_t selection_end;
    size_t editor_bound_line;
    int request_scroll_to_selection;
    int request_focus_goto;
    int request_focus_filter;
    int request_focus_label;
    int request_focus_doc;
    int request_save_overlay;
    int request_focus_new_bookmark;
    int suppress_history_push;
    bool show_help;
    char goto_input[64];
    char filter_input[128];
    char label_filter_input[128];
    char strings_filter_input[128];
    char edit_label_input[128];
    char edit_doc_input[1024];
    char save_path_input[512];
    char base_config_path[1024];
    char status_message[256];
    char global_search_input[128];
    bool show_search_window;
    std::vector<size_t> search_results;
    int request_focus_global_search;
    bool request_xref_popup;
    uint8_t xref_popup_bank;
    uint32_t xref_popup_addr;
    int edit_data_length;      /* byte count for bytes[N] data button */
    int edit_table_rows;       /* row count when edit_table_is_rows */
    int edit_table_is_rows;    /* 0 = counted, 1 = rows[N] */
    int edit_field_add_count;  /* repeat count used when clicking a field button */
    ApexEditField edit_inline_fields[APEX_MAX_EDIT_FIELDS];
    int edit_inline_count;
    bool edit_inline_flow_stop;
    ApexEditField edit_schema_fields[APEX_MAX_EDIT_FIELDS];
    int edit_schema_count;
    std::vector<size_t> history_back;
    std::vector<size_t> history_forward;
    int dmd_scrub_offset;

    size_t hex_selected_offset;
    size_t hex_anchor_offset;   /* range start — set on plain left-click */
    bool hex_has_range;         /* true when a multi-byte range is selected */
    bool hex_active;
    bool hex_is_edit_target;    /* hex view was the last view directly interacted with:
                                   classify/label edits target the hex byte, not the
                                   disassembly line start */
    bool hex_window_focused;
    int hex_request_follow;
    size_t hex_prev_selected_line;
    size_t hex_hover_off;       /* byte hovered last frame; drives the block highlight */
    int    hex_hover_valid;
    char hex_search_input[64];
    int request_focus_hex_search;

    bool show_flow_arrows = true;
    bool show_navigator;
    bool show_labels;
    bool show_banks;
    bool show_transitions;
    bool show_bookmarks;
    bool show_disasm;
    bool show_details;
    bool show_refs;
    bool show_dmd;
    bool show_call_graph;
    bool show_tables;
    bool show_hardware;
    bool show_edit;
    bool show_hex;
    bool request_layout_reset;

    std::vector<struct LabelIndexEntry> cached_labels;
    bool labels_valid;
    std::vector<Bookmark> bookmarks;

    std::vector<GraphNode> graph_nodes;
    int graph_root_idx;
    int graph_depth_in;
    int graph_depth_out;
    bool graph_needs_rebuild;

    bool show_rom_info;
    RomInfoState rom_info;
    bool show_match_window;
    MatchWindowState match_state;
    bool show_rom_compare;
    CompareWindowState compare_state;
    bool show_coverage;
    CoverageWindowState coverage_state;
    bool show_inline_list;
    bool show_entries_list;
    bool show_strings_list;
    bool show_types_editor;
    bool show_pattern_search;
    char pattern_search_input[128];
    std::vector<size_t> pattern_search_results;
    int request_focus_pattern_search;

    bool show_ram_refs;
    char ram_ref_input[32];
    std::vector<size_t> ram_ref_results;
    int request_focus_ram_refs;

    bool show_ref_exclusions;
    bool show_symbols_editor;
    char sym_edit_name[64];
    char sym_edit_value[16];
    int  sym_selected;       /* index into project->symbols, -1 = none */
    int  sym_usages_sel;
    const ApexRenderedDocument *sym_usages_doc;
    std::vector<size_t> sym_usages_cache;
    bool show_rom_map;
    bool show_dmd_list;
    bool show_sprite_list;
    bool show_sprite_gallery;
    bool show_code_candidates;
    ApexCodeCandidates code_candidates;
    bool code_candidates_stale;
    bool show_inline_candidates;
    ApexInlineCandidates inline_candidates;
    bool inline_candidates_stale;

    bool refs_pinned;
    uint8_t refs_pinned_bank;
    uint32_t refs_pinned_addr;

    bool graph_pinned;
    uint8_t graph_pinned_bank;
    uint32_t graph_pinned_addr;

    bool overlay_dirty;

    struct SpriteScanEntry {
        uint8_t bank;
        uint32_t cpu_addr;
        size_t rom_offset;
        uint8_t header_type, enc_type;
        uint8_t width, height;
        size_t consumed;
        bool classified;
    };
    std::vector<SpriteScanEntry> sprite_candidates;
    bool sprite_scan_done;
    int sprite_filter_min_w = 1, sprite_filter_max_w = 128;
    int sprite_filter_min_h = 1, sprite_filter_max_h = 32;
    int sprite_nh_height = 12; /* height for sprite_noheader classify button */
    int sprite_gallery_zoom = 1; /* Sprite Gallery pixel zoom: 1x / 2x / 4x */

    struct VsiTableEntry {
        int table_idx;      /* index in master table */
        int image_idx;      /* image number within sub-table */
        uint8_t table_height; /* height from sub-table descriptor */
        uint8_t bank;
        uint32_t cpu_addr;
        size_t rom_offset;
        bool is_noheader;   /* true = no-header VSI (byte 0 = width) */
        uint8_t width, height;
        bool classified;    /* already in data_ranges as DATA_SPRITE / DATA_SPRITE_NOHEADER */
    };
    struct VsiSubTableInfo {
        int table_idx;
        uint8_t bank;
        uint32_t cpu_addr;   /* sub-table start address */
        size_t header_len;   /* bytes before pointer array (min/max pairs + terminator + H + spacing) */
        int num_images;
        uint8_t table_height;
    };
    std::vector<VsiTableEntry> vsi_table_entries;
    std::vector<VsiSubTableInfo> vsi_sub_tables;
    bool vsi_table_scan_done = false;
};

struct SnapshotLabel {
    int has_bank;
    uint8_t bank;
    uint32_t addr;
    std::string name;
};

struct SnapshotEntry {
    int has_bank;
    uint8_t bank;
    uint32_t addr;
};

struct SnapshotData {
    uint8_t bank;
    uint32_t addr;
    std::string spec;
};

struct SnapshotTable {
    uint8_t bank;
    uint32_t addr;
    std::string spec;
};

struct SnapshotDoc {
    int has_bank;
    uint8_t bank;
    uint32_t addr;
    std::string text;
};

struct SnapshotInline {
    int has_bank;
    uint8_t bank;
    uint32_t addr;
    std::string spec;
};

struct SnapshotTypeValue {
    uint32_t value;
    std::string name;
};

struct SnapshotType {
    std::string name;
    int is_word; /* 0=byte, 1=word */
    std::vector<SnapshotTypeValue> values;
};

struct SnapshotSymbol {
    std::string name;
    uint32_t    value;
};

struct OriginalSnapshot {
    std::vector<SnapshotLabel> labels;
    std::vector<SnapshotEntry> entries;
    std::vector<SnapshotEntry> ref_exclusions;
    std::vector<SnapshotEntry> literals;
    std::vector<SnapshotData> data;
    std::vector<SnapshotTable> tables;
    std::vector<SnapshotDoc> docs;
    std::vector<SnapshotInline> inline_sigs;
    std::vector<SnapshotType> types;
    std::vector<SnapshotSymbol> symbols;
};

struct LineByteSpan {
    int valid;
    size_t start;
    size_t end;
};

struct DmdPreviewInfo {
    bool valid;
    bool from_target;
    uint8_t bank;
    uint32_t cpu_addr;
    size_t rom_offset;
    uint8_t decoder_type;
    size_t consumed;
    char title[128];
    uint8_t plane[APEX_DMD_PAGE_BYTES];
};

struct SpritePreviewInfo {
    bool valid;
    bool from_target;
    uint8_t bank;
    uint32_t cpu_addr;
    size_t rom_offset;
    uint8_t header_type;
    uint8_t enc_type;
    size_t consumed;
    uint8_t vert_offset;
    uint8_t horiz_offset;
    uint8_t width;
    uint8_t height;
    char title[128];
    uint8_t pixels[APEX_SPRITE_MAX_BYTES];   /* plane 0 */
    bool    two_plane;                       /* bicolor: pixels1 holds plane 1 */
    uint8_t pixels1[APEX_SPRITE_MAX_BYTES];  /* plane 1 (bicolor only) */
};

// --- Shared Helper Function Declarations ---

// Core State & Navigation
void select_line(UiState *state, size_t line_index, int request_scroll);
void handle_line_selection(UiState *state, size_t line_index, bool shift_held);
void history_jump(UiState *state, int backward);
void set_status(UiState *state, const char *message);
int selected_address(const ApexRenderedDocument *document, const UiState *state, uint8_t *bank, uint32_t *cpu_addr);
LineByteSpan selected_line_span(const ApexProject *project, const ApexRenderedDocument *document, const UiState *state);
int project_locate_rom_bytes(const ApexProject *project, uint8_t bank, uint32_t addr, const uint8_t **src, size_t *len, size_t *rom_offset);

// DMD logic
int address_is_dmd_fullframe_start(const ApexProject *project, uint8_t bank, uint32_t addr);
int decode_dmd_preview_at(const ApexProject *project, uint8_t bank, uint32_t addr, DmdPreviewInfo *preview);
DmdPreviewInfo find_dmd_preview(const ApexProject *project, const ApexRenderedDocument *document, UiState *state);

// Sprite logic
int address_is_sprite_start(const ApexProject *project, uint8_t bank, uint32_t addr);
int decode_sprite_preview_at(const ApexProject *project, uint8_t bank, uint32_t addr, SpritePreviewInfo *preview);
int decode_sprite_preview_with_height(const ApexProject *project, uint8_t bank, uint32_t addr, unsigned hint_height, SpritePreviewInfo *preview);
SpritePreviewInfo find_sprite_preview(const ApexProject *project, const ApexRenderedDocument *document, UiState *state);

// Inline spec string
std::string inline_sig_spec_string(const InlineSignature *s);

// Parsing & Utilities
int parse_target_address(const char *input, uint8_t *bank, uint32_t *cpu_addr);
int line_matches_filter(const ApexRenderedLine *line, const char *filter);
void run_global_search(const ApexRenderedDocument *document, const char *query,
                       std::vector<size_t> &results);
const char *block_name(ApexRenderedBlockKind kind);
const char *transition_name(ApexRenderedTransitionKind kind);
std::string line_to_string(const ApexRenderedLine *line);
std::string label_name(const ApexRenderedLine *line);
std::string table_def_spec_string(const TableDef *table);

// Field builder helpers
void fields_to_spec(char *buf, size_t cap, const ApexEditField *fields, int count);
void spec_to_fields(const char *spec, ApexEditField *fields, int *count, int max, const ApexProject *p);
void load_doc_editor_buffer(const ApexProject *project, UiState *state, uint8_t bank, uint32_t cpu_addr);

// Analysis: Labels & Pointers
void ensure_label_index(const ApexRenderedDocument *document, UiState *state);
int label_entry_matches_filter(const LabelIndexEntry &entry, const char *filter);
std::string label_at_address(const ApexRenderedDocument *document, UiState *state, uint8_t bank, uint32_t cpu_addr);
std::vector<LineTargetEntry> find_line_targets(const ApexRenderedDocument *document, UiState *state, const ApexRenderedLine *line);
int resolve_pointer_target(const ApexProject *project, const ApexRenderedLine *line, uint8_t *target_bank, uint32_t *target_addr, int *is_far);
int jump_to_first_line_target(const ApexRenderedDocument *document, UiState *state, const ApexRenderedLine *line);
void follow_selected_link(const ApexRenderedDocument *document, UiState *state);

// Analysis: XRefs
std::vector<RefEntry> find_incoming_refs(const ApexProject *project, const ApexRenderedDocument *document, UiState *state, uint8_t bank, uint32_t cpu_addr);
std::vector<RefEntry> find_outgoing_refs(const ApexProject *project, const ApexRenderedDocument *document, UiState *state, uint8_t bank, uint32_t cpu_addr);

// Analysis: Call Graph
int find_routine_start(const ApexRenderedDocument *document, uint8_t bank, uint32_t addr, size_t *out_line_index);
void rebuild_call_graph(ApexProject *project, const ApexRenderedDocument *document, UiState *state);

// Analysis: Misc
bool select_line_by_address(const ApexRenderedDocument *document, UiState *state);
void jump_to_transition(const ApexRenderedDocument *document, UiState *state, ApexRenderedTransitionKind kind, int forward);
void move_selection_relative(const ApexRenderedDocument *document, UiState *state, int delta);
void jump_primary_transition(const ApexRenderedDocument *document, UiState *state, int forward);
void sync_editor_state(const ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void auto_label_targets(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void rerender_and_reselect(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state, uint8_t bank, uint32_t cpu_addr);
void apply_code_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void apply_data_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state, const char *spec);
void apply_string_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void apply_string_lp_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void apply_table_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state, const char *spec);
void clear_kind_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
const ApexRenderedLine *find_first_line_in_bank(const ApexRenderedDocument *document, uint8_t bank, size_t *line_index);
int find_line_by_rom_offset(const ApexRenderedDocument *document, size_t rom_offset, size_t *line_index);
int rom_offset_to_cpu_address(const ApexProject *project, size_t offset, uint8_t *bank, uint32_t *cpu_addr);
ApexRenderedBlockKind get_offset_kind(const ApexProject *project, const ApexRenderedDocument *document, size_t offset);

// Data
const CpuHelpInfo *lookup_cpu_help(const char *mnemonic);
const HardwareRegister *lookup_hardware(uint32_t addr);
std::vector<const HardwareRegister*> find_hardware_in_text(const char *text, size_t length);

// Project & Snapshot
OriginalSnapshot build_original_snapshot(const ApexProject *project);
OriginalSnapshot build_config_snapshot(const char *config_path);
int write_delta_overlay(const ApexProject *project, const OriginalSnapshot *snapshot, const char *path, const char *include_path, std::string *status);
int write_full_config(ApexProject *project, const char *path, std::string *status);

// Session
void clear_session();
void save_session(const char *rom_path, const char *config_path, const UiState *state, const ApexRenderedDocument *document);
int load_global_session(char *rom_path, char *config_path);
void load_rom_session(const char *rom_path, UiState *state, const ApexRenderedDocument *document);

// Clipboard
void copy_selection_to_clipboard(const ApexRenderedDocument *document, const UiState *state);

struct HardwareAccess {
    const HardwareRegister *reg;
    std::vector<size_t> line_indices;
};

// --- UI Window Rendering ---
void render_line_table(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_label_list(const ApexRenderedDocument *document, UiState *state);
void render_bank_list(const ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_transition_list(const ApexRenderedDocument *document, UiState *state);
void render_xref_popup(ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_bookmark_list(const ApexRenderedDocument *document, UiState *state);
void render_global_search(const ApexRenderedDocument *document, UiState *state);
void render_hex_view(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_call_graph(ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_editor(ApexProject *project, const ApexRenderedDocument **document_ptr, const OriginalSnapshot *snapshot, UiState *state);
void render_dmd_view(const ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_tables_window(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_hardware_window(ApexProject *project, const ApexRenderedDocument *document, UiState *state);

// Analysis: Tables
void auto_search_tables(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);

// Analysis: Hardware
std::vector<HardwareAccess> find_hardware_accesses(const ApexProject *project, const ApexRenderedDocument *document);
size_t hardware_register_count();
const HardwareRegister *get_hardware_register(size_t index);

// ROM Info
void render_rom_info(const ApexProject *project, UiState *state);

// Match from Reference
void render_match_window(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_rom_compare_window(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_coverage_window(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);

// Analysis: Inline list, Entries list & Types editor
void render_inline_list(ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_entries_list(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_strings_list(ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_types_editor(ApexProject *project, UiState *state);
void render_symbols_editor(ApexProject *project, const ApexRenderedDocument *document, UiState *state);

// Analysis: Pattern Search & RAM XRefs
std::vector<size_t> search_hex_pattern(const ApexProject *project, const char *input);
std::vector<size_t> find_ram_refs(const ApexRenderedDocument *document, const char *addr_input);
void render_pattern_search(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_ram_refs(const ApexProject *project, const ApexRenderedDocument *document, UiState *state);

// Analysis: Ref Exclusions
void render_ref_exclusions(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_code_candidates(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_inline_candidates(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_rom_map(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);

// DMD and Sprite list windows
void render_dmd_list_window(const ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_sprite_list_window(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_sprite_gallery_window(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);

#endif
