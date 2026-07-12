#include "apeximgui_core.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "ImGuiFileDialog.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <signal.h>

static ApexProject *g_crash_project = NULL;
static const OriginalSnapshot *g_crash_snapshot = NULL;
static char g_crash_overlay_path[512] = "";
static char g_crash_base_config[1024] = "";

/* User-adjustable global font zoom (View menu + Ctrl +/-/0), applied via
   style.FontScaleMain and persisted in imgui.ini through a settings handler.
   Independent of automatic HiDPI scaling (io.ConfigDpiScaleFonts → FontScaleDpi),
   which composes on top: text size = base * FontScaleMain * FontScaleDpi. */
static const float FONT_SCALE_MIN  = 0.5f;
static const float FONT_SCALE_MAX  = 3.0f;
static const float FONT_SCALE_STEP = 0.1f;
static float g_ui_font_scale = 1.0f;

/* Persisted visibility of the Warnings panel (mirrors state.show_warnings; synced
   in the main loop). Kept as a global so the imgui.ini settings handler can reach
   it without a UiState pointer. */
static int g_show_warnings = 0;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void set_ui_font_scale(float v)
{
    g_ui_font_scale = clampf(v, FONT_SCALE_MIN, FONT_SCALE_MAX);
    ImGui::MarkIniSettingsDirty(); /* schedule a persist of the new value */
}

/* Persist g_ui_font_scale in imgui.ini alongside window/dock layout. Registered
   before the first NewFrame so the stored value is loaded at startup. */
static void register_view_settings_handler(void)
{
    ImGuiSettingsHandler h;
    h.TypeName = "ApexII";
    h.TypeHash = ImHashStr("ApexII");
    h.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *, const char *name) -> void * {
        return strcmp(name, "View") == 0 ? (void *)1 : NULL;
    };
    h.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *, void *, const char *line) {
        float v; int iv;
        if (sscanf(line, "FontScale=%f", &v) == 1) {
            g_ui_font_scale = clampf(v, FONT_SCALE_MIN, FONT_SCALE_MAX);
        } else if (sscanf(line, "ShowWarnings=%d", &iv) == 1) {
            g_show_warnings = iv != 0;
        }
    };
    h.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf) {
        buf->appendf("[%s][View]\n", handler->TypeName);
        buf->appendf("FontScale=%.3f\n", g_ui_font_scale);
        buf->appendf("ShowWarnings=%d\n\n", g_show_warnings);
    };
    ImGui::AddSettingsHandler(&h);
}

static void handle_fatal_signal(int sig)
{
    if (g_crash_project && g_crash_snapshot && g_crash_overlay_path[0]) {
        char backup_path[524];
        snprintf(backup_path, sizeof(backup_path), "%s.crash", g_crash_overlay_path);
        std::string st;
        write_delta_overlay(g_crash_project, g_crash_snapshot, backup_path,
                            g_crash_base_config, &st);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Directory of a file path, for seeding a file dialog's starting folder.
   ImGuiFileDialog's `path` must be a directory; passing a file (e.g. the config
   .ini) makes its directory iterator fail repeatedly ("Not a directory").
   Returns "." when the path has no directory component or is empty. */
static std::string dir_of(const char *p)
{
    if (!p || !p[0]) return ".";
    const char *fwd = strrchr(p, '/');
    const char *bwd = strrchr(p, '\\');
    const char *s = (fwd && bwd) ? (fwd > bwd ? fwd : bwd) : (fwd ? fwd : bwd);
    if (!s) return ".";
    if (s == p) return "/";              /* path like "/file" */
    return std::string(p, (size_t)(s - p));
}

/* Helper: render one frame for the startup file picker loop */
static void startup_frame_render(SDL_Window *win)
{
    ImGui::Render();
    ImGuiIO &fio = ImGui::GetIO();
    glViewport(0, 0, (int)fio.DisplaySize.x, (int)fio.DisplaySize.y);
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(win);
}

/* Show a file dialog in a mini-loop before the main project is loaded.
   Returns true when a path was chosen; false means user cancelled/quit.
   If ini_mode is true, Cancel means "no INI" (still returns true). */
static bool startup_pick_file(SDL_Window *win, bool ini_mode,
                               char *out_path, size_t out_size)
{
    const char *key     = ini_mode ? "StartupIni" : "StartupRom";
    const char *title   = ini_mode ? "Select Config INI  (Cancel = no INI)"
                                   : "Select ROM File";
    const char *filters = ini_mode ? ".ini" : ".rom,.bin";

    IGFD::FileDialogConfig cfg;
    cfg.path = ".";
    ImGuiFileDialog::Instance()->OpenDialog(key, title, filters, cfg);

    bool picked = false;
    bool user_quit = false;
    while (!picked && !user_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) { user_quit = true; }
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::GetStyle().FontScaleMain = g_ui_font_scale; /* honor persisted font zoom */

        ImGuiIO &fio = ImGui::GetIO();
        ImVec2 dlg_min(std::min(700.0f, fio.DisplaySize.x * 0.85f),
                       std::min(450.0f, fio.DisplaySize.y * 0.85f));
        if (ImGuiFileDialog::Instance()->Display(key,
                ImGuiWindowFlags_NoCollapse, dlg_min)) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
                strncpy(out_path, p.c_str(), out_size - 1);
                out_path[out_size - 1] = '\0';
            } else if (!ini_mode) {
                user_quit = true;  /* Cancel on ROM dialog = quit */
            }
            /* Cancel on INI dialog = no INI (out_path stays empty) */
            ImGuiFileDialog::Instance()->Close();
            picked = true;
        }

        startup_frame_render(win);
    }
    return !user_quit;
}

int main(int argc, char **argv)
{
    ApexProject *project = NULL;
    const ApexRenderedDocument *document = NULL;
    OriginalSnapshot original_snapshot;
    SDL_Window *window = NULL;
    SDL_GLContext gl_context = NULL;
    UiState state = {};
    bool done = false;
    bool want_quit = false;
    bool want_consolidate = false;
    bool want_save_ini_as = false;
    bool want_nvram_import = false;
    bool want_nvram_export = false;
    char rom_path[1024] = "";
    char config_path[1024] = "";

    if (argc == 3) {
        strncpy(rom_path,    argv[1], 1023);
        strncpy(config_path, argv[2], 1023);
    } else if (argc == 2) {
        strncpy(rom_path, argv[1], 1023);
        config_path[0] = '\0';
    } else if (argc == 1) {
        load_global_session(rom_path, config_path);
        /* If session also empty, rom_path stays ""; picker runs below */
    } else {
        fprintf(stderr, "usage: %s ROM [CONFIG]\n", argv[0]);
        return 2;
    }

    /* --- SDL + OpenGL + ImGui init (must happen before any file picker) --- */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window = SDL_CreateWindow("ApexII",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1600, 950,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                              SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    /* Auto-scale fonts to the monitor's DPI (dynamic fonts, no atlas rebuild).
       The user's manual zoom (g_ui_font_scale) composes on top of this. */
    io.ConfigDpiScaleFonts = true;
    /* Keep disassembly warnings out of the console; the Warnings panel shows them. */
    apex_warn_to_stderr = 0;
    /* Must be registered before the first NewFrame loads imgui.ini. */
    register_view_settings_handler();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 150");

    /* --- Startup file picker (when no ROM was specified) --- */
    if (rom_path[0] == '\0') {
        if (!startup_pick_file(window, false, rom_path, sizeof(rom_path))) {
            /* User quit the ROM picker */
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();
            SDL_GL_DeleteContext(gl_context);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }
        /* Optionally pick an INI */
        startup_pick_file(window, true, config_path, sizeof(config_path));
    }

    /* --- Load project --- */
    /* If an overlay from a previous session exists, use it as effective config. */
    const char *effective_config = config_path[0] ? config_path : NULL;
    char overlay_path[1040] = "";
    if (config_path[0]) {
        FILE *of;
        snprintf(overlay_path, sizeof(overlay_path), "%s.apeximgui.ini", config_path);
        of = fopen(overlay_path, "r");
        if (of) {
            fclose(of);
            effective_config = overlay_path;
        }
    }

    project = apex_project_open(rom_path, effective_config);
    if (!project || apex_project_analyze(project) != 0) {
        fprintf(stderr, "failed to analyze\n");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    document = apex_project_render(project, 0, 0);
    if (!document) {
        fprintf(stderr, "failed to render\n");
        apex_project_free(project);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    original_snapshot = build_config_snapshot(config_path[0] ? config_path : NULL);

    state.editor_bound_line  = (size_t)-1;
    state.labels_valid       = false;
    state.graph_depth_in     = 2;
    state.graph_depth_out    = 2;
    state.graph_needs_rebuild = true;
    state.show_navigator     = true;
    state.show_disasm        = true;
    state.show_labels        = true;
    state.show_banks         = true;
    state.show_details       = true;
    state.show_refs          = true;
    state.show_dmd           = true;
    state.show_edit          = true;
    state.show_hex           = true;
    state.show_call_graph    = false;
    state.show_hardware      = false;
    state.show_tables        = false;
    state.show_match_window     = false;
    state.show_rom_compare      = false;
    state.show_coverage         = false;
    state.match_state.scan_enabled   = true;
    state.match_state.min_confidence = APEX_MATCH_CONF_MEDIUM;
    state.show_inline_list      = false;
    state.show_entries_list     = false;
    state.show_strings_list     = false;
    state.show_types_editor     = false;
    state.show_symbols_editor   = false;
    state.sym_selected          = -1;
    state.sym_usages_sel        = -1;
    state.sym_usages_doc        = nullptr;
    state.show_ref_exclusions    = false;
    state.show_code_candidates    = false;
    state.code_candidates         = {};
    state.code_candidates_stale   = false;
    state.show_inline_candidates  = false;
    state.inline_candidates       = {};
    state.inline_candidates_stale = false;
    state.show_warnings           = false;
    state.warnings_stale          = true;
    state.show_nvram_import       = false;
    state.nvram_source_path[0]    = '\0';
    state.show_rom_map           = false;
    state.show_dmd_list         = false;
    state.show_sprite_list      = false;
    state.show_sprite_gallery   = false;
    state.sprite_scan_done      = false;
    state.show_bookmarks     = true;
    state.show_transitions   = false;
    /* Build the default docked layout only on first run (no imgui.ini yet). On
       later runs ImGui restores the saved window positions/docking/visibility,
       so a user's arrangement persists. "Reset Layout" forces a rebuild. */
    bool ini_exists = false;
    if (io.IniFilename && io.IniFilename[0]) {
        FILE *inif = fopen(io.IniFilename, "rb");
        if (inif) { ini_exists = true; fclose(inif); }
    }
    state.request_layout_reset = !ini_exists;

    /* Restore session state after defaults are set so saved values win. */
    load_rom_session(rom_path, &state, document);

    if (config_path[0]) {
        /* save_path_input is 512 bytes; ".apeximgui.ini" is 14 chars → cap path at 497 chars */
        snprintf(state.save_path_input, sizeof(state.save_path_input), "%.*s.apeximgui.ini",
                 (int)(sizeof(state.save_path_input) - 15), config_path);
        strncpy(state.base_config_path, config_path, 1023);
        state.base_config_path[1023] = '\0';
    } else if (rom_path[0]) {
        /* No config supplied: derive default from ROM filename.
           Strip .rom/.bin extension (any case) and append .ini. */
        size_t len = strlen(rom_path);
        /* Find start of last path component. */
        size_t last_sep = 0;
        for (size_t i = 0; i < len; i++)
            if (rom_path[i] == '/' || rom_path[i] == '\\') last_sep = i + 1u;
        /* Find last dot in that component (use len = no dot if none found). */
        size_t dot = len;
        for (size_t i = last_sep; i < len; i++)
            if (rom_path[i] == '.') dot = i;
        /* Only strip if the extension is .rom or .bin (case-insensitive). */
        if (dot < len) {
            const char *e = rom_path + dot;
            char el[8] = {'\0'};
            for (int j = 0; j < 7 && e[j]; j++)
                el[j] = (e[j] >= 'A' && e[j] <= 'Z') ? (char)(e[j] + 32) : e[j];
            if (strcmp(el, ".rom") != 0 && strcmp(el, ".bin") != 0)
                dot = len;
        }
        snprintf(state.save_path_input, sizeof(state.save_path_input),
                 "%.*s.ini", (int)dot, rom_path);
        state.base_config_path[0] = '\0';
    } else {
        strcpy(state.save_path_input, "apeximgui.ini");
        state.base_config_path[0] = '\0';
    }

    /* Update window title to show ROM filename */
    {
        const char *base = strrchr(rom_path, '/');
        if (!base) base = strrchr(rom_path, '\\');
        SDL_SetWindowTitle(window, base ? base + 1 : rom_path);
    }

    g_crash_project  = project;
    g_crash_snapshot = &original_snapshot;
    snprintf(g_crash_overlay_path, sizeof(g_crash_overlay_path), "%s", state.save_path_input);
    snprintf(g_crash_base_config,  sizeof(g_crash_base_config),  "%s", state.base_config_path);
    signal(SIGSEGV, handle_fatal_signal);
    signal(SIGABRT, handle_fatal_signal);
    signal(SIGFPE,  handle_fatal_signal);
    signal(SIGILL,  handle_fatal_signal);

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                if (state.overlay_dirty) { want_quit = true; } else { done = true; }
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                if (state.overlay_dirty) { want_quit = true; } else { done = true; }
            }
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::GetStyle().FontScaleMain = g_ui_font_scale; /* apply user font zoom */
        /* Apply the persisted Warnings-panel visibility once (imgui.ini is loaded
           on the first NewFrame), then mirror user toggles back for saving. */
        {
            static bool warnings_vis_applied = false;
            if (!warnings_vis_applied) {
                warnings_vis_applied = true;
                state.show_warnings = g_show_warnings != 0;
            } else if ((state.show_warnings ? 1 : 0) != g_show_warnings) {
                g_show_warnings = state.show_warnings ? 1 : 0;
                ImGui::MarkIniSettingsDirty();
            }
        }
        sync_editor_state(project, document, &state);

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save Overlay", "Ctrl+S")) {
                    state.request_save_overlay = 1;
                }
                if (ImGui::MenuItem("Save INI As...")) {
                    want_save_ini_as = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Import RAM Map (JSON)...")) {
                    want_nvram_import = true;
                }
                if (ImGui::MenuItem("Export RAM Map (JSON)...")) {
                    want_nvram_export = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Re-analyze", "F5")) {
                    uint8_t cur_b = 0xffu; uint32_t cur_a = 0u;
                    selected_address(document, &state, &cur_b, &cur_a);
                    rerender_and_reselect(project, &document, &state, cur_b, cur_a);
                }
                if (ImGui::MenuItem("Force Full Re-analyze", "Shift+F5")) {
                    uint8_t cur_b = 0xffu; uint32_t cur_a = 0u;
                    selected_address(document, &state, &cur_b, &cur_a);
                    apex_project_invalidate(project, APEX_DIRTY_ANALYSIS);
                    rerender_and_reselect(project, &document, &state, cur_b, cur_a);
                }
                {
                    bool has_base = state.base_config_path[0] != '\0';
                    if (!has_base) ImGui::BeginDisabled();
                    if (ImGui::MenuItem("Consolidate into Base INI")) {
                        want_consolidate = true;
                    }
                    if (!has_base) ImGui::EndDisabled();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    if (state.overlay_dirty) { want_quit = true; } else { done = true; }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                const char *ul = apex_project_undo_label(project);
                const char *rl = apex_project_redo_label(project);
                char ulbl[64], rlbl[64];
                if (ul) snprintf(ulbl, sizeof(ulbl), "Undo %s", ul);
                else    snprintf(ulbl, sizeof(ulbl), "Undo");
                if (rl) snprintf(rlbl, sizeof(rlbl), "Redo %s", rl);
                else    snprintf(rlbl, sizeof(rlbl), "Redo");
                if (ImGui::MenuItem(ulbl, "Ctrl+Z", false, apex_project_can_undo(project))) {
                    uint8_t cb = 0xffu; uint32_t ca = 0u;
                    selected_address(document, &state, &cb, &ca);
                    if (apex_project_undo(project) == 0)
                        rerender_and_reselect(project, &document, &state, cb, ca);
                }
                if (ImGui::MenuItem(rlbl, "Ctrl+Y", false, apex_project_can_redo(project))) {
                    uint8_t cb = 0xffu; uint32_t ca = 0u;
                    selected_address(document, &state, &cb, &ca);
                    if (apex_project_redo(project) == 0)
                        rerender_and_reselect(project, &document, &state, cb, ca);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Windows")) {
                ImGui::MenuItem("Navigator",    "N",      &state.show_navigator);
                ImGui::MenuItem("Disassembly",  "D",      &state.show_disasm);
                ImGui::MenuItem("Hex Inspector","H",      &state.show_hex);
                ImGui::MenuItem("Global Search","Ctrl+F", &state.show_search_window);
                ImGui::Separator();
                ImGui::MenuItem("Banks",       NULL, &state.show_banks);
                ImGui::MenuItem("Labels",      NULL, &state.show_labels);
                ImGui::MenuItem("Bookmarks",   NULL, &state.show_bookmarks);
                ImGui::MenuItem("Transitions",  NULL, &state.show_transitions);
                ImGui::MenuItem("Flow Arrows",  NULL, &state.show_flow_arrows);
                ImGui::Separator();
                ImGui::MenuItem("ROM Info",             NULL, &state.show_rom_info);
                ImGui::MenuItem("Match from Reference", NULL, &state.show_match_window);
                ImGui::MenuItem("ROM Compare",          NULL, &state.show_rom_compare);
                ImGui::MenuItem("Coverage",             NULL, &state.show_coverage);
                ImGui::MenuItem("Call Graph",     NULL, &state.show_call_graph);
                ImGui::MenuItem("Hardware",       NULL, &state.show_hardware);
                ImGui::MenuItem("Tables",         NULL, &state.show_tables);
                ImGui::MenuItem("Inline Sigs",    NULL, &state.show_inline_list);
                ImGui::MenuItem("Code Entries",   NULL, &state.show_entries_list);
                ImGui::MenuItem("Strings",        NULL, &state.show_strings_list);
                ImGui::MenuItem("Symbols",        NULL, &state.show_symbols_editor);
                ImGui::MenuItem("Types",          NULL, &state.show_types_editor);
                ImGui::MenuItem("Pattern Search", NULL, &state.show_pattern_search);
                ImGui::MenuItem("RAM References", NULL, &state.show_ram_refs);
                ImGui::MenuItem("Ref Exclusions",    NULL, &state.show_ref_exclusions);
                ImGui::MenuItem("Code Candidates",    NULL, &state.show_code_candidates);
                ImGui::MenuItem("Inline Candidates", NULL, &state.show_inline_candidates);
                ImGui::MenuItem("Warnings",       NULL, &state.show_warnings);
                ImGui::MenuItem("ROM Map",        NULL, &state.show_rom_map);
                ImGui::MenuItem("DMD Frames",     NULL, &state.show_dmd_list);
                ImGui::MenuItem("Sprites",        NULL, &state.show_sprite_list);
                ImGui::MenuItem("Sprite Gallery", NULL, &state.show_sprite_gallery);
                ImGui::Separator();
                ImGui::MenuItem("Details",     NULL, &state.show_details);
                ImGui::MenuItem("References",  NULL, &state.show_refs);
                ImGui::MenuItem("DMD Preview", NULL, &state.show_dmd);
                ImGui::MenuItem("Editor",      NULL, &state.show_edit);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Layout")) {
                    state.request_layout_reset = true;
                }
                ImGui::Separator();
                ImGui::TextDisabled("Font");
                ImGui::SetNextItemWidth(180.0f);
                float fs = g_ui_font_scale;
                if (ImGui::SliderFloat("##fontscale", &fs, FONT_SCALE_MIN, FONT_SCALE_MAX,
                                       "Zoom %.2fx")) {
                    set_ui_font_scale(fs);
                }
                if (ImGui::MenuItem("Zoom In",    "Ctrl++")) set_ui_font_scale(g_ui_font_scale + FONT_SCALE_STEP);
                if (ImGui::MenuItem("Zoom Out",   "Ctrl+-")) set_ui_font_scale(g_ui_font_scale - FONT_SCALE_STEP);
                if (ImGui::MenuItem("Reset Zoom", "Ctrl+0")) set_ui_font_scale(1.0f);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("Show Shortcuts", "H")) {
                    state.show_help = !state.show_help;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();

        if (state.request_layout_reset) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, io.DisplaySize);

            ImGuiID dock_main_id = dockspace_id;
            ImGuiID dock_left_id   = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left,
                                                                   0.20f, NULL, &dock_main_id);
            ImGuiID dock_right_id  = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right,
                                                                   0.25f, NULL, &dock_main_id);
            ImGuiID dock_bottom_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down,
                                                                   0.30f, NULL, &dock_main_id);

            ImGui::DockBuilderDockWindow("Disassembly", dock_main_id);
            ImGui::DockBuilderDockWindow("Navigator",   dock_left_id);
            ImGui::DockBuilderDockWindow("Banks",       dock_left_id);
            ImGui::DockBuilderDockWindow("Labels",      dock_left_id);
            ImGui::DockBuilderDockWindow("Bookmarks",   dock_left_id);
            ImGui::DockBuilderDockWindow("Transitions", dock_left_id);
            ImGui::DockBuilderDockWindow("Details",     dock_right_id);
            ImGui::DockBuilderDockWindow("DMD",         dock_right_id);
            ImGui::DockBuilderDockWindow("DMD Frames",  dock_bottom_id);
            ImGui::DockBuilderDockWindow("Edit",        dock_right_id);
            ImGui::DockBuilderDockWindow("ROM Info",            dock_right_id);
            ImGui::DockBuilderDockWindow("Match from Reference", dock_bottom_id);
            ImGui::DockBuilderDockWindow("ROM Compare",          dock_bottom_id);
            ImGui::DockBuilderDockWindow("Coverage",             dock_bottom_id);
            ImGui::DockBuilderDockWindow("Call Graph",     dock_bottom_id);
            ImGui::DockBuilderDockWindow("References",     dock_bottom_id);
            ImGui::DockBuilderDockWindow("Hardware",       dock_bottom_id);
            ImGui::DockBuilderDockWindow("Tables",         dock_bottom_id);
            ImGui::DockBuilderDockWindow("Hex",            dock_bottom_id);
            ImGui::DockBuilderDockWindow("Sprites",        dock_bottom_id);
            ImGui::DockBuilderDockWindow("Sprite Gallery", dock_bottom_id);
            ImGui::DockBuilderDockWindow("Inline Sigs",    dock_bottom_id);
            ImGui::DockBuilderDockWindow("Code Entries",   dock_bottom_id);
            ImGui::DockBuilderDockWindow("Strings",        dock_bottom_id);
            ImGui::DockBuilderDockWindow("Symbols",        dock_bottom_id);
            ImGui::DockBuilderDockWindow("Types",          dock_bottom_id);
            ImGui::DockBuilderDockWindow("Pattern Search", dock_bottom_id);
            ImGui::DockBuilderDockWindow("RAM References", dock_bottom_id);
            ImGui::DockBuilderDockWindow("Ref Exclusions",  dock_bottom_id);
            ImGui::DockBuilderDockWindow("Code Candidates",   dock_bottom_id);
            ImGui::DockBuilderDockWindow("Inline Candidates", dock_bottom_id);
            ImGui::DockBuilderDockWindow("Warnings",          dock_bottom_id);
            ImGui::DockBuilderDockWindow("Global Search",   dock_bottom_id);
            ImGui::DockBuilderDockWindow("ROM Map",        dock_right_id);

            ImGui::DockBuilderFinish(dockspace_id);
            state.request_layout_reset = false;
        }

        if (state.show_navigator) {
            ImGui::Begin("Navigator", &state.show_navigator);
            if (ImGui::Button("Back")) {
                history_jump(&state, 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Forward")) {
                history_jump(&state, 0);
            }
            ImGui::SameLine();
            if (ImGui::Button("Help")) {
                state.show_help = !state.show_help;
            }
            if (state.request_focus_goto) {
                ImGui::SetKeyboardFocusHere();
                state.request_focus_goto = 0;
            }
            ImGui::InputText("Goto", state.goto_input, 64);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                select_line_by_address(document, &state);
            }
            ImGui::SameLine();
            if (ImGui::Button("Jump")) {
                select_line_by_address(document, &state);
            }
            if (state.request_focus_filter) {
                ImGui::SetKeyboardFocusHere();
                state.request_focus_filter = 0;
            }
            ImGui::InputText("Filter", state.filter_input, 128);
            if (ImGui::Button("Next boundary")) {
                jump_primary_transition(document, &state, 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Prev boundary")) {
                jump_primary_transition(document, &state, 0);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Recent");
            {
                size_t hcount = state.history_back.size();
                size_t hshow  = hcount > 25 ? 25 : hcount;
                for (size_t hi = 0; hi < hshow; hi++) {
                    size_t hli = state.history_back[hcount - 1 - hi];
                    if (hli >= (size_t)document->line_count) {
                        continue;
                    }
                    const auto *hl = &document->lines[hli];
                    char hbuf[64];
                    if (hl->has_location) {
                        snprintf(hbuf, sizeof(hbuf), "B%02x_A%04x",
                                 hl->bank, (unsigned)hl->cpu_addr & 0xffff);
                    } else {
                        snprintf(hbuf, sizeof(hbuf), "line %lu", (unsigned long)hli);
                    }
                    bool is_bm = false;
                    if (hl->has_location) {
                        for (const auto &bk : state.bookmarks)
                            if (bk.bank == hl->bank && bk.addr == hl->cpu_addr)
                                { is_bm = true; break; }
                    }
                    ImGui::PushID((int)(2000 + hi));
                    if (is_bm)
                        ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(0.60f, 0.30f, 0.85f, 0.40f));
                    if (ImGui::SmallButton(hbuf)) {
                        select_line(&state, hli, 1);
                    }
                    if (is_bm) ImGui::PopStyleColor();
                    if (hl->has_location) {
                        std::string lbl = label_at_address(document, &state,
                                                           hl->bank, hl->cpu_addr);
                        if (!lbl.empty()) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("%s", lbl.c_str());
                        }
                    }
                    ImGui::PopID();
                }
            }
            ImGui::End();
        }

        if (state.show_labels) {
            ImGui::Begin("Labels", &state.show_labels);
            render_label_list(document, &state);
            ImGui::End();
        }
        if (state.show_banks) {
            ImGui::Begin("Banks", &state.show_banks);
            render_bank_list(project, document, &state);
            ImGui::End();
        }
        if (state.show_transitions) {
            ImGui::Begin("Transitions", &state.show_transitions);
            render_transition_list(document, &state);
            ImGui::End();
        }
        if (state.show_bookmarks) {
            ImGui::Begin("Bookmarks", &state.show_bookmarks);
            render_bookmark_list(document, &state);
            ImGui::End();
        }
        if (state.show_disasm) {
            ImGui::Begin("Disassembly", &state.show_disasm);
            render_line_table(project, &document, &state);
            ImGui::End();
        }

        if (state.show_refs) {
            ImGui::Begin("References", &state.show_refs);
            {
                uint8_t refs_bank = 0;
                uint32_t refs_addr = 0;
                bool has_refs_addr = false;

                if (state.refs_pinned) {
                    refs_bank = state.refs_pinned_bank;
                    refs_addr = state.refs_pinned_addr;
                    has_refs_addr = true;
                } else if (state.selected_line < document->line_count &&
                           document->lines[state.selected_line].has_location) {
                    const auto *l = &document->lines[state.selected_line];
                    refs_bank = l->bank;
                    refs_addr = l->cpu_addr;
                    has_refs_addr = true;
                }

                if (state.refs_pinned) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.35f, 0.05f, 1.0f));
                    if (ImGui::SmallButton("Unpin")) {
                        state.refs_pinned = false;
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    std::string lbl = label_at_address(document, &state, refs_bank, refs_addr);
                    char hdr[192];
                    if (lbl.empty()) {
                        snprintf(hdr, sizeof(hdr), "Pinned: B%02x_A%04x",
                                 refs_bank, (unsigned)refs_addr & 0xffffu);
                    } else {
                        snprintf(hdr, sizeof(hdr), "Pinned: %s", lbl.c_str());
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.70f, 0.20f, 1.0f));
                    ImGui::TextUnformatted(hdr);
                    ImGui::PopStyleColor();
                } else {
                    if (has_refs_addr) {
                        if (ImGui::SmallButton("Pin")) {
                            state.refs_pinned = true;
                            state.refs_pinned_bank = refs_bank;
                            state.refs_pinned_addr = refs_addr;
                        }
                    } else {
                        ImGui::BeginDisabled();
                        ImGui::SmallButton("Pin");
                        ImGui::EndDisabled();
                    }
                }
                ImGui::Separator();

                if (has_refs_addr) {
                    auto in  = find_incoming_refs(project, document, &state, refs_bank, refs_addr);
                    auto out = find_outgoing_refs(project, document, &state, refs_bank, refs_addr);
                    ImGui::TextUnformatted("Incoming");
                    if (in.empty()) {
                        ImGui::TextDisabled("none");
                    } else {
                        for (size_t ri = 0; ri < in.size(); ri++) {
                            const auto &r = in[ri];
                            char c[192];
                            snprintf(c, 192, "%s B%02x_A%04x", r.kind.c_str(), r.bank, r.cpu_addr);
                            ImGui::PushID((int)ri);
                            if (ImGui::SmallButton(c)) {
                                select_line(&state, r.line_index, 1);
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::Separator();
                    ImGui::TextUnformatted("Outgoing");
                    if (out.empty()) {
                        ImGui::TextDisabled("none");
                    } else {
                        for (size_t ri = 0; ri < out.size(); ri++) {
                            const auto &r = out[ri];
                            char c[192];
                            snprintf(c, 192, "%s B%02x_A%04x", r.kind.c_str(), r.bank, r.cpu_addr);
                            ImGui::PushID((int)(1000 + ri));
                            if (ImGui::SmallButton(c)) {
                                select_line(&state, r.line_index, 1);
                            }
                            ImGui::PopID();
                        }
                    }
                } else {
                    ImGui::TextDisabled("No selection.");
                }
            }
            ImGui::End();
        }

        if (state.show_dmd) {
            ImGui::Begin("DMD", &state.show_dmd);
            render_dmd_view(project, document, &state);
            ImGui::End();
        }
        if (state.show_call_graph) {
            ImGui::Begin("Call Graph", &state.show_call_graph);
            render_call_graph(project, document, &state);
            ImGui::End();
        }
        if (state.show_tables) {
            ImGui::Begin("Tables", &state.show_tables);
            render_tables_window(project, &document, &state);
            ImGui::End();
        }
        if (state.show_hardware) {
            ImGui::Begin("Hardware", &state.show_hardware);
            render_hardware_window(project, document, &state);
            ImGui::End();
        }
        if (state.show_symbols_editor) {
            ImGui::Begin("Symbols", &state.show_symbols_editor);
            render_symbols_editor(project, document, &state);
            ImGui::End();
        }
        if (state.show_types_editor) {
            ImGui::Begin("Types", &state.show_types_editor);
            render_types_editor(project, &state);
            ImGui::End();
        }
        if (state.show_rom_info) {
            ImGui::Begin("ROM Info", &state.show_rom_info);
            render_rom_info(project, &state);
            ImGui::End();
        }
        if (state.show_match_window) {
            ImGui::Begin("Match from Reference", &state.show_match_window);
            render_match_window(project, &document, &state);
            ImGui::End();
        }
        if (state.show_rom_compare) {
            ImGui::Begin("ROM Compare", &state.show_rom_compare);
            render_rom_compare_window(project, &document, &state);
            ImGui::End();
        }
        if (state.show_coverage) {
            ImGui::Begin("Coverage", &state.show_coverage);
            render_coverage_window(project, &document, &state);
            ImGui::End();
        }
        if (state.show_inline_list) {
            ImGui::Begin("Inline Sigs", &state.show_inline_list);
            render_inline_list(project, document, &state);
            ImGui::End();
        }
        if (state.show_entries_list) {
            ImGui::Begin("Code Entries", &state.show_entries_list);
            render_entries_list(project, &document, &state);
            ImGui::End();
        }
        if (state.show_strings_list) {
            ImGui::Begin("Strings", &state.show_strings_list);
            render_strings_list(project, document, &state);
            ImGui::End();
        }
        if (state.show_pattern_search) {
            ImGui::Begin("Pattern Search", &state.show_pattern_search);
            render_pattern_search(project, &document, &state);
            ImGui::End();
        }
        if (state.show_ram_refs) {
            ImGui::Begin("RAM References", &state.show_ram_refs);
            render_ram_refs(project, document, &state);
            ImGui::End();
        }
        if (state.show_ref_exclusions) {
            ImGui::Begin("Ref Exclusions", &state.show_ref_exclusions);
            render_ref_exclusions(project, &document, &state);
            ImGui::End();
        }
        if (state.show_code_candidates) {
            ImGui::Begin("Code Candidates", &state.show_code_candidates);
            render_code_candidates(project, &document, &state);
            ImGui::End();
        }
        if (state.show_inline_candidates) {
            ImGui::Begin("Inline Candidates", &state.show_inline_candidates);
            render_inline_candidates(project, &document, &state);
            ImGui::End();
        }
        if (state.show_warnings) {
            ImGui::Begin("Warnings", &state.show_warnings);
            render_warnings_view(project, &document, &state);
            ImGui::End();
        }
        render_xref_popup(project, document, &state);
        if (state.show_search_window) {
            ImGui::Begin("Global Search", &state.show_search_window);
            render_global_search(document, &state);
            ImGui::End();
        }
        if (state.show_rom_map) {
            ImGui::Begin("ROM Map", &state.show_rom_map);
            render_rom_map(project, &document, &state);
            ImGui::End();
        }
        if (state.show_dmd_list) {
            ImGui::Begin("DMD Frames", &state.show_dmd_list);
            render_dmd_list_window(project, document, &state);
            ImGui::End();
        }
        if (state.show_sprite_gallery) {
            ImGui::Begin("Sprite Gallery", &state.show_sprite_gallery);
            render_sprite_gallery_window(project, &document, &state);
            ImGui::End();
        }
        if (state.show_sprite_list) {
            ImGui::Begin("Sprites", &state.show_sprite_list);
            render_sprite_list_window(project, &document, &state);
            ImGui::End();
        }
        if (state.show_edit) {
            ImGui::Begin("Edit", &state.show_edit);
            render_editor(project, &document, &original_snapshot, &state);
            ImGui::End();
        }
        if (state.show_hex) {
            ImGui::Begin("Hex", &state.show_hex);
            render_hex_view(project, &document, &state);
            ImGui::End();
        }

        if (state.show_details) {
            ImGui::Begin("Details", &state.show_details);
            if (state.selected_line < document->line_count) {
                const ApexRenderedLine *line = &document->lines[state.selected_line];
                LineByteSpan span = selected_line_span(project, document, &state);
                std::vector<LineTargetEntry> targets = find_line_targets(document, &state, line);
                ImGui::Text("Line: %lu  Block: %s",
                            (unsigned long)state.selected_line, block_name(line->block_kind));
                if (line->has_location) {
                    ImGui::Text("Address: B%02x_A%04x  ROM: 0x%06lx",
                                line->bank, (unsigned)line->cpu_addr & 0xffffu,
                                (unsigned long)line->rom_addr);
                    const LabelSet *ls = (line->bank == 0xffu) ? &project->system_labels : NULL;
                    if (!ls) {
                        int bi = bank_index_for_far_ref(project->rom.data, project->banks,
                                                        line->bank);
                        if (bi >= 0) {
                            ls = &project->bank_labels[bi];
                        }
                    }
                    if (ls) {
                        for (size_t li = 0; li < ls->count; li++) {
                            if (ls->items[li].addr == line->cpu_addr) {
                                const Label *lbl = &ls->items[li];
                                if (lbl->explain) {
                                    ImGui::Text("Origin: %s", lbl->explain);
                                }
                                if (lbl->kind_explain) {
                                    ImGui::Text("Classification: %s", lbl->kind_explain);
                                }
                                if (lbl->is_conflict) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.1f, 1.0f));
                                    ImGui::TextUnformatted("CONFLICT: address has contradictory classifications");
                                    ImGui::PopStyleColor();
                                }
                                break;
                            }
                        }
                    }
                }
                if (span.valid) {
                    ImGui::Text("Span: %lu bytes", (unsigned long)(span.end - span.start));
                }
                ImGui::Separator();
                if (!targets.empty()) {
                    ImGui::TextUnformatted("Targets:");
                    for (auto &t : targets) {
                        char c[128];
                        snprintf(c, 128, "%s (B%02x_A%04x)", t.name.c_str(), t.bank,
                                 (unsigned)t.cpu_addr & 0xffffu);
                        if (ImGui::SmallButton(c)) {
                            select_line(&state, t.line_index, 1);
                        }
                    }
                    ImGui::Separator();
                }
                ImGui::TextWrapped("%.*s", (int)line->length, line->text);
            }
            ImGui::End();
        }

        if (state.show_help) {
            ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
            ImGui::Begin("Keyboard Shortcuts", &state.show_help);

            auto krow = [](const char *keys, const char *desc) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", keys);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(desc);
            };
            constexpr ImGuiTableFlags kTableFlags =
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;

            ImGui::SeparatorText("Navigate");
            if (ImGui::BeginTable("##nav", 2, kTableFlags)) {
                ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("##d", ImGuiTableColumnFlags_WidthStretch);
                krow("J / Down Arrow",     "Move selection down");
                krow("K / Up Arrow",       "Move selection up");
                krow("N / P",              "Next / prev block boundary");
                krow("F / Enter",          "Follow link / jump to target");
                krow("[ / ]",              "History back / forward");
                krow("Alt+\xe2\x86\x90 / Alt+\xe2\x86\x92", "History back / forward (alt)");
                krow("G",                  "Go to address");
                krow("/",                  "Focus filter bar");
                ImGui::EndTable();
            }

            ImGui::SeparatorText("Classify");
            if (ImGui::BeginTable("##cls", 2, kTableFlags)) {
                ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("##d", ImGuiTableColumnFlags_WidthStretch);
                krow("C",    "Mark as code");
                krow("D",    "Mark as data");
                krow("S",    "Mark as string");
                krow("T",    "Mark as table (uses current schema)");
                krow("1",    "Repeat last classification");
                krow("Del",  "Clear classification");
                ImGui::EndTable();
            }

            ImGui::SeparatorText("Edit");
            if (ImGui::BeginTable("##edt", 2, kTableFlags)) {
                ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("##d", ImGuiTableColumnFlags_WidthStretch);
                krow("L",         "Edit label");
                krow("Shift+D",   "Edit comment / doc");
                krow("B",         "Add bookmark");
                krow("Shift+Click", "Extend selection range");
                ImGui::EndTable();
            }

            ImGui::SeparatorText("View");
            if (ImGui::BeginTable("##viw", 2, kTableFlags)) {
                ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("##d", ImGuiTableColumnFlags_WidthStretch);
                krow("X",        "Show incoming XRefs");
                krow("H",        "Show / hide this window");
                krow("Ctrl+F",   "Global search");
                krow("Ctrl+C",   "Copy selection");
                krow("Ctrl+S",   "Save config overlay");
                krow("Ctrl+Z",   "Undo last edit");
                krow("Ctrl+Y",   "Redo");
                krow("F5",       "Re-analyze");
                krow("Shift+F5", "Force full re-analyze");
                krow("Ctrl +/-", "Font zoom in / out");
                krow("Ctrl+0",   "Reset font zoom");
                ImGui::EndTable();
            }

            ImGui::SeparatorText("DMD Scrub");
            if (ImGui::BeginTable("##dmd", 2, kTableFlags)) {
                ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("##d", ImGuiTableColumnFlags_WidthStretch);
                krow("+ / -",    "Advance / rewind 1 byte (Shift: 32)");
                krow("0",        "Reset offset");
                krow("M",        "Mark DMD fullframe at current offset");
                ImGui::EndTable();
            }

            ImGui::SeparatorText("Tips");
            ImGui::BulletText("Hex View: click byte to inspect; syncs with disassembly.");
            ImGui::BulletText("Pattern Search: e.g.  BD ?? 7E  (?? = any byte).");
            ImGui::BulletText("RAM References: find all instructions accessing a RAM address.");
            ImGui::BulletText("Double-click a label token in a ; referenced_by line to navigate.");

            ImGui::End();
        }

        if (!io.WantTextInput) {
            if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Z) ||
                               ImGui::IsKeyPressed(ImGuiKey_Y))) {
                bool redo = ImGui::IsKeyPressed(ImGuiKey_Y) ||
                            (ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyShift);
                uint8_t cb = 0xffu; uint32_t ca = 0u;
                selected_address(document, &state, &cb, &ca);
                int rc = redo ? apex_project_redo(project) : apex_project_undo(project);
                if (rc == 0)
                    rerender_and_reselect(project, &document, &state, cb, ca);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_H)) {
                state.show_help = !state.show_help;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_G)) {
                state.request_focus_goto = 1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Slash)) {
                state.request_focus_filter = 1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_L)) {
                state.request_focus_label = 1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_D)) {
                if (io.KeyShift) {
                    state.request_focus_doc = 1;
                } else {
                    uint8_t b;
                    uint32_t a;
                    if (selected_address(document, &state, &b, &a)) {
                        char spec[32];
                        snprintf(spec, sizeof(spec), "bytes[%d]",
                                 state.edit_data_length > 0 ? state.edit_data_length : 1);
                        apply_data_at_selection(project, &document, &state, spec);
                    }
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_S)) {
                if (io.KeyCtrl) {
                    state.request_save_overlay = 1;
                } else {
                    apply_string_at_selection(project, &document, &state);
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_T)) {
                char spec[320] = "counted(ptr16_data)";
                if (state.edit_schema_count > 0) {
                    char schema[256];
                    fields_to_spec(schema, sizeof(schema),
                                   state.edit_schema_fields, state.edit_schema_count);
                    if (state.edit_table_is_rows) {
                        snprintf(spec, sizeof(spec), "rows[%d](%s)",
                                 state.edit_table_rows, schema);
                    } else {
                        snprintf(spec, sizeof(spec), "counted(%s)", schema);
                    }
                }
                apply_table_at_selection(project, &document, &state, spec);
            }
            /* Ctrl +/-/0 = font zoom; the same keys without Ctrl drive DMD scrub. */
            bool key_plus  = ImGui::IsKeyPressed(ImGuiKey_Equal) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadAdd);
            bool key_minus = ImGui::IsKeyPressed(ImGuiKey_Minus) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract);
            bool key_zero  = ImGui::IsKeyPressed(ImGuiKey_0) ||
                             ImGui::IsKeyPressed(ImGuiKey_Keypad0);
            if (io.KeyCtrl) {
                if (key_plus)  set_ui_font_scale(g_ui_font_scale + FONT_SCALE_STEP);
                if (key_minus) set_ui_font_scale(g_ui_font_scale - FONT_SCALE_STEP);
                if (key_zero)  set_ui_font_scale(1.0f);
            } else {
                if (key_plus)  state.dmd_scrub_offset += io.KeyShift ? 32 : 1;
                if (key_minus) state.dmd_scrub_offset -= io.KeyShift ? 32 : 1;
                if (key_zero)  state.dmd_scrub_offset = 0;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_M)) {
                uint8_t b;
                uint32_t a;
                if (selected_address(document, &state, &b, &a)) {
                    uint32_t ta = (uint32_t)((int64_t)a + state.dmd_scrub_offset);
                    if (apex_project_set_kind(project, 1, b, ta, APEX_KIND_DATA,
                                             "dmd_fullframe") == 0) {
                        rerender_and_reselect(project, &document, &state, b, ta);
                        state.dmd_scrub_offset = 0;
                    }
                }
            }
            if ((io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) ||
                ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
                history_jump(&state, 1);
            }
            if ((io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ||
                ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
                history_jump(&state, 0);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_B)) {
                uint8_t b;
                uint32_t a;
                if (selected_address(document, &state, &b, &a)) {
                    char n[64];
                    snprintf(n, 64, "Bookmark @ B%02x_%04x", b, a);
                    state.bookmarks.push_back({b, a, n});
                    state.request_focus_new_bookmark = 1;
                }
            }
            if (!state.hex_window_focused) {
                if (ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                    move_selection_relative(document, &state, 1);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                    move_selection_relative(document, &state, -1);
                }
                int page = (int)(ImGui::GetIO().DisplaySize.y /
                                 ImGui::GetTextLineHeightWithSpacing()) - 4;
                if (page < 5) page = 5;
                if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
                    move_selection_relative(document, &state, page);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
                    move_selection_relative(document, &state, -page);
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_N)) {
                jump_primary_transition(document, &state, 1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_P)) {
                jump_primary_transition(document, &state, 0);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_1) ||
                ImGui::IsKeyPressed(ImGuiKey_Keypad1)) {
                repeat_last_classify(project, &document, &state);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_C)) {
                if (io.KeyCtrl) {
                    copy_selection_to_clipboard(document, &state);
                } else {
                    apply_code_at_selection(project, &document, &state);
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_X)) {
                if (state.selected_line < document->line_count) {
                    const auto *l = &document->lines[state.selected_line];
                    if (l->has_location) {
                        state.request_xref_popup = true;
                        state.xref_popup_bank = l->bank;
                        state.xref_popup_addr = l->cpu_addr;
                    }
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                clear_kind_at_selection(project, &document, &state);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
                uint8_t cur_b = 0xffu; uint32_t cur_a = 0u;
                selected_address(document, &state, &cur_b, &cur_a);
                if (io.KeyShift)
                    apex_project_invalidate(project, APEX_DIRTY_ANALYSIS);
                rerender_and_reselect(project, &document, &state, cur_b, cur_a);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
                    state.show_search_window = true;
                    state.request_focus_global_search = 1;
                } else if (state.selected_line < document->line_count) {
                    const auto *l = &document->lines[state.selected_line];
                    uint8_t tb;
                    uint32_t ta;
                    int tf;
                    size_t tli;
                    if (resolve_pointer_target(project, l, &tb, &ta, &tf) &&
                        apex_render_find_line_by_address(document, tb, ta, &tli)) {
                        select_line(&state, tli, 1);
                    } else {
                        follow_selected_link(document, &state);
                    }
                }
            }
        }

        if (want_quit) {
            ImGui::OpenPopup("Unsaved Changes##quit");
            want_quit = false;
        }
        if (ImGui::BeginPopupModal("Unsaved Changes##quit", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("The overlay has unsaved changes.");
            ImGui::Separator();
            if (ImGui::Button("Save & Exit")) {
                std::string st;
                write_delta_overlay(project, &original_snapshot,
                                    state.save_path_input, state.base_config_path, &st);
                done = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Exit Without Saving")) {
                done = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        /* --- Save INI As dialog --- */
        if (want_save_ini_as) {
            IGFD::FileDialogConfig cfg;
            cfg.path     = dir_of(state.base_config_path);
            cfg.fileName = "config.ini";
            cfg.flags    = ImGuiFileDialogFlags_ConfirmOverwrite;
            ImGuiFileDialog::Instance()->OpenDialog("SaveIniAs", "Save INI As", ".ini", cfg);
            want_save_ini_as = false;
        }
        if (ImGuiFileDialog::Instance()->Display("SaveIniAs",
                ImGuiWindowFlags_NoCollapse, ImVec2(600, 400))) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
                std::string st;
                if (write_full_config(project, p.c_str(), &st) > 0)
                    set_status(&state, ("saved: " + p).c_str());
                else
                    set_status(&state, ("save failed: " + st).c_str());
            }
            ImGuiFileDialog::Instance()->Close();
        }

        /* --- RAM map import (nvram-maps JSON) --- */
        if (want_nvram_import) {
            IGFD::FileDialogConfig cfg;
            cfg.path = dir_of(state.base_config_path);
            ImGuiFileDialog::Instance()->OpenDialog("NvramImport", "Import RAM Map (JSON)",
                                                    ".json", cfg);
            want_nvram_import = false;
        }
        if (ImGuiFileDialog::Instance()->Display("NvramImport",
                ImGuiWindowFlags_NoCollapse, ImVec2(600, 400))) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
                std::string st;
                if (nvram_prepare_import(project, &state, p.c_str(), &st) != 0)
                    set_status(&state, ("RAM import failed: " + st).c_str());
                else
                    snprintf(state.nvram_source_path, sizeof(state.nvram_source_path),
                             "%s", p.c_str()); /* reuse as export template */
            }
            ImGuiFileDialog::Instance()->Close();
        }
        if (state.show_nvram_import) {
            ImGui::SetNextWindowSize(ImVec2(680, 460), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Import RAM Map", &state.show_nvram_import)) {
                render_nvram_import_window(project, &document, &state);
            }
            ImGui::End();
            if (!state.show_nvram_import) state.nvram_import_rows.clear();
        }

        /* --- RAM map export --- */
        if (want_nvram_export) {
            IGFD::FileDialogConfig cfg;
            cfg.path     = dir_of(state.base_config_path);
            cfg.fileName = "ram_map.json";
            cfg.flags    = ImGuiFileDialogFlags_ConfirmOverwrite;
            ImGuiFileDialog::Instance()->OpenDialog("NvramExport", "Export RAM Map (JSON)",
                                                    ".json", cfg);
            want_nvram_export = false;
        }
        if (ImGuiFileDialog::Instance()->Display("NvramExport",
                ImGuiWindowFlags_NoCollapse, ImVec2(600, 400))) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
                std::string st;
                if (nvram_export(project, p.c_str(), state.nvram_source_path, &st) == 0)
                    set_status(&state, ("RAM map exported: " + p +
                                        (state.nvram_source_path[0] ? " (merged)" : "")).c_str());
                else
                    set_status(&state, ("RAM export failed: " + st).c_str());
            }
            ImGuiFileDialog::Instance()->Close();
        }

        if (want_consolidate) {
            ImGui::OpenPopup("Consolidate##consolidate");
            want_consolidate = false;
        }
        if (ImGui::BeginPopupModal("Consolidate##consolidate", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Overwrite base INI with merged state:");
            ImGui::TextDisabled("%s", state.base_config_path);
            ImGui::TextUnformatted("The overlay will be reset to empty.");
            ImGui::Separator();
            if (ImGui::Button("Consolidate")) {
                std::string st;
                if (write_full_config(project, state.base_config_path, &st) > 0) {
                    original_snapshot = build_original_snapshot(project);
                    /* Reset overlay to an empty stub so it no longer diverges */
                    FILE *ov = fopen(state.save_path_input, "w");
                    if (ov) {
                        const char *bp = strrchr(state.base_config_path, '/');
                        if (!bp) bp = strrchr(state.base_config_path, '\\');
                        const char *base_name = bp ? bp + 1 : state.base_config_path;
                        fprintf(ov, "; Apex ImGui overlay\ninclude = %s\n", base_name);
                        fclose(ov);
                    }
                    state.overlay_dirty = false;
                    set_status(&state, "consolidated into base INI");
                } else {
                    set_status(&state, ("consolidate failed: " + st).c_str());
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    save_session(rom_path, config_path, &state, document);
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    apex_free_code_candidates(&state.code_candidates);
    apex_free_inline_candidates(&state.inline_candidates);
    apex_project_free(project);
    return 0;
}
