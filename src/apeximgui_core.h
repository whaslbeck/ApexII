#ifndef APEXIMGUI_CORE_H
#define APEXIMGUI_CORE_H

#include "imgui.h"

#include <map>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include "apex_adjnames.h"
#include "apex_analysis.h"
#include "apex_args.h"
#include "apex_bits.h"
#include "apex_card.h"
#include "apex_compare.h"
#include "apex_inline_detect.h"
#include "apex_match.h"
#include "apex_patch.h"
#include "apex_project.h"
#include "apex_ramfind.h"
#include "apex_render.h"
#include "apex_snippet.h"
#include "apex_wpc_hooks.h"
#include "apexdmd.h"
#include "apexsprite.h"
}

#include "apex_bg_job.h"    /* worker thread for slow UI actions */
#include "apex_imgexport.h" /* minimal PNG / animated-GIF writers (DMD export) */
#include "apex_pinmame.h"   /* C++ client for the PinMAME remote debugger (dynamic analysis) */

// --- ROM Info State ---

struct RomInfoState {
    bool computed;
    /* metadata */
    bool os_valid;
    uint8_t os_major;
    uint8_t os_minor;
    uint32_t reset_addr;
    char game_version[32];
    /* game identity (name / number / date via system-bank far pointers) */
    int game_id_found;
    uint16_t game_id_ptr_addr;
    char game_name[32];
    char game_number[8];
    char game_date[16];
    uint8_t game_name_bank;
    uint16_t game_name_addr;
    uint8_t game_number_bank;
    uint16_t game_number_addr;
    uint8_t game_date_bank;
    uint16_t game_date_addr;
    uint16_t stored_csum;
    uint16_t computed_csum;
    uint16_t stored_delta;
    /* hashes */
    uint32_t crc32_val;
    uint8_t sha1[20];
    uint8_t sha256[32];
};

// --- Match Window State ---

struct MatchWindowState {
    char ref_rom_path[1024];
    char ref_ini_path[1024];
    bool scan_enabled;
    int min_confidence;
    bool has_results;
    std::string run_status;
    ApexProject *src_project;

    struct Result {
        std::string label_name;
        uint32_t src_addr;
        uint8_t src_bank;
        uint32_t dst_addr;
        uint8_t dst_bank;
        int confidence;
        bool accepted;
    };
    std::vector<Result> results;
    char filter[128];
    int show_mode; /* 0=all, 1=pending, 2=accepted */

    /* "Run Match" loads and analyses both ROMs on a worker; it only touches its
       own projects, never the live one.  pending_* hold its output until the UI
       thread adopts it. */
    std::vector<Result> pending_results;
    ApexProject *pending_src = nullptr;
    std::string pending_status;
    BackgroundJob job; /* declared after its outputs */

    ~MatchWindowState() {
        job.cancel();
        job.wait();
        if (pending_src) {
            apex_project_free(pending_src);
            pending_src = nullptr;
        }
        if (src_project) {
            apex_project_free(src_project);
            src_project = nullptr;
        }
    }
};

struct CompareWindowState {
    char rom_b_path[1024] = {0};
    char ini_b_path[1024] = {0};
    int min_instrs = 5;
    bool inc_code = true;
    bool inc_strings = true;
    bool inc_tables = true;
    bool inject_paged = false;
    bool has_results = false;
    std::string run_status;
    ApexProject *a_project = nullptr; /* clone of the live project at run time */
    ApexProject *b_project = nullptr;
    ApexFingerprintDB *a_db = nullptr; /* snapshot of A at run time */
    ApexFingerprintDB *b_db = nullptr;
    std::vector<ApexCompareEntry> results;
    size_t n_identical = 0, n_moved = 0, n_changed = 0, n_removed = 0, n_added = 0;
    char filter[128] = {0};
    bool show_identical = false;
    bool show_moved = true;
    bool show_changed = true;
    bool show_removed = true;
    bool show_added = true;

    /* The worker (see compare_run) fills these; adopted when it finishes. */
    struct Pending {
        ApexProject *a_project = nullptr, *b_project = nullptr;
        ApexFingerprintDB *a_db = nullptr, *b_db = nullptr;
        std::vector<ApexCompareEntry> results;
        size_t n_identical = 0, n_moved = 0, n_changed = 0, n_removed = 0, n_added = 0;
        std::string status;
        bool ok = false;
    } pending;
    BackgroundJob job; /* declared after its outputs */

    static void free_parts(ApexProject *&a, ApexProject *&b, ApexFingerprintDB *&adb,
                           ApexFingerprintDB *&bdb) {
        if (bdb)
            apex_fingerprint_free(bdb);
        if (adb)
            apex_fingerprint_free(adb);
        if (b)
            apex_project_free(b);
        if (a)
            apex_project_free(a);
        a = b = nullptr;
        adb = bdb = nullptr;
    }
    void reset() {
        free_parts(a_project, b_project, a_db, b_db);
        results.clear();
        has_results = false;
    }
    ~CompareWindowState() {
        job.cancel();
        job.wait();
        free_parts(pending.a_project, pending.b_project, pending.a_db, pending.b_db);
        reset();
    }
};

/* Game Hooks window: the WPC game-hook table (see apex_wpc_hooks.h) resolved
   against the current labels. */
struct HookCallerRow {
    uint8_t bank;
    uint32_t addr;
    int kind; /* ApexHookCallKind */
    std::string routine;
};
struct HookRow {
    size_t slot;
    uint32_t slot_addr;
    uint8_t target_bank;
    uint32_t target_addr;
    bool stub;
    int shared;
    std::string slot_name;   /* label at the slot ("" if none / generated) */
    std::string target_name; /* label at the target ("" if none) */
    std::vector<HookCallerRow> callers;
};
struct HooksWindowState {
    const void *doc_ptr = nullptr; /* staleness check against the rendered doc */
    bool computed = false;
    std::vector<HookRow> rows;
    std::vector<uint32_t> helpers;
    bool hide_stubs = true;
    int run_filter = 0; /* 0 = all, 1 = target executed, 2 = new since coverage baseline */
    char filter[64] = "";
    int selected = -1; /* index into rows */
    char name_buf[96] = "";
};

/* Patch window (apeximgui_patch.cpp): the open .patch.ini, its last build and
   the editor buffers of the selected op. */
struct PatchRange {
    size_t off; /* ROM file offset */
    size_t len;
    int op; /* index into the patch's ops */
};
struct PatchUiState {
    char path[1024] = ""; /* the .patch.ini ("" = none open) */
    bool loaded = false;
    bool dirty = false;
    ApexPatch patch{};
    ApexPatchBuild build{};
    bool built = false;
    std::vector<PatchRange> ranges; /* patched bytes of the last build, by offset */
    std::vector<uint8_t> orig_rom;  /* original image (the project ROM before any swap) */
    bool patched_view = false;      /* the project currently holds the patched bytes */
    bool view_sync = false;         /* project bytes must be re-synced to the view */
    bool open_pending = false;      /* session named a patch: open it once analysed */
    int selected = -1;
    int editor_for = -1; /* op whose fields are in the buffers below */
    char ed_name[64] = "";
    char ed_at[128] = "";
    char ed_expect[512] = "";
    char ed_bytes[512] = "";
    char ed_code[16384] = "";
    char ed_file[512] = "";
    char ed_note[256] = "";
    int ed_count = 0;
    int ed_length = 0;
    char ed_output[512] = "";
    char ed_alloc[64] = "";
    char ed_placed[32] = "";
    char ed_target[128] = "";
    char ed_label[64] = "";
    int ed_width = 3;
    char ed_value[128] = "";
    char ed_vtype[64] = "";
    char ed_text[512] = "";
    int ed_mode = 0;               /* string: auto, inplace, shift, relocate;
                                      dmd: auto, inplace, relocate */
    int ed_planes = 0;             /* dmd: 0 = from the analysis */
    int add_type = APEX_PATCH_ASM; /* type of the next "Add op" */
    /* free space of the original's analysis (a patched view's would see the
       patch code's labels); refreshed on every build in the original view */
    ApexFreeSpace detected{};
    bool detected_ok = false;
    bool show_map = false;
    /* undo: the patch as text before each change (apex_patch_to_text);
       `snap` is the current state, edits within a second are one step */
    std::vector<std::string> undo, redo;
    std::string snap;
    std::string saved; /* the text last loaded / saved: undoing back to it is clean */
    double snap_time = 0.0;
    bool focused = false; /* the Patch window has focus: Ctrl+Z undoes patch edits */
    /* snippet library (apex_snippet.h), loaded when the dialog first opens */
    std::string exe_dir;
    ApexSnippetList snippets{};
    bool snippets_loaded = false;
    bool snippet_open = false;
    int snippet_sel = 0;
    char snippet_vals[8][128] = {};
    char snippet_at[160] = "";
    char snippet_alloc[64] = "";
    char snippet_name[64] = "";
    /* "Check run": executed / code bytes per op, from PinMAME's coverage of a
       patched-ROM test (-1 = no code to check); cleared by every rebuild */
    std::vector<int> cov_hit, cov_total;
    /* data ops: a read trace on the first byte the game reads (set up by Test
       in PinMAME, polled while it runs); per op: traced, read, reader PCs */
    struct DataTrace {
        int op;
        uint8_t bank;
        uint32_t addr;
        bool read;
        std::vector<long> pcs;
    };
    std::vector<DataTrace> data_traces;
    bool data_trace_pending = false;
    bool data_tracing = false;
    double data_next = 0.0;
    /* last rebase (Export > Rebase...): the new patch and how each op went */
    bool rebase_open = false;
    std::string rebase_out, rebase_labels, rebase_rom, rebase_ini;
    std::vector<ApexRebaseOp> rebase_ops;
    std::vector<std::string> rebase_names;
    int rebase_failed = 0;
    /* free-RAM finder (apex_ramfind.h): static map + RAM snapshots */
    bool show_ram = false;
    bool ram_ready = false;
    bool ram_sampling = false;
    double ram_next = 0.0;
    int ram_min = 8;
    ApexRamMap ram{};
};

/* Argument Values window: typed argument sites (apex_args.h) grouped by
   type and value. */
struct ArgValueRow {
    uint32_t value;
    std::string name;          /* enum name ("" if the value has none) */
    std::vector<size_t> sites; /* indices into ArgsWindowState.sites */
};
struct ArgSiteRow {
    uint8_t bank;
    uint32_t value_addr;
    uint32_t call_addr;
    int reg;
    std::string type_name;
    uint32_t value;
    std::string callee;
    std::string caller;
};
/* Bit Map window: lamps and flags by matrix and bit (apex_bits.h). */
struct BitsWindowState {
    const void *doc_ptr = nullptr; /* staleness check against the rendered doc */
    bool computed = false;
    ApexBitMap map{};
    std::vector<std::string> path_names; /* label of each bit routine */
    int matrix = -1;                     /* selected B code, -1 = first */
    int selected = -1;                   /* first site index of the selected bit */
    char filter[64] = "";
};

/* Routine Card window (apex_card.h): the routine at the cursor, or a pinned one. */
struct CardWindowState {
    bool pinned = false;
    uint8_t bank = 0;
    uint32_t addr = 0;
    bool valid = false;
    const void *doc_ptr = nullptr;
    ApexRoutineCard card{};
    char name_buf[96] = "";
    uint32_t name_for = 0xffffffffu; /* (bank<<16)|addr the buffer belongs to */
};

/* Event Timeline window: switches, lamp / flag operations and watched routines
   recorded from a running PinMAME, in time order. */
struct TimelineEvent {
    double t;      /* seconds since the recording started */
    int kind;      /* 0 switch, 1 bit operation, 2 watched routine, 3 note */
    int path = -1; /* bit operation: index into the bit map's paths */
    int matrix = 0, bit = 0, on = 0;
    uint8_t bank = 0xff; /* code to jump to */
    uint32_t addr = 0;
    std::string text;
};
struct TimelineWatch {
    uint8_t bank;
    uint32_t addr;
    std::string name;
};
struct TimelineState {
    bool recording = false;
    bool want_switches = true, want_bits = true, want_tests = false;
    double t0 = 0.0, next_poll = 0.0;
    long hits_seen = 0;
    std::vector<int> sw_state; /* last switch states (index = switch number) */
    std::vector<TimelineWatch> watches;
    /* tracepoint index -> what it records: -1 - k = bit path k, else watch k */
    std::vector<int> tp_role;
    std::vector<uint32_t> tp_addr;
    std::vector<TimelineEvent> events;
    char filter[64] = "";
    bool follow = true;
    bool points_dirty = false;  /* re-install the tracepoints (watch list changed) */
    bool hide_matrix[256] = {}; /* display filter: bit events of these matrices */
};

struct ArgsWindowState {
    const void *doc_ptr = nullptr; /* staleness check against the rendered doc */
    bool computed = false;
    std::vector<ArgSiteRow> sites;
    std::vector<std::string> type_names; /* types with at least one site */
    int type_idx = 0;
    std::vector<ArgValueRow> values; /* of the selected type */
    int values_for = -1;             /* type_idx the values were built for */
    int selected = -1;               /* index into values */
    char name_buf[96] = "";
    int decl_reg = 0;  /* @args form: register (A,B,D,X,Y,U) */
    int decl_type = 0; /* @args form: index into the project's [types] */
};

struct CoverageWindowState {
    bool computed = false;
    const void *doc_ptr = nullptr; /* staleness check against the rendered doc */
    size_t rom_size = 0;
    size_t totals[7] = {0, 0, 0, 0, 0, 0, 0}; /* indexed by ApexRenderedBlockKind */
    bool include_unknown = false;             /* also list never-reached UNKNOWN runs */
    int min_gap = 2;                          /* ignore gaps shorter than this */
    unsigned gaps_ver = 0;                    /* bumped whenever gaps is rebuilt */
    struct Gap {
        size_t off;
        size_t len;
        uint8_t bank;
        uint32_t addr;
        int unknown;
    };
    std::vector<Gap> gaps;
    int next_gap = 0; /* cursor for "jump to next gap" */
};

// --- Keyboard shortcuts (apeximgui_keys.cpp) ---
enum KeyAction {
    KEY_MOVE_DOWN,
    KEY_MOVE_UP,
    KEY_PAGE_DOWN,
    KEY_PAGE_UP,
    KEY_NEXT_BLOCK,
    KEY_PREV_BLOCK,
    KEY_FOLLOW,
    KEY_HISTORY_BACK,
    KEY_HISTORY_FORWARD,
    KEY_GOTO,
    KEY_COMMAND_PALETTE,
    KEY_FOCUS_FILTER,
    KEY_TABLE_HEADER,
    KEY_MARK_CODE,
    KEY_MARK_DATA,
    KEY_MARK_STRING,
    KEY_MARK_TABLE,
    KEY_REPEAT_CLASSIFY,
    KEY_CLEAR_KIND,
    KEY_EDIT_LABEL,
    KEY_EDIT_DOC,
    KEY_BOOKMARK,
    KEY_UNDO,
    KEY_REDO,
    KEY_SAVE,
    KEY_BLOCK_START,
    KEY_BLOCK_END,
    KEY_BLOCK_CLEAR,
    KEY_COPY,
    KEY_XREFS,
    KEY_HELP,
    KEY_GLOBAL_SEARCH,
    KEY_REANALYZE,
    KEY_FULL_REANALYZE,
    KEY_ZOOM_IN,
    KEY_ZOOM_OUT,
    KEY_ZOOM_RESET,
    KEY_DMD_NEXT,
    KEY_DMD_PREV,
    KEY_DMD_NEXT32,
    KEY_DMD_PREV32,
    KEY_DMD_RESET,
    KEY_DMD_MARK,
    KEY_MOVE_LEFT,  /* hex editor: one byte back */
    KEY_MOVE_RIGHT, /* hex editor: one byte on */
    KEY_ACTION_COUNT
};
#define KEY_BINDINGS_PER_ACTION 2

/* True when one of the action's chords was pressed this frame (exact
   modifiers); always false while the editor captures a new binding. */
/* A width designed for the default 13 px font, scaled to the current font
   size (the View zoom and the monitor's DPI): fixed table columns and input
   fields grow with the text instead of cutting it off. */
inline float ui_w(float px) {
    return px * ImGui::GetFontSize() / 13.0f;
}

bool key_action_pressed(KeyAction a);
/* Centre an ImGuiFileDialog the first time it shows (call right before its
   Display(); a closed dialog leaves the next window alone). */
void center_file_dialog(const char *key);
/* Current bindings as menu hint text, e.g. "Ctrl+Y / Ctrl+Shift+Z". */
const char *key_hint(KeyAction a);
const char *key_action_id(KeyAction a);
const char *key_action_desc(KeyAction a);
ImGuiKeyChord key_binding(KeyAction a, int slot);
void key_set_binding(KeyAction a, int slot, ImGuiKeyChord chord);
void key_reset_action(KeyAction a);
bool key_capturing();
/* imgui.ini persistence ([ApexII][Keys]: bindings that differ from default). */
void keys_read_ini_line(const char *line);
void keys_write_ini(ImGuiTextBuffer *buf);
/* Shortcut editor (body of the Keyboard Shortcuts window). */
void render_keys_editor();

// --- Constants and Enums ---

#define APEX_MAX_EDIT_FIELDS 12

/* A single field in an inline or table-schema field list. */
typedef struct {
    int kind;           /* TableFieldKind cast to int, or -1 for a named type */
    int count;          /* repeat count (>= 1) */
    char type_name[32]; /* valid when kind == -1 */
    int param;          /* *_sprite fields: no-header image height; 0 = none */
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
    int row_index; /* >= 0 for table row refs */
    uint32_t row_cpu_addr;
};

/* One place where code flow breaks off: a code block whose last instruction does
   not transfer control (no RTS/JMP/BRA/... and not a flow-stop tail call) yet is
   immediately followed by a non-code block — the signature of mis-classified code
   or a wrong inline signature. */
struct FlowBreakEntry {
    size_t line_index; /* the last code instruction's line in the document */
    uint8_t bank;
    uint32_t cpu_addr;
    std::string insn; /* rendered text of that last instruction */
    std::string next; /* what follows: "data", "unclassified", "fill", ... */
};

/* One "; WARNING ..." line scraped from the rendered document for the Warnings
   panel.  has_location is false for warnings whose bank/cpu could not be parsed. */
struct WarningEntry {
    size_t line_index;  /* index of the warning comment line in the document */
    std::string type;   /* e.g. "inline_far_code_invalid" */
    std::string detail; /* message text after the bank/cpu/rom fields */
    bool has_location;
    bool acked; /* scraped from a "; WARNING_ACK" line */
    uint8_t bank;
    uint32_t cpu_addr;
};

/* One candidate RAM location read from an nvram-maps JSON, shown in the import
   preview window (name, address, doc, whether it collides with existing state). */
struct NvramImportRow {
    std::string name;
    uint32_t addr;
    std::string doc;
    bool selected;
    bool overwrites;      /* would replace an existing symbol/doc */
    std::string conflict; /* human description of the collision, if any */
};

/* One 16-bit immediate-load instruction (LDX/LDD/LDU/… #imm16, CMPX/… #imm16)
   surfaced by the Immediate Loads panel so the user can jump to the instruction,
   jump to the value as a target address, and label that target. */
struct ImmLoadRef {
    size_t line_index; /* the instruction's document line (jump-to-instruction) */
    uint8_t bank;      /* instruction bank */
    uint32_t cpu_addr; /* instruction address */
    uint32_t imm;      /* the 16-bit immediate value */
    uint8_t tgt_bank;  /* resolved target bank (valid only when tgt_in_rom) */
    int tgt_in_rom;    /* immediate lands in ROM address space (paged or system) */
    int symbolic;      /* the immediate currently renders as a label, not #0x.... */
    char mnem[8];      /* mnemonic, e.g. "LDX" */
};

/* One entry in the change log (View > Change Log): a single recorded edit, fed by
   the ApexProject change listener.  Address fields are navigable when has_addr. */
struct ChangeLogEntry {
    unsigned long seq;
    std::string action;
    bool has_addr;
    uint8_t bank;
    uint32_t addr;
};

/* One contiguous run of executed bytes reported by PinMAME coverage, mapped to
   ApexII's (bank, cpu_addr) model.  is_code = the run start is currently
   classified as CODE; runs that are NOT code are the actionable "missed code"
   worklist (real execution proves they are code). */
struct PinmameRun {
    uint8_t bank;
    uint32_t addr;
    uint32_t len;
    int is_code;
};

/* One parsed op of the switch-control mini-script.  Grammar (one op per line):
     press <sw> | release <sw> | pulse <sw> [ms] | wait <ms> | resume | pause
   Frame-stepped so a `wait` never blocks the UI thread. */
enum PmScriptKind {
    PM_OP_PRESS,
    PM_OP_RELEASE,
    PM_OP_PULSE,
    PM_OP_WAIT,
    PM_OP_RESUME,
    PM_OP_PAUSE
};
struct PmScriptOp {
    int kind; /* PmScriptKind */
    int a;    /* switch number */
    int b;    /* ms (pulse/wait) */
};

/* One routine in the call-frequency hotlist (execution count from instrument). */
struct PmHotEntry {
    uint8_t bank;
    uint32_t addr;
    long count;
    std::string name;
    long base; /* hotlist: count when the baseline was taken (-1 = no baseline) */
};

/* One routine in a coverage diff: instruction PCs executed in only one of the
   baseline and the current import, grouped by the routine containing them. */
struct PmDiffRoutine {
    uint8_t bank;
    uint32_t addr;  /* routine label address (or the first PC if no label) */
    uint32_t first; /* first differing PC */
    int count;      /* differing PCs in this routine */
    std::string name;
};

/* One code site that accessed a traced RAM address (RAM xref). */
struct PmAccessEntry {
    uint8_t bank;
    uint32_t addr;
    long reads = 0;
    long writes = 0;
    bool has_value = false; /* last written value known (newer emulators) */
    long last_value = 0;
    std::string name;
};

/* One live-watched RAM variable (value refreshed from the emulator each poll). */
struct PmWatch {
    uint32_t addr;
    int size;                 /* 1 or 2 bytes */
    std::string name;         /* resolved NVRAM symbol, if any */
    std::vector<uint8_t> val; /* last read */
};

/* State for the optional PinMAME dynamic-analysis integration: user config
   (persisted), the live process handle, and the last coverage import. */
struct PinmameState {
    char bin_path[512] = "/usr/local/bin/xpinmamed.x11"; /* config: xpinmamed binary */
    char rompath[512] = ""; /* config: PinMAME rom dir (defaulted at startup) */
    char game[64] = "";     /* config: romset name, e.g. "hurr_l2" */
    int port = 8080;        /* config: remote-debugger HTTP port */

    ApexPinmame pm; /* spawned child + port */
    bool connected = false;
    bool alive = false;               /* child process is running (set by the pump) */
    bool external = false;            /* attached via Connect to an emulator we did not spawn */
    bool launching = false;           /* spawned, waiting for the HTTP server */
    bool resume_on_connect = false;   /* resume the game once connected (patch test) */
    bool coverage_on_connect = false; /* patch test: clear + start coverage once connected */

    /* live DMD framebuffer */
    bool show_dmd = false;
    std::vector<uint8_t> dmd_lum;
    int dmd_w = 0, dmd_h = 0;
    double dmd_next = 0.0;
    /* animated-GIF recorder (one-frame lookahead for correct per-frame timing) */
    bool dmd_recording = false;
    ApexGif *dmd_gif = nullptr;
    int dmd_gif_w = 0, dmd_gif_h = 0; /* size the GIF was opened with */
    std::vector<uint8_t> dmd_pending; /* last captured frame not yet written */
    double dmd_pending_time = 0.0;
    bool dmd_has_pending = false;
    long dmd_rec_frames = 0;
    ApexPinmameInfo info;
    ApexPinmameCpu cpu;
    double next_poll = 0.0;

    long cov_executed = 0;
    long cov_addressable = 0;
    std::vector<PinmameRun> cov_runs;
    bool cov_only_noncode = true;             /* worklist filter */
    std::unordered_set<uint32_t> cov_reached; /* (bank<<16)|addr, for the disasm overlay */
    bool cov_overlay = false;                 /* tint executed/dead code in the disasm */
    /* Import runs its ~hundreds of HTTP requests on a worker; these hold the
       worker's raw result until the UI thread turns it into cov_runs. */
    std::vector<std::pair<uint8_t, uint32_t>> cov_fetched; /* reached (bank, addr) */
    long cov_fetched_executed = 0;
    long cov_fetched_addressable = 0;
    BackgroundJob cov_job; /* declared after its outputs: destroyed (joined) first */
    /* Differential coverage: a saved import (e.g. "ball in play, nothing hit")
       to compare a later one ("ball drained with ball save lit") against. */
    std::unordered_set<uint32_t> cov_baseline; /* same keys as cov_reached */
    long cov_baseline_executed = 0;
    int cov_view = 0; /* 0 = all runs, 1 = new since baseline, 2 = no longer executed */
    std::vector<PmDiffRoutine> cov_diff;
    bool cov_diff_dirty = true;
    const void *cov_diff_doc = nullptr; /* document cov_diff was grouped against */
    std::string status;

    /* breakpoints / watchpoints / halt */
    std::vector<ApexPinmamePoint> points;
    std::vector<ApexPinmameSwitch> switches;
    bool resumed = false;      /* we issued resume; watch for a breakpoint halt */
    int wp_mode = 2;           /* default watchpoint mode: write */
    ApexPinmameHalt last_halt; /* last SSE halt event (which bp/wp fired) */
    unsigned long last_halt_seq = 0;
    char bp_cond[64] = "";            /* optional condition for new breakpoints (e.g. A==7F) */
    int bp_ignore = 0;                /* new breakpoints: skip this many hits */
    int wp_vcond = 0;                 /* new watchpoints: 0 none, 1.. eq ne lt gt le ge */
    char wp_val[8] = "";              /* new watchpoints: value for wp_vcond (hex) */
    std::map<uint32_t, long> wp_hits; /* watchpoint hit counts (API gives none), keyed by addr */

    /* call-frequency hotlist (via instrument counters) */
    std::vector<PmHotEntry> hotlist;
    int hot_instrumented = 0;
    int hot_job_count = 0;
    bool hot_job_capped = false;
    BackgroundJob hot_job;                 /* "Instrument routines"; declared after its outputs */
    std::map<uint32_t, long> hot_baseline; /* (bank<<16)|addr -> count at baseline */
    bool hot_has_baseline = false;
    bool hot_only_new = false; /* hotlist filter: routines not called at baseline */

    /* dynamic RAM xref: collect the code sites that access a variable */
    char xref_addr[16] = "";
    int xref_mode = 3; /* 1=r 2=w 3=rw */
    bool xref_collecting = false;
    /* Collected through the emulator's memory access trace (no halts). */
    std::vector<PmAccessEntry> xref_accessors;
    long xref_total = 0;       /* trace total already consumed */
    double xref_next_poll = 0; /* next trace read */

    /* execution backtrace (recent-instruction ring, fetched on halt) */
    bool trace_enabled = false;
    std::vector<ApexPinmameTrace> trace;

    /* live RAM watch */
    std::vector<PmWatch> watches;
    char watch_addr[16] = "";
    int watch_size = 0; /* combo index: 0 = 1 byte, 1 = 2 bytes */

    /* heuristic call stack (return addresses found on the S stack at a halt) */
    std::vector<PmHotEntry> callstack;

    /* computed-jump resolver: break at an indexed JMP/JSR, step, record the real
       target across hits */
    char jmp_addr[16] = "";
    bool jmp_collecting = false;
    std::vector<ApexPinmamePoint> jmp_saved_bps; /* user's breakpoints, restored on Stop */
    uint32_t jmp_pc = 0;                         /* the jump instruction's address */
    int jmp_bank = -1;                           /* its bank (system = -1) */
    std::vector<PmHotEntry> jmp_targets;

    /* switch-control mini-script (frame-stepped, no UI blocking) */
    char script[4096] = "";
    std::vector<PmScriptOp> script_ops;
    size_t script_ip = 0;
    bool script_running = false;
    double script_next = 0.0;
    std::string script_status;

    /* Periodic status poll and DMD refresh run on workers, so a slow or hung
       emulator cannot stall the UI.  Results are adopted when the job ends. */
    struct PollResult {
        bool connected = false;
        ApexPinmameInfo info;
        ApexPinmameCpu cpu;
        std::vector<ApexPinmamePoint> points;
        std::vector<ApexPinmameSwitch> switches;
        std::vector<std::pair<uint32_t, int>> watch_keys; /* (addr, size) polled */
        std::vector<std::vector<uint8_t>> watch_vals;
    } poll_result;
    struct DmdResult {
        bool got = false;
        int w = 0, h = 0;
        std::vector<uint8_t> lum;
    } dmd_result;
    BackgroundJob poll_job; /* declared after their outputs */
    BackgroundJob dmd_job;
};

/* Which classification action the "repeat last classification" hotkey replays. */
enum ApexLastClassifyOp {
    APEX_LAST_CLASSIFY_NONE = 0,
    APEX_LAST_CLASSIFY_DATA,   /* apply_data_at_selection(last_classify_spec) */
    APEX_LAST_CLASSIFY_STRING, /* apply_string_at_selection */
    APEX_LAST_CLASSIFY_TABLE,  /* apply_table_at_selection(last_classify_spec) */
    APEX_LAST_CLASSIFY_CODE,   /* apply_code_at_selection */
    APEX_LAST_CLASSIFY_CLEAR,  /* clear_kind_at_selection */
};

struct UiState {
    size_t selected_line;
    /* Explicit multi-line block (for copy / range ops), decoupled from the
       cursor: plain clicks/navigation move the cursor only and never disturb it.
       Set via shift-click or the A/E hotkeys, cleared with Esc. */
    bool block_active = false;
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
    size_t inline_edit_line = (size_t)-1;
    bool inline_edit_focus = false;
    uint8_t inline_edit_bank = 0;
    uint32_t inline_edit_addr = 0;
    char inline_edit_buf[128] = "";

    /* Command palette (Ctrl+P): jump to any label or address by typing. */
    bool request_command_palette = false;
    bool cmd_palette_focus = false;
    bool cmd_palette_scroll = false;
    int cmd_palette_sel = 0;
    char cmd_palette_input[128] = "";
    char edit_label_input[128];
    char edit_doc_input[1024];
    char save_path_input[512];
    char base_config_path[1024];
    char status_message[256];
    char global_search_input[128];
    bool show_search_window;
    std::vector<size_t> search_results;
    unsigned long search_results_gen = 0; /* document generation search_results index into */
    unsigned search_results_ver = 0;      /* bumped whenever search_results is rebuilt */
    int request_focus_global_search;
    bool request_xref_popup;
    uint8_t xref_popup_bank;
    uint32_t xref_popup_addr;
    int edit_data_length;     /* byte count for bytes[N] data button */
    int edit_table_rows;      /* row count when edit_table_is_rows */
    int edit_table_is_rows;   /* 0 = counted, 1 = rows[N] */
    int edit_field_add_count; /* repeat count used when clicking a field button */
    ApexEditField edit_inline_fields[APEX_MAX_EDIT_FIELDS];
    int edit_inline_count;
    bool edit_inline_flow_stop;
    ApexEditField edit_schema_fields[APEX_MAX_EDIT_FIELDS];
    int edit_schema_count;
    std::vector<size_t> history_back;
    std::vector<size_t> history_forward;
    /* Line indices change on every re-render: rerender_and_reselect saves the
       history and block anchors as (bank, addr) here (from the old document)
       and finish_async_rerender maps them back onto the new one. */
    struct LineLoc {
        bool has;
        uint8_t bank;
        uint32_t addr;
    };
    std::vector<LineLoc> history_back_loc, history_forward_loc;
    LineLoc block_a_loc = {false, 0, 0}, block_b_loc = {false, 0, 0};
    bool line_locs_pending = false;
    int dmd_scrub_offset;

    size_t hex_selected_offset; /* the cursor byte (moves freely; never disturbs the block) */
    size_t editor_bound_hex = (size_t)-1; /* hex byte the editor fields were filled for */
    size_t hex_anchor_offset;             /* block start */
    size_t hex_block_end;                 /* block end — decoupled from the cursor */
    bool hex_has_range;                   /* true when a byte block is marked (a..end) */
    bool hex_active;
    bool hex_is_edit_target; /* hex view was the last view directly interacted with:
                                classify/label edits target the hex byte, not the
                                disassembly line start */
    bool hex_window_focused;
    int hex_request_follow;
    size_t hex_prev_selected_line;
    size_t hex_hover_off; /* byte hovered last frame; drives the block highlight */
    int hex_hover_valid;
    char hex_search_input[64];
    int request_focus_hex_search;
    bool hex_search_bank_only;     /* restrict byte search to the cursor's current bank */
    bool hex_search_ascii = false; /* interpret the query as ASCII text, not hex bytes */
    bool hex_search_ci = false;    /* case-insensitive (ASCII mode only) */

    /* Last classification action, replayed by the "repeat" hotkey (1) so a run of
       individual classifications reduces to n,1,n,1,…  See repeat_last_classify(). */
    int last_classify_op = APEX_LAST_CLASSIFY_NONE; /* ApexLastClassifyOp */
    std::string
        last_classify_spec; /* spec for DATA/TABLE ops (unbounded: table row formats can be long) */

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

    size_t sprite_classified_key[5] = {0, 0, 0, 0, 0}; /* refresh_sprite_classified cache */
    std::vector<struct LabelIndexEntry> cached_labels;
    /* lower-cased label name -> indices into cached_labels (ascending), so the
       per-row, per-frame link lookup in find_line_targets is O(1) per token. */
    std::unordered_map<std::string, std::vector<uint32_t>> cached_label_by_name;
    unsigned long cached_labels_gen = (unsigned long)-1;
    bool labels_valid;
    std::vector<Bookmark> bookmarks;

    /* render_line_table's filtered row list, cached across frames — rebuilt only
       when the document changes (generation) or the filter text changes. */
    std::vector<size_t> cached_visible;
    unsigned long cached_visible_gen = (unsigned long)-1;
    std::string cached_visible_filter;

    /* project->refs indices sorted by (source_bank, source_addr) so
       find_outgoing_refs is O(log n) instead of a full scan every frame.
       Rebuilt when the document generation changes (i.e. after a re-analysis). */
    std::vector<uint32_t> cached_out_ref_order;
    unsigned long cached_out_ref_gen = (unsigned long)-1;

    /* Transitions / Strings list rows, cached on (generation, filter) so the
       O(line_count) scan runs only when the document or filter changes. */
    std::vector<size_t> cached_transitions;
    unsigned long cached_transitions_gen = (unsigned long)-1;
    std::string cached_transitions_filter;
    std::vector<size_t> cached_strings_rows;
    unsigned long cached_strings_gen = (unsigned long)-1;
    std::string cached_strings_filter;

    /* Full per-ROM-byte extended block-kind map for the Hex view, cached on the
       document generation so the O(line_count) forward-fill runs once per edit
       instead of every frame. */
    std::vector<uint8_t> cached_hex_kinds;
    unsigned long cached_hex_kinds_gen = (unsigned long)-1;

    /* Async re-render: an edit sets async_pending; the main loop launches the
       render on a worker thread and shows a busy overlay (rendering=true) until
       it finishes, then reselects (async_b, async_a). */
    bool async_pending = false;
    bool rendering = false;
    uint8_t async_b = 0;
    uint32_t async_a = 0;
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
    bool show_hooks;
    HooksWindowState hooks_state;
    bool show_args;
    ArgsWindowState args_state;
    bool show_bits;
    BitsWindowState bits_state;
    bool show_card;
    CardWindowState card_state;
    bool show_timeline;
    TimelineState timeline;
    bool show_patch;
    PatchUiState patch_state;
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
    std::vector<size_t> ram_ref_results; /* line indices into the document of ram_ref_gen */
    std::string ram_ref_query;           /* query that produced ram_ref_results */
    unsigned long ram_ref_gen = 0;
    unsigned ram_ref_ver = 0;
    int request_focus_ram_refs;

    bool show_imm_loads;
    std::vector<ImmLoadRef> imm_load_results;
    bool imm_loads_scanned;
    bool imm_only_unlabeled; /* filter: hide immediates already resolved to a label */
    bool imm_only_rom;       /* filter: only immediates whose value lands in ROM space */

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
    char sym_edit_length[16]; /* block byte length (dec or 0x-hex); empty/1 = single byte */
    int sym_selected;         /* index into project->symbols, -1 = none */
    int sym_usages_sel;
    const ApexRenderedDocument *sym_usages_doc;
    std::vector<size_t> sym_usages_cache;
    bool show_rom_map;
    bool show_dmd_list;
    bool show_sprite_list;
    bool show_sprite_gallery;
    bool show_code_candidates;
    ApexCodeCandidates code_candidates;
    unsigned code_candidates_ver = 0; /* bumped on every rescan (table sort cache) */
    bool code_candidates_stale;
    bool show_inline_detect = false; /* "Inline detection (prime bank)" window */
    ApexInlineFindings inline_findings = {nullptr, 0, 0};
    bool inline_findings_valid = false;
    unsigned long inline_findings_gen = 0; /* document generation of the last run */
    bool inline_detect_show_confirmed = false;
    char inline_detect_status[96] = "";
    bool show_warnings;
    std::vector<WarningEntry> warnings;
    bool warnings_stale;
    bool show_flow_breaks;
    std::vector<FlowBreakEntry> flow_breaks;
    bool flow_breaks_stale;
    /* "Create type from a text table's strings" popup state. */
    bool tt_type_request = false;
    uint8_t tt_type_bank = 0;
    uint32_t tt_type_addr = 0;
    int tt_type_word = 0;
    char tt_type_name[64] = "";
    bool show_nvram_import;
    std::vector<NvramImportRow> nvram_import_rows;
    char nvram_source_path[512]; /* last imported nvram JSON — reused as export template */

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
    int sprite_nh_height = 12;   /* height for sprite_noheader classify button */
    int sprite_gallery_zoom = 1; /* Sprite Gallery pixel zoom: 1x / 2x / 4x */

    struct VsiTableEntry {
        int table_idx;        /* index in master table */
        int image_idx;        /* image number within sub-table */
        uint8_t table_height; /* height from sub-table descriptor */
        uint8_t bank;
        uint32_t cpu_addr;
        size_t rom_offset;
        bool is_noheader; /* true = no-header VSI (byte 0 = width) */
        uint8_t width, height;
        bool classified; /* already in data_ranges as DATA_SPRITE / DATA_SPRITE_NOHEADER */
    };
    struct VsiSubTableInfo {
        int table_idx;
        uint8_t bank;
        uint32_t cpu_addr; /* sub-table start address */
        size_t
            header_len; /* bytes before pointer array (min/max pairs + terminator + H + spacing) */
        int num_images;
        uint8_t table_height;
    };
    std::vector<VsiTableEntry> vsi_table_entries;
    std::vector<VsiSubTableInfo> vsi_sub_tables;
    bool vsi_table_scan_done = false;

    /* Reviewable table-search candidates (suggest-then-accept, like sprites). */
    struct TableCandidate {
        uint8_t bank;
        uint32_t cpu_addr;
        std::string spec; /* the [tables] spec to apply, e.g. rows[42](far_dmd_fullframe) */
        int rows;         /* row count (for display/sorting) */
        std::string kind; /* short label: "far_dmd", "far_ptr", "text" */
        bool already;     /* address already classified as a table */
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
    uint8_t value;     /* aux byte (far_imm target bank); 0 for valueless sections */
    uint8_t value2;    /* far_imm: FarImmType */
    uint32_t aux_addr; /* far_imm: paired bank-load instruction address */
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
    uint32_t value;
    uint32_t length;
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
    size_t planes;  /* bit-plane count (1, or 2 for 4-colour) */
    bool two_plane; /* plane1 holds the second bit-plane */
    char title[128];
    uint8_t plane[APEX_DMD_PAGE_BYTES];
    uint8_t plane1[APEX_DMD_PAGE_BYTES]; /* second bit-plane (two_plane only) */
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
    uint8_t pixels[APEX_SPRITE_MAX_BYTES];  /* plane 0 */
    bool two_plane;                         /* bicolor: pixels1 holds plane 1 */
    uint8_t pixels1[APEX_SPRITE_MAX_BYTES]; /* plane 1 (bicolor only) */
};

// --- Shared Helper Function Declarations ---

// Core State & Navigation
void select_line(UiState *state, size_t line_index, int request_scroll);
void render_command_palette(const ApexRenderedDocument **document_ptr, UiState *state);
void handle_line_selection(UiState *state, size_t line_index, bool shift_held);
void history_jump(UiState *state, int backward);
void set_status(UiState *state, const char *message);
int selected_address(const ApexRenderedDocument *document, const UiState *state, uint8_t *bank,
                     uint32_t *cpu_addr);
LineByteSpan selected_line_span(const ApexProject *project, const ApexRenderedDocument *document,
                                const UiState *state);
int project_locate_rom_bytes(const ApexProject *project, uint8_t bank, uint32_t addr,
                             const uint8_t **src, size_t *len, size_t *rom_offset);

// DMD logic
int address_is_dmd_fullframe_start(const ApexProject *project, uint8_t bank, uint32_t addr);
int decode_dmd_preview_at(const ApexProject *project, uint8_t bank, uint32_t addr,
                          DmdPreviewInfo *preview);
DmdPreviewInfo find_dmd_preview(const ApexProject *project, const ApexRenderedDocument *document,
                                UiState *state);

// Sprite logic
int address_is_sprite_start(const ApexProject *project, uint8_t bank, uint32_t addr);
int decode_sprite_preview_at(const ApexProject *project, uint8_t bank, uint32_t addr,
                             SpritePreviewInfo *preview);
int decode_sprite_preview_with_height(const ApexProject *project, uint8_t bank, uint32_t addr,
                                      unsigned hint_height, SpritePreviewInfo *preview);
SpritePreviewInfo find_sprite_preview(const ApexProject *project,
                                      const ApexRenderedDocument *document, UiState *state);

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
void spec_to_fields(const char *spec, ApexEditField *fields, int *count, int max,
                    const ApexProject *p);
void load_doc_editor_buffer(const ApexProject *project, UiState *state, uint8_t bank,
                            uint32_t cpu_addr);

// Analysis: Labels & Pointers
void ensure_label_index(const ApexRenderedDocument *document, UiState *state);
int label_entry_matches_filter(const LabelIndexEntry &entry, const char *filter);
std::string label_at_address(const ApexRenderedDocument *document, UiState *state, uint8_t bank,
                             uint32_t cpu_addr);
std::vector<LineTargetEntry> find_line_targets(const ApexRenderedDocument *document, UiState *state,
                                               const ApexRenderedLine *line);
int resolve_pointer_target(const ApexProject *project, const ApexRenderedLine *line,
                           uint8_t *target_bank, uint32_t *target_addr, int *is_far);
int jump_to_first_line_target(const ApexRenderedDocument *document, UiState *state,
                              const ApexRenderedLine *line);
void follow_selected_link(const ApexRenderedDocument *document, UiState *state);

// Analysis: XRefs
std::vector<RefEntry> find_incoming_refs(const ApexProject *project,
                                         const ApexRenderedDocument *document, UiState *state,
                                         uint8_t bank, uint32_t cpu_addr);
std::vector<RefEntry> find_outgoing_refs(const ApexProject *project,
                                         const ApexRenderedDocument *document, UiState *state,
                                         uint8_t bank, uint32_t cpu_addr);

// Analysis: Call Graph
int find_routine_start(const ApexRenderedDocument *document, uint8_t bank, uint32_t addr,
                       size_t *out_line_index);
void rebuild_call_graph(ApexProject *project, const ApexRenderedDocument *document, UiState *state);

// Analysis: Misc
bool select_line_by_address(const ApexRenderedDocument *document, UiState *state);
void jump_to_transition(const ApexRenderedDocument *document, UiState *state,
                        ApexRenderedTransitionKind kind, int forward);
void move_selection_relative(const ApexRenderedDocument *document, UiState *state, int delta);
void jump_primary_transition(const ApexRenderedDocument *document, UiState *state, int forward);
void sync_editor_state(const ApexProject *project, const ApexRenderedDocument *document,
                       UiState *state);
void auto_label_targets(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state);
void rerender_and_reselect(ApexProject *project, const ApexRenderedDocument **document_ptr,
                           UiState *state, uint8_t bank, uint32_t cpu_addr);
void finish_async_rerender(ApexProject *project, const ApexRenderedDocument **document_ptr,
                           UiState *state);
void apply_code_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state);
void apply_data_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state, const char *spec);
void apply_string_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr,
                               UiState *state);
void apply_table_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr,
                              UiState *state, const char *spec);
void clear_kind_at_selection(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state);
void repeat_last_classify(ApexProject *project, const ApexRenderedDocument **document_ptr,
                          UiState *state);
const ApexRenderedLine *find_first_line_in_bank(const ApexRenderedDocument *document, uint8_t bank,
                                                size_t *line_index);
int find_line_by_rom_offset(const ApexRenderedDocument *document, size_t rom_offset,
                            size_t *line_index);
int rom_offset_to_cpu_address(const ApexProject *project, size_t offset, uint8_t *bank,
                              uint32_t *cpu_addr);
ApexRenderedBlockKind get_offset_kind(const ApexProject *project,
                                      const ApexRenderedDocument *document, size_t offset);

// Data
const CpuHelpInfo *lookup_cpu_help(const char *mnemonic);
const HardwareRegister *lookup_hardware(uint32_t addr);
std::vector<const HardwareRegister *> find_hardware_in_text(const char *text, size_t length);

// Project & Snapshot
OriginalSnapshot build_original_snapshot(const ApexProject *project);
OriginalSnapshot build_config_snapshot(const char *config_path);
int write_delta_overlay(const ApexProject *project, const OriginalSnapshot *snapshot,
                        const char *path, const char *include_path, std::string *status);
int write_full_config(ApexProject *project, const char *path, std::string *status);
/* Save the overlay: a delta against the base INI when possible, else a full
   overlay.  Sets the status message; returns true on success. */
bool save_overlay(ApexProject *project, const OriginalSnapshot *snapshot, UiState *state);

// Session
void clear_session();
void save_session(const char *rom_path, const char *config_path, const UiState *state,
                  const ApexRenderedDocument *document);
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
void render_line_table(ApexProject *project, const ApexRenderedDocument **document_ptr,
                       UiState *state);
void render_label_list(const ApexRenderedDocument *document, UiState *state);
void render_bank_list(const ApexProject *project, const ApexRenderedDocument *document,
                      UiState *state);
void render_transition_list(const ApexRenderedDocument *document, UiState *state);
void render_xref_popup(ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_bookmark_list(const ApexRenderedDocument *document, UiState *state);
void render_global_search(const ApexRenderedDocument *document, UiState *state);
void render_hex_view(ApexProject *project, const ApexRenderedDocument **document_ptr,
                     UiState *state);
void render_call_graph(ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_editor(ApexProject *project, const ApexRenderedDocument **document_ptr,
                   const OriginalSnapshot *snapshot, UiState *state);
void render_dmd_view(const ApexProject *project, const ApexRenderedDocument *document,
                     UiState *state);
void render_tables_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                          UiState *state);
void render_hardware_window(ApexProject *project, const ApexRenderedDocument *document,
                            UiState *state);

// Analysis: Tables
void scan_table_candidates(ApexProject *project, UiState *state);

// Analysis: Hardware
std::vector<HardwareAccess> find_hardware_accesses(const ApexProject *project,
                                                   const ApexRenderedDocument *document);
size_t hardware_register_count();
const HardwareRegister *get_hardware_register(size_t index);

// ROM Info
void render_rom_info(ApexProject *project, const ApexRenderedDocument *document, UiState *state);

// Match from Reference
void render_match_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                         UiState *state);
void render_rom_compare_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                               UiState *state);
void render_coverage_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                            UiState *state);
void render_hooks_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                         UiState *state);
void render_args_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state);

// Analysis: Inline list, Entries list & Types editor
void render_inline_list(ApexProject *project, const ApexRenderedDocument *document, UiState *state);
void render_entries_list(ApexProject *project, const ApexRenderedDocument **document_ptr,
                         UiState *state);
void render_strings_list(ApexProject *project, const ApexRenderedDocument *document,
                         UiState *state);
void render_types_editor(ApexProject *project, UiState *state);
void render_symbols_editor(ApexProject *project, const ApexRenderedDocument *document,
                           UiState *state);

// Analysis: Pattern Search & RAM XRefs
std::vector<size_t> search_hex_pattern(const ApexProject *project, const char *input);
std::vector<size_t> find_ram_refs(const ApexRenderedDocument *document, const char *addr_input);
void render_pattern_search(ApexProject *project, const ApexRenderedDocument **document_ptr,
                           UiState *state);
void render_ram_refs(const ApexProject *project, const ApexRenderedDocument *document,
                     UiState *state);

// Analysis: Immediate Loads (LDX/LDD/… #imm16 that may reference an address)
std::vector<ImmLoadRef> find_imm_loads(const ApexProject *project,
                                       const ApexRenderedDocument *document);
void render_imm_loads(ApexProject *project, const ApexRenderedDocument **document_ptr,
                      UiState *state);

// Change Log: audit trail of every recorded edit, fed by the project change listener
void ui_change_listener(void *ctx, const ApexChangeEvent *ev);
void render_change_log(const ApexRenderedDocument *document, UiState *state);

// PinMAME: optional dynamic-analysis integration (spawn xpinmamed, import coverage)
// pinmame_pump runs the per-frame work (poll, halt, script, DMD fetch) once from the
// main loop, so the live views stay updated regardless of which window is visible.
void pinmame_pump(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
/* Patching (apeximgui_patch.cpp). */
void render_patch_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                         UiState *state);
/* Per-frame patch work: keeps the project bytes in step with the
   Original/Patched view once no render is running. */
void patch_pump(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
/* "Patch" submenu items for a disassembly line (context menu). */
void patch_context_menu(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state, const ApexRenderedLine *line);
/* Op index whose patched bytes cover [rom_off, rom_end), or -1. */
int patch_op_at(const UiState *state, size_t rom_off, size_t rom_end);
/* Main-menu "Patch" entries. */
void patch_menu(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
/* Game Hooks: redirect a hook slot to new patch code (an allocated asm op in
   the target's bank that ends by continuing at the original target, and a far
   pointer op at the slot). */
void patch_redirect_slot(ApexProject *project, UiState *state, uint32_t slot_addr,
                         uint8_t target_bank, uint32_t target_addr, bool target_is_stub,
                         const char *slot_name);
/* Undo / redo of patch edits; patch_undo_active: the Patch window has focus
   and a patch is open (then Ctrl+Z / Ctrl+Y go to it). */
/* PinMAME just connected (before the game resumes): install the Check run
   read traces of a patch test. */
void patch_pinmame_connected(UiState *state);
bool patch_undo_active(const UiState *state);
/* The "Insert Snippet" window (opened from the Patch window / context menu). */
void render_snippet_dialog(ApexProject *project, UiState *state);
/* Insight windows (apeximgui_views_insight.cpp). */
void render_bits_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state);
void render_card_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state);
void render_timeline_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                            UiState *state);
/* Types window section: adjustment / audit names from the ROM's text tables. */
void render_adjnames_section(ApexProject *project, UiState *state);
/* Per-frame timeline recording (polls PinMAME while recording). */
void timeline_pump(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state);
/* The rebase result window (Patch > Export > Rebase onto another ROM). */
void render_rebase_result(ApexProject *project, UiState *state);
void patch_undo(ApexProject *project, UiState *state, bool redo);
/* Load the patch file remembered for this ROM (session) without UI. */
void patch_open_path(ApexProject *project, UiState *state, const char *path);
void render_pinmame(ApexProject *project, const ApexRenderedDocument **document_ptr,
                    UiState *state);
void render_pinmame_dmd(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state);
void render_pinmame_switches(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state);
void render_pinmame_coverage(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state);
void pinmame_stop_gif(UiState *state); /* finalise an in-progress DMD GIF recording */

// Analysis: Ref Exclusions
void render_ref_exclusions(ApexProject *project, const ApexRenderedDocument **document_ptr,
                           UiState *state);
void render_code_candidates(ApexProject *project, const ApexRenderedDocument **document_ptr,
                            UiState *state);
void render_inline_detect(ApexProject *project, const ApexRenderedDocument **document_ptr,
                          UiState *state);
void render_warnings_view(ApexProject *project, const ApexRenderedDocument **document_ptr,
                          UiState *state);
void render_flow_breaks_view(ApexProject *project, const ApexRenderedDocument **document_ptr,
                             UiState *state);
/* From a line inside a TABLE block, find the table's start (header) address.
   Returns false if the line is not part of a table. */
bool find_table_start(const ApexRenderedDocument *d, size_t line_idx, uint8_t *out_bank,
                      uint32_t *out_addr);
void render_nvram_import_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                                UiState *state);
/* Populate state->nvram_import_rows from parsed nvram locations, flagging
   collisions against the project's current symbols/docs. Returns 0 on success. */
int nvram_prepare_import(ApexProject *project, UiState *state, const char *json_path,
                         std::string *err);
/* Write the project's RAM symbols/docs to an nvram-maps JSON file. When
   template_path is non-empty and readable, merge into it losslessly (only
   names/docs updated); otherwise write a fresh minimal map. */
int nvram_export(const ApexProject *project, const char *json_path, const char *template_path,
                 std::string *err);
void render_rom_map(ApexProject *project, const ApexRenderedDocument **document_ptr,
                    UiState *state);

// DMD and Sprite list windows
void render_dmd_list_window(const ApexProject *project, const ApexRenderedDocument *document,
                            UiState *state);
void render_sprite_list_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                               UiState *state);
void render_sprite_gallery_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                                  UiState *state);

#endif
