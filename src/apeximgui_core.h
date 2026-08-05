#ifndef APEXIMGUI_CORE_H
#define APEXIMGUI_CORE_H

#include "imgui.h"
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_set>

extern "C" {
#include "apexdmd.h"
#include "apexsprite.h"
#include "apex_project.h"
#include "apex_render.h"
#include "apex_analysis.h"
#include "apex_match.h"
#include "apex_compare.h"
}

#include "apex_pinmame.h"  /* C++ client for the PinMAME remote debugger (dynamic analysis) */
#include "apex_imgexport.h" /* minimal PNG / animated-GIF writers (DMD export) */

// --- ROM Info State ---

struct RomInfoState {
    bool     computed;
    /* metadata */
    bool     os_valid;
    uint8_t  os_major;
    uint8_t  os_minor;
    uint32_t reset_addr;
    char     game_version[32];
    /* game identity (name / number / date via system-bank far pointers) */
    int      game_id_found;
    uint16_t game_id_ptr_addr;
    char     game_name[32];
    char     game_number[8];
    char     game_date[16];
    uint8_t  game_name_bank;    uint16_t game_name_addr;
    uint8_t  game_number_bank;  uint16_t game_number_addr;
    uint8_t  game_date_bank;    uint16_t game_date_addr;
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

/* One place where code flow breaks off: a code block whose last instruction does
   not transfer control (no RTS/JMP/BRA/... and not a flow-stop tail call) yet is
   immediately followed by a non-code block — the signature of mis-classified code
   or a wrong inline signature. */
struct FlowBreakEntry {
    size_t   line_index;   /* the last code instruction's line in the document */
    uint8_t  bank;
    uint32_t cpu_addr;
    std::string insn;      /* rendered text of that last instruction */
    std::string next;      /* what follows: "data", "unclassified", "fill", ... */
};

/* One "; WARNING ..." line scraped from the rendered document for the Warnings
   panel.  has_location is false for warnings whose bank/cpu could not be parsed. */
struct WarningEntry {
    size_t line_index;   /* index of the warning comment line in the document */
    std::string type;    /* e.g. "inline_far_code_invalid" */
    std::string detail;  /* message text after the bank/cpu/rom fields */
    bool has_location;
    bool acked;          /* scraped from a "; WARNING_ACK" line */
    uint8_t bank;
    uint32_t cpu_addr;
};

/* One candidate RAM location read from an nvram-maps JSON, shown in the import
   preview window (name, address, doc, whether it collides with existing state). */
struct NvramImportRow {
    std::string name;
    uint32_t    addr;
    std::string doc;
    bool        selected;
    bool        overwrites;   /* would replace an existing symbol/doc */
    std::string conflict;     /* human description of the collision, if any */
};

/* One 16-bit immediate-load instruction (LDX/LDD/LDU/… #imm16, CMPX/… #imm16)
   surfaced by the Immediate Loads panel so the user can jump to the instruction,
   jump to the value as a target address, and label that target. */
struct ImmLoadRef {
    size_t   line_index;  /* the instruction's document line (jump-to-instruction) */
    uint8_t  bank;        /* instruction bank */
    uint32_t cpu_addr;    /* instruction address */
    uint32_t imm;         /* the 16-bit immediate value */
    uint8_t  tgt_bank;    /* resolved target bank (valid only when tgt_in_rom) */
    int      tgt_in_rom;  /* immediate lands in ROM address space (paged or system) */
    int      symbolic;    /* the immediate currently renders as a label, not #0x.... */
    char     mnem[8];     /* mnemonic, e.g. "LDX" */
};

/* One entry in the change log (View > Change Log): a single recorded edit, fed by
   the ApexProject change listener.  Address fields are navigable when has_addr. */
struct ChangeLogEntry {
    unsigned long seq;
    std::string   action;
    bool          has_addr;
    uint8_t       bank;
    uint32_t      addr;
};

/* One contiguous run of executed bytes reported by PinMAME coverage, mapped to
   ApexII's (bank, cpu_addr) model.  is_code = the run start is currently
   classified as CODE; runs that are NOT code are the actionable "missed code"
   worklist (real execution proves they are code). */
struct PinmameRun {
    uint8_t  bank;
    uint32_t addr;
    uint32_t len;
    int      is_code;
};

/* One parsed op of the switch-control mini-script.  Grammar (one op per line):
     press <sw> | release <sw> | pulse <sw> [ms] | wait <ms> | resume | pause
   Frame-stepped so a `wait` never blocks the UI thread. */
enum PmScriptKind {
    PM_OP_PRESS, PM_OP_RELEASE, PM_OP_PULSE, PM_OP_WAIT, PM_OP_RESUME, PM_OP_PAUSE
};
struct PmScriptOp {
    int kind; /* PmScriptKind */
    int a;    /* switch number */
    int b;    /* ms (pulse/wait) */
};

/* One routine in the call-frequency hotlist (execution count from instrument). */
struct PmHotEntry {
    uint8_t     bank;
    uint32_t    addr;
    long        count;
    std::string name;
};

/* One live-watched RAM variable (value refreshed from the emulator each poll). */
struct PmWatch {
    uint32_t             addr;
    int                  size;   /* 1 or 2 bytes */
    std::string          name;   /* resolved NVRAM symbol, if any */
    std::vector<uint8_t> val;    /* last read */
};

/* State for the optional PinMAME dynamic-analysis integration: user config
   (persisted), the live process handle, and the last coverage import. */
struct PinmameState {
    char bin_path[512] = "/usr/local/bin/xpinmamed.x11"; /* config: xpinmamed binary */
    char rompath[512]  = "";     /* config: PinMAME rom dir (defaulted at startup) */
    char game[64]      = "";     /* config: romset name, e.g. "hurr_l2" */
    int  port          = 8080;   /* config: remote-debugger HTTP port */

    ApexPinmame     pm;          /* spawned child + port */
    bool            connected = false;
    bool            alive = false;     /* child process is running (set by the pump) */
    bool            launching = false; /* spawned, waiting for the HTTP server */

    /* live DMD framebuffer */
    bool                 show_dmd = false;
    std::vector<uint8_t> dmd_lum;
    int                  dmd_w = 0, dmd_h = 0;
    double               dmd_next = 0.0;
    /* animated-GIF recorder (one-frame lookahead for correct per-frame timing) */
    bool                 dmd_recording = false;
    ApexGif             *dmd_gif = nullptr;
    std::vector<uint8_t> dmd_pending;      /* last captured frame not yet written */
    double               dmd_pending_time = 0.0;
    bool                 dmd_has_pending = false;
    long                 dmd_rec_frames = 0;
    ApexPinmameInfo info;
    ApexPinmameCpu  cpu;
    double          next_poll = 0.0;

    long                    cov_executed = 0;
    long                    cov_addressable = 0;
    std::vector<PinmameRun> cov_runs;
    bool                    cov_only_noncode = true; /* worklist filter */
    std::unordered_set<uint32_t> cov_reached; /* (bank<<16)|addr, for the disasm overlay */
    bool                    cov_overlay = false;      /* tint executed/dead code in the disasm */
    std::string             status;

    /* breakpoints / watchpoints / halt */
    std::vector<ApexPinmamePoint>  points;
    std::vector<ApexPinmameSwitch> switches;
    bool   resumed = false;   /* we issued resume; watch for a breakpoint halt */
    int    wp_mode = 2;       /* default watchpoint mode: write */
    ApexPinmameHalt last_halt;      /* last SSE halt event (which bp/wp fired) */
    unsigned long   last_halt_seq = 0;
    char   bp_cond[64] = "";        /* optional condition for new breakpoints (e.g. A==7F) */
    std::map<uint32_t, long> wp_hits; /* watchpoint hit counts (API gives none), keyed by addr */

    /* call-frequency hotlist (via instrument counters) */
    std::vector<PmHotEntry> hotlist;
    int    hot_instrumented = 0;

    /* dynamic RAM xref: collect the code sites that access a variable */
    char   xref_addr[16] = "";
    int    xref_mode = 3;             /* 1=r 2=w 3=rw */
    bool   xref_collecting = false;
    std::vector<PmHotEntry> xref_accessors;

    /* execution backtrace (recent-instruction ring, fetched on halt) */
    bool   trace_enabled = false;
    std::vector<ApexPinmameTrace> trace;

    /* live RAM watch */
    std::vector<PmWatch> watches;
    char   watch_addr[16] = "";
    int    watch_size = 0;   /* combo index: 0 = 1 byte, 1 = 2 bytes */

    /* heuristic call stack (return addresses found on the S stack at a halt) */
    std::vector<PmHotEntry> callstack;

    /* computed-jump resolver: break at an indexed JMP/JSR, step, record the real
       target across hits */
    char     jmp_addr[16] = "";
    bool     jmp_collecting = false;
    uint32_t jmp_pc = 0;      /* the jump instruction's address */
    int      jmp_bank = -1;   /* its bank (system = -1) */
    std::vector<PmHotEntry> jmp_targets;

    /* switch-control mini-script (frame-stepped, no UI blocking) */
    char   script[4096] = "";
    std::vector<PmScriptOp> script_ops;
    size_t script_ip = 0;
    bool   script_running = false;
    double script_next = 0.0;
    std::string script_status;
};

/* Which classification action the "repeat last classification" hotkey replays. */
enum ApexLastClassifyOp {
    APEX_LAST_CLASSIFY_NONE = 0,
    APEX_LAST_CLASSIFY_DATA,       /* apply_data_at_selection(last_classify_spec) */
    APEX_LAST_CLASSIFY_STRING,     /* apply_string_at_selection */
    APEX_LAST_CLASSIFY_TABLE,      /* apply_table_at_selection(last_classify_spec) */
    APEX_LAST_CLASSIFY_CODE,       /* apply_code_at_selection */
    APEX_LAST_CLASSIFY_CLEAR,      /* clear_kind_at_selection */
};

struct UiState {
    size_t selected_line;
    /* Explicit multi-line block (for copy / range ops), decoupled from the
       cursor: plain clicks/navigation move the cursor only and never disturb it.
       Set via shift-click or the A/E hotkeys, cleared with Esc. */
    bool   block_active = false;
    size_t block_a = 0;
    size_t block_b = 0;
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

    /* Inline label rename: double-click a label in the disassembly to edit it in
       place. inline_edit_line is the document line being edited (or (size_t)-1). */
    size_t   inline_edit_line  = (size_t)-1;
    bool     inline_edit_focus = false;
    uint8_t  inline_edit_bank  = 0;
    uint32_t inline_edit_addr  = 0;
    char     inline_edit_buf[128] = "";

    /* Command palette (Ctrl+P): jump to any label or address by typing. */
    bool request_command_palette = false;
    bool cmd_palette_focus       = false;
    bool cmd_palette_scroll      = false;
    int  cmd_palette_sel         = 0;
    char cmd_palette_input[128]  = "";
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

    size_t hex_selected_offset; /* the cursor byte (moves freely; never disturbs the block) */
    size_t hex_anchor_offset;   /* block start */
    size_t hex_block_end;       /* block end — decoupled from the cursor */
    bool hex_has_range;         /* true when a byte block is marked (a..end) */
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
    bool hex_search_bank_only;  /* restrict byte search to the cursor's current bank */
    bool hex_search_ascii = false; /* interpret the query as ASCII text, not hex bytes */
    bool hex_search_ci    = false; /* case-insensitive (ASCII mode only) */

    /* Last classification action, replayed by the "repeat" hotkey (1) so a run of
       individual classifications reduces to n,1,n,1,…  See repeat_last_classify(). */
    int  last_classify_op = APEX_LAST_CLASSIFY_NONE;  /* ApexLastClassifyOp */
    std::string last_classify_spec;                   /* spec for DATA/TABLE ops (unbounded: table row formats can be long) */

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

    /* render_line_table's filtered row list, cached across frames — rebuilt only
       when the document changes (generation) or the filter text changes. */
    std::vector<size_t> cached_visible;
    unsigned long       cached_visible_gen = (unsigned long)-1;
    std::string         cached_visible_filter;

    /* project->refs indices sorted by (source_bank, source_addr) so
       find_outgoing_refs is O(log n) instead of a full scan every frame.
       Rebuilt when the document generation changes (i.e. after a re-analysis). */
    std::vector<uint32_t> cached_out_ref_order;
    unsigned long         cached_out_ref_gen = (unsigned long)-1;

    /* Transitions / Strings list rows, cached on (generation, filter) so the
       O(line_count) scan runs only when the document or filter changes. */
    std::vector<size_t> cached_transitions;
    unsigned long       cached_transitions_gen = (unsigned long)-1;
    std::string         cached_transitions_filter;
    std::vector<size_t> cached_strings_rows;
    unsigned long       cached_strings_gen = (unsigned long)-1;
    std::string         cached_strings_filter;

    /* Full per-ROM-byte extended block-kind map for the Hex view, cached on the
       document generation so the O(line_count) forward-fill runs once per edit
       instead of every frame. */
    std::vector<uint8_t> cached_hex_kinds;
    unsigned long        cached_hex_kinds_gen = (unsigned long)-1;

    /* Async re-render: an edit sets async_pending; the main loop launches the
       render on a worker thread and shows a busy overlay (rendering=true) until
       it finishes, then reselects (async_b, async_a). */
    bool    async_pending = false;
    bool    rendering     = false;
    uint8_t async_b       = 0;
    uint32_t async_a      = 0;
    /* Dock tab that was active when the render started; refocused afterwards so
       hiding the panels behind the busy overlay doesn't reset the selected tab. */
    std::string restore_focus;

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

    bool show_imm_loads;
    std::vector<ImmLoadRef> imm_load_results;
    bool imm_loads_scanned;
    bool imm_only_unlabeled;  /* filter: hide immediates already resolved to a label */
    bool imm_only_rom;        /* filter: only immediates whose value lands in ROM space */

    bool show_change_log;
    std::vector<ChangeLogEntry> change_log; /* fed by the ApexProject change listener */

    bool show_pinmame;
    bool show_pinmame_dmd;
    bool show_pinmame_switches;
    bool show_pinmame_coverage;
    PinmameState pinmame;

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
    bool show_warnings;
    std::vector<WarningEntry> warnings;
    bool warnings_stale;
    bool show_flow_breaks;
    std::vector<FlowBreakEntry> flow_breaks;
    bool flow_breaks_stale;
    /* "Create type from a text table's strings" popup state. */
    bool     tt_type_request = false;
    uint8_t  tt_type_bank = 0;
    uint32_t tt_type_addr = 0;
    int      tt_type_word = 0;
    char     tt_type_name[64] = "";
    bool show_nvram_import;
    std::vector<NvramImportRow> nvram_import_rows;
    char nvram_source_path[512];  /* last imported nvram JSON — reused as export template */

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

    /* Reviewable table-search candidates (suggest-then-accept, like sprites). */
    struct TableCandidate {
        uint8_t  bank;
        uint32_t cpu_addr;
        std::string spec;   /* the [tables] spec to apply, e.g. rows[42](far_dmd_fullframe) */
        int      rows;      /* row count (for display/sorting) */
        std::string kind;   /* short label: "far_dmd", "far_ptr", "text" */
        bool     already;   /* address already classified as a table */
    };
    std::vector<TableCandidate> table_candidates;
    bool table_scan_done = false;
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
    uint8_t value;      /* aux byte (far_imm target bank); 0 for valueless sections */
    uint8_t value2;     /* far_imm: FarImmType */
    uint32_t aux_addr;  /* far_imm: paired bank-load instruction address */
    SnapshotEntry() : has_bank(0), bank(0), addr(0), value(0), value2(0), aux_addr(0) {}
    SnapshotEntry(int hb, uint8_t b, uint32_t a, uint8_t v = 0, uint8_t v2 = 0, uint32_t ax = 0)
        : has_bank(hb), bank(b), addr(a), value(v), value2(v2), aux_addr(ax) {}
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
    std::vector<SnapshotEntry> ack_warnings;
    std::vector<SnapshotEntry> far_imms;
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
void render_command_palette(const ApexRenderedDocument **document_ptr, UiState *state);
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
void finish_async_rerender(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void apply_code_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void apply_data_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state, const char *spec);
void apply_string_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void apply_table_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state, const char *spec);
void clear_kind_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void repeat_last_classify(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
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
void copy_hex_block_to_clipboard(const ApexProject *project, const UiState *state);

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
void scan_table_candidates(ApexProject *project, UiState *state);

// Analysis: Hardware
std::vector<HardwareAccess> find_hardware_accesses(const ApexProject *project, const ApexRenderedDocument *document);
size_t hardware_register_count();
const HardwareRegister *get_hardware_register(size_t index);

// ROM Info
void render_rom_info(ApexProject *project, const ApexRenderedDocument *document, UiState *state);

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

// Analysis: Immediate Loads (LDX/LDD/… #imm16 that may reference an address)
std::vector<ImmLoadRef> find_imm_loads(const ApexProject *project, const ApexRenderedDocument *document);
void render_imm_loads(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);

// Change Log: audit trail of every recorded edit, fed by the project change listener
void ui_change_listener(void *ctx, const ApexChangeEvent *ev);
void render_change_log(const ApexRenderedDocument *document, UiState *state);

// PinMAME: optional dynamic-analysis integration (spawn xpinmamed, import coverage)
// pinmame_pump runs the per-frame work (poll, halt, script, DMD fetch) once from the
// main loop, so the live views stay updated regardless of which window is visible.
void pinmame_pump(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_pinmame(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_pinmame_dmd(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_pinmame_switches(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_pinmame_coverage(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void pinmame_stop_gif(UiState *state); /* finalise an in-progress DMD GIF recording */

// Analysis: Ref Exclusions
void render_ref_exclusions(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_code_candidates(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_inline_candidates(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_warnings_view(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_flow_breaks_view(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
/* From a line inside a TABLE block, find the table's start (header) address.
   Returns false if the line is not part of a table. */
bool find_table_start(const ApexRenderedDocument *d, size_t line_idx,
                      uint8_t *out_bank, uint32_t *out_addr);
void render_nvram_import_window(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
/* Populate state->nvram_import_rows from parsed nvram locations, flagging
   collisions against the project's current symbols/docs. Returns 0 on success. */
int nvram_prepare_import(ApexProject *project, UiState *state, const char *json_path,
                        std::string *err);
/* Write the project's RAM symbols/docs to an nvram-maps JSON file. When
   template_path is non-empty and readable, merge into it losslessly (only
   names/docs updated); otherwise write a fresh minimal map. */
int nvram_export(const ApexProject *project, const char *json_path,
                 const char *template_path, std::string *err);
void render_rom_map(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);

// DMD and Sprite list windows
void render_dmd_list_window(const ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_sprite_list_window(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
void render_sprite_gallery_window(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);

#endif
