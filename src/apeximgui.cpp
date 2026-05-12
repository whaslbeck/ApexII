#include "apeximgui_core.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
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

int main(int argc, char **argv)
{
    ApexProject *project;
    const ApexRenderedDocument *document;
    OriginalSnapshot original_snapshot;
    SDL_Window *window = NULL;
    SDL_GLContext gl_context = NULL;
    UiState state = {};
    bool done = false;
    bool want_quit = false;
    char rom_path[1024] = "";
    char config_path[1024] = "";

    if (argc == 3) {
        strncpy(rom_path,    argv[1], 1023);
        strncpy(config_path, argv[2], 1023);
    } else if (argc == 2) {
        strncpy(rom_path, argv[1], 1023);
        config_path[0] = '\0';
    } else if (argc == 1) {
        if (!load_global_session(rom_path, config_path)) {
            fprintf(stderr, "usage: %s ROM [CONFIG]\n", argv[0]);
            return 2;
        }
    } else {
        fprintf(stderr, "usage: %s ROM [CONFIG]\n", argv[0]);
        return 2;
    }

    /* If an overlay from a previous session exists, load it as the effective config
       so all prior work is available. The snapshot is always built from the base config
       only, ensuring the saved delta covers all sessions cumulatively. */
    const char *effective_config = config_path[0] ? config_path : NULL;
    /* config_path can be up to 1023 chars; ".apeximgui.ini" is 14 chars → need 1038 + NUL */
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
        return 1;
    }
    document = apex_project_render(project, 0, 0);
    if (!document) {
        fprintf(stderr, "failed to render\n");
        return 1;
    }
    original_snapshot = build_config_snapshot(config_path[0] ? config_path : NULL);
    load_rom_session(rom_path, &state, document);

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

    window = SDL_CreateWindow("ApexII ImGui",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1600, 950,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                              SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 150");

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
    state.show_bookmarks     = true;
    state.show_transitions   = false;
    state.request_layout_reset = true;

    if (config_path[0]) {
        /* save_path_input is 512 bytes; ".apeximgui.ini" is 14 chars → cap path at 497 chars */
        snprintf(state.save_path_input, sizeof(state.save_path_input), "%.*s.apeximgui.ini",
                 (int)(sizeof(state.save_path_input) - 15), config_path);
        strncpy(state.base_config_path, config_path, 1023);
        state.base_config_path[1023] = '\0';
    } else {
        strcpy(state.save_path_input, "apeximgui.ini");
        state.base_config_path[0] = '\0';
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
        sync_editor_state(project, document, &state);

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save Overlay", "Ctrl+S")) {
                    state.request_save_overlay = 1;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    if (state.overlay_dirty) { want_quit = true; } else { done = true; }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Windows")) {
                ImGui::MenuItem("Navigator",    "N", &state.show_navigator);
                ImGui::MenuItem("Disassembly",  "D", &state.show_disasm);
                ImGui::MenuItem("Hex Inspector","H", &state.show_hex);
                ImGui::Separator();
                ImGui::MenuItem("Banks",       NULL, &state.show_banks);
                ImGui::MenuItem("Labels",      NULL, &state.show_labels);
                ImGui::MenuItem("Bookmarks",   NULL, &state.show_bookmarks);
                ImGui::MenuItem("Transitions", NULL, &state.show_transitions);
                ImGui::Separator();
                ImGui::MenuItem("Call Graph",     NULL, &state.show_call_graph);
                ImGui::MenuItem("Hardware",       NULL, &state.show_hardware);
                ImGui::MenuItem("Tables",         NULL, &state.show_tables);
                ImGui::MenuItem("Pattern Search", NULL, &state.show_pattern_search);
                ImGui::MenuItem("RAM References", NULL, &state.show_ram_refs);
                ImGui::Separator();
                ImGui::MenuItem("Details",     NULL, &state.show_details);
                ImGui::MenuItem("References",  NULL, &state.show_refs);
                ImGui::MenuItem("DMD Preview", NULL, &state.show_dmd);
                ImGui::MenuItem("Editor",      NULL, &state.show_edit);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Layout")) {
                    state.request_layout_reset = true;
                }
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
            ImGui::DockBuilderDockWindow("Edit",        dock_right_id);
            ImGui::DockBuilderDockWindow("Call Graph",     dock_bottom_id);
            ImGui::DockBuilderDockWindow("References",     dock_bottom_id);
            ImGui::DockBuilderDockWindow("Hardware",       dock_bottom_id);
            ImGui::DockBuilderDockWindow("Tables",         dock_bottom_id);
            ImGui::DockBuilderDockWindow("Hex",            dock_bottom_id);
            ImGui::DockBuilderDockWindow("Pattern Search", dock_bottom_id);
            ImGui::DockBuilderDockWindow("RAM References", dock_bottom_id);

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
            if (ImGui::Button("Next code_to_data")) {
                jump_to_transition(document, &state, APEX_RENDER_TRANSITION_CODE_TO_DATA, 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Prev code_to_data")) {
                jump_to_transition(document, &state, APEX_RENDER_TRANSITION_CODE_TO_DATA, 0);
            }
            if (ImGui::Button("Next table_to_data")) {
                jump_to_transition(document, &state, APEX_RENDER_TRANSITION_TABLE_TO_DATA, 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Prev table_to_data")) {
                jump_to_transition(document, &state, APEX_RENDER_TRANSITION_TABLE_TO_DATA, 0);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Recent");
            {
                size_t hcount = state.history_back.size();
                size_t hshow  = hcount > 15 ? 15 : hcount;
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
                    ImGui::PushID((int)(2000 + hi));
                    if (ImGui::SmallButton(hbuf)) {
                        select_line(&state, hli, 1);
                    }
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
            if (state.selected_line < document->line_count &&
                document->lines[state.selected_line].has_location) {
                const auto *l = &document->lines[state.selected_line];
                auto in  = find_incoming_refs(project, document, &state, l->bank, l->cpu_addr);
                auto out = find_outgoing_refs(project, document, &state, l->bank, l->cpu_addr);
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
        render_xref_popup(project, document, &state);
        if (state.show_search_window) {
            ImGui::Begin("Global Search", &state.show_search_window);
            render_global_search(document, &state);
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
            ImGui::Begin("Help", &state.show_help);
            ImGui::BulletText("g/l: goto/label; /: filter; j/k: down/up; n/p: next/prev transition");
            ImGui::BulletText("c/d/s/t: mark code/data/string/table; Del: clear; f/Enter: follow link");
            ImGui::BulletText("X: XRefs; L/Shift+D: edit label/doc; B: bookmark; +/-: DMD Scrub; 0: reset; m: mark DMD");
            ImGui::BulletText("Ctrl+S: save overlay; Ctrl+F: global search; Ctrl+C: copy; Shift+Click: range");
            ImGui::BulletText("Hex View: Click byte to inspect; highlights disassembly selection.");
            ImGui::BulletText("Pattern Search: search ROM bytes, e.g. 'BD ?? 7E' (?? = wildcard).");
            ImGui::BulletText("RAM References: find all instructions accessing a RAM address.");
            ImGui::BulletText("Alt+Left/Right: history");
            ImGui::End();
        }

        if (!io.WantTextInput) {
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
                        apply_data_at_selection(project, &document, &state,
                            state.edit_spec_input[0] ? state.edit_spec_input : "bytes[1]");
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
                apply_table_at_selection(project, &document, &state,
                    state.edit_spec_input[0] ? state.edit_spec_input : "counted(ptr16_data)");
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Equal) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
                state.dmd_scrub_offset += io.KeyShift ? 32 : 1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Minus) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
                state.dmd_scrub_offset -= io.KeyShift ? 32 : 1;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_0) ||
                ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
                state.dmd_scrub_offset = 0;
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
            if (ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                move_selection_relative(document, &state, 1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                move_selection_relative(document, &state, -1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_N)) {
                jump_primary_transition(document, &state, 1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_P)) {
                jump_primary_transition(document, &state, 0);
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
    apex_project_free(project);
    return 0;
}
