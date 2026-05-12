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
#include "apex_project.h"
#include "apex_render.h"
#include "apex_analysis.h"
}

// --- Constants and Enums ---

enum {
    EDIT_DATA_CUSTOM = 0,
    EDIT_DATA_BYTES,
    EDIT_DATA_STRING,
    EDIT_DATA_FAR_STRING,
    EDIT_DATA_FAR_DATA,
    EDIT_DATA_FAR_TABLE,
    EDIT_DATA_FAR_CODE
};

enum {
    EDIT_TABLE_CUSTOM = 0,
    EDIT_TABLE_COUNTED,
    EDIT_TABLE_ROWS
};

enum {
    EDIT_SCHEMA_CUSTOM = 0,
    EDIT_SCHEMA_PTR16_DATA,
    EDIT_SCHEMA_PTR16_CODE,
    EDIT_SCHEMA_PTR16_STRING,
    EDIT_SCHEMA_FAR_DATA,
    EDIT_SCHEMA_FAR_CODE,
    EDIT_SCHEMA_FAR_STRING
};

enum {
    EDIT_INLINE_CUSTOM = 0,
    EDIT_INLINE_BYTE,
    EDIT_INLINE_PTR16_DATA,
    EDIT_INLINE_PTR16_CODE,
    EDIT_INLINE_PTR16_STRING,
    EDIT_INLINE_FAR_DATA,
    EDIT_INLINE_FAR_CODE,
    EDIT_INLINE_FAR_STRING
};

enum {
    EDIT_DOC_ROUTINE = 0,
    EDIT_DOC_TABLE
};

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
    char edit_label_input[128];
    char edit_spec_input[128];
    char edit_inline_input[128];
    char edit_inline_name_input[64];
    char edit_doc_input[1024];
    char edit_table_schema_input[128];
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
    int edit_data_mode;
    int edit_data_length;
    int edit_table_mode;
    int edit_table_rows;
    int edit_table_schema_mode;
    int edit_inline_mode;
    int edit_doc_mode;
    std::vector<size_t> history_back;
    std::vector<size_t> history_forward;
    int dmd_scrub_offset;

    size_t hex_selected_offset;
    bool hex_active;
    int hex_request_follow;
    size_t hex_prev_selected_line;

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

    bool show_pattern_search;
    char pattern_search_input[128];
    std::vector<size_t> pattern_search_results;
    int request_focus_pattern_search;

    bool show_ram_refs;
    char ram_ref_input[32];
    std::vector<size_t> ram_ref_results;
    int request_focus_ram_refs;

    bool refs_pinned;
    uint8_t refs_pinned_bank;
    uint32_t refs_pinned_addr;

    bool overlay_dirty;
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

struct OriginalSnapshot {
    std::vector<SnapshotLabel> labels;
    std::vector<SnapshotEntry> entries;
    std::vector<SnapshotData> data;
    std::vector<SnapshotTable> tables;
    std::vector<SnapshotDoc> routine_docs;
    std::vector<SnapshotDoc> table_docs;
    std::vector<SnapshotInline> inline_sigs;
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

// Parsing & Utilities
int parse_target_address(const char *input, uint8_t *bank, uint32_t *cpu_addr);
int line_matches_filter(const ApexRenderedLine *line, const char *filter);
const char *block_name(ApexRenderedBlockKind kind);
const char *transition_name(ApexRenderedTransitionKind kind);
std::string line_to_string(const ApexRenderedLine *line);
std::string label_name(const ApexRenderedLine *line);
std::string table_def_spec_string(const TableDef *table);

// Presets & Sync
void apply_data_preset(UiState *state);
void apply_inline_preset(UiState *state);
void apply_table_preset(UiState *state);
void sync_spec_presets(UiState *state, const char *spec);
void sync_inline_presets(UiState *state, const char *spec);
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

// Analysis: Pattern Search & RAM XRefs
std::vector<size_t> search_hex_pattern(const ApexProject *project, const char *input);
std::vector<size_t> find_ram_refs(const ApexRenderedDocument *document, const char *addr_input);
void render_pattern_search(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_ram_refs(const ApexProject *project, const ApexRenderedDocument *document, UiState *state);

#endif
