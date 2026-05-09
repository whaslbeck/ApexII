#include "apeximgui_core.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
    ApexProject *project; const ApexRenderedDocument *document; OriginalSnapshot original_snapshot;
    SDL_Window *window = NULL; SDL_GLContext gl_context = NULL; UiState state = {};
    bool done = false; char rom_path[1024] = "", config_path[1024] = "";

    if (argc == 3) { strncpy(rom_path, argv[1], 1023); strncpy(config_path, argv[2], 1023); }
    else if (argc == 2) { strncpy(rom_path, argv[1], 1023); config_path[0] = '\0'; }
    else if (argc == 1) { if (!load_global_session(rom_path, config_path)) { fprintf(stderr, "usage: %s ROM [CONFIG]\n", argv[0]); return 2; } }
    else { fprintf(stderr, "usage: %s ROM [CONFIG]\n", argv[0]); return 2; }

    project = apex_project_open(rom_path, config_path[0] ? config_path : NULL);
    if (!project || apex_project_analyze(project) != 0) { fprintf(stderr, "failed to analyze\n"); return 1; }
    document = apex_project_render(project, 0, 0);
    if (!document) { fprintf(stderr, "failed to render\n"); return 1; }
    original_snapshot = build_original_snapshot(project);
    load_rom_session(rom_path, &state, document);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) { fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0); SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window = SDL_CreateWindow("ApexII ImGui", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 950, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return 1; }
    gl_context = SDL_GL_CreateContext(window); SDL_GL_MakeCurrent(window, gl_context); SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark(); ImGui_ImplSDL2_InitForOpenGL(window, gl_context); ImGui_ImplOpenGL3_Init("#version 150");

    state.editor_bound_line = (size_t)-1; state.labels_valid = false; state.graph_depth_in = 2; state.graph_depth_out = 2; state.graph_needs_rebuild = true;
    if (project->config_path && *project->config_path) snprintf(state.save_path_input, 512, "%s.apeximgui.ini", project->config_path); else strcpy(state.save_path_input, "apeximgui.ini");

    while (!done) {
        SDL_Event event; while (SDL_PollEvent(&event)) { ImGui_ImplSDL2_ProcessEvent(&event); if (event.type == SDL_QUIT) done = true; if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) done = true; }
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        sync_editor_state(project, document, &state); ImGui::DockSpaceOverViewport();

        ImGui::Begin("Navigator");
        if (ImGui::Button("Back")) history_jump(&state, 1);
        ImGui::SameLine();
        if (ImGui::Button("Forward")) history_jump(&state, 0);
        ImGui::SameLine();
        if (ImGui::Button("Help")) state.show_help = !state.show_help;
        if (state.request_focus_goto) { ImGui::SetKeyboardFocusHere(); state.request_focus_goto = 0; }
        ImGui::InputText("Goto", state.goto_input, 64); if (ImGui::IsItemDeactivatedAfterEdit()) select_line_by_address(document, &state);
        ImGui::SameLine(); if (ImGui::Button("Jump")) select_line_by_address(document, &state);
        if (state.request_focus_filter) { ImGui::SetKeyboardFocusHere(); state.request_focus_filter = 0; }
        ImGui::InputText("Filter", state.filter_input, 128);
        if (ImGui::Button("Next code_to_data")) jump_to_transition(document, &state, APEX_RENDER_TRANSITION_CODE_TO_DATA, 1);
        ImGui::SameLine();
        if (ImGui::Button("Prev code_to_data")) jump_to_transition(document, &state, APEX_RENDER_TRANSITION_CODE_TO_DATA, 0);
        if (ImGui::Button("Next table_to_data")) jump_to_transition(document, &state, APEX_RENDER_TRANSITION_TABLE_TO_DATA, 1);
        ImGui::SameLine();
        if (ImGui::Button("Prev table_to_data")) jump_to_transition(document, &state, APEX_RENDER_TRANSITION_TABLE_TO_DATA, 0);
        ImGui::End();

        ImGui::Begin("Labels"); render_label_list(document, &state); ImGui::End();
        ImGui::Begin("Banks"); render_bank_list(project, document, &state); ImGui::End();
        ImGui::Begin("Transitions"); render_transition_list(document, &state); ImGui::End();
        ImGui::Begin("Bookmarks"); render_bookmark_list(document, &state); ImGui::End();
        ImGui::Begin("Disassembly"); render_line_table(project, &document, &state); ImGui::End();
        ImGui::Begin("References");
        if (state.selected_line < document->line_count && document->lines[state.selected_line].has_location) {
            const auto *l = &document->lines[state.selected_line];
            auto in = find_incoming_refs(project, document, &state, l->bank, l->cpu_addr), out = find_outgoing_refs(project, document, &state, l->bank, l->cpu_addr);
            ImGui::TextUnformatted("Incoming"); if (in.empty()) ImGui::TextDisabled("none"); else for (auto &r : in) { char c[192]; snprintf(c, 192, "%s B%02x_A%04x", r.kind.c_str(), r.bank, r.cpu_addr); if (ImGui::SmallButton(c)) select_line(&state, r.line_index, 1); }
            ImGui::Separator(); ImGui::TextUnformatted("Outgoing"); if (out.empty()) ImGui::TextDisabled("none"); else for (auto &r : out) { char c[192]; snprintf(c, 192, "%s B%02x_A%04x", r.kind.c_str(), r.bank, r.cpu_addr); if (ImGui::SmallButton(c)) select_line(&state, r.line_index, 1); }
        } else ImGui::TextDisabled("No selection.");
        ImGui::End();
        ImGui::Begin("DMD"); render_dmd_view(project, document, &state); ImGui::End();
        ImGui::Begin("Call Graph"); render_call_graph(project, document, &state); ImGui::End();
        ImGui::Begin("Tables"); render_tables_window(project, &document, &state); ImGui::End();
        ImGui::Begin("Hardware"); render_hardware_window(project, document, &state); ImGui::End();
        render_xref_popup(project, document, &state);
        if (state.show_search_window) { ImGui::Begin("Global Search", &state.show_search_window); render_global_search(document, &state); ImGui::End(); }
        ImGui::Begin("Edit"); render_editor(project, &document, &original_snapshot, &state); ImGui::End();
        ImGui::Begin("Hex"); render_hex_view(project, &document, &state); ImGui::End();

        if (state.show_help) {
            ImGui::Begin("Help", &state.show_help);
            ImGui::BulletText("g/l: goto/label; /: filter; j/k: down/up; n/p: next/prev transition");
            ImGui::BulletText("c/d/s/t: mark code/data/string/table; Del: clear; f/Enter: follow link");
            ImGui::BulletText("X: XRefs; L/Shift+D: edit label/doc; B: bookmark; +/-: DMD Scrub; 0: reset; m: mark DMD");
            ImGui::BulletText("Ctrl+S: save overlay; Ctrl+F: global search; Ctrl+C: copy; Shift+Click: range");
            ImGui::BulletText("Hex View: Click byte to inspect/edit; highlights disassembly selection.");
            ImGui::BulletText("Alt+Left/Right: history");
            ImGui::End();
        }

        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_H)) state.show_help = !state.show_help;
            if (ImGui::IsKeyPressed(ImGuiKey_G)) state.request_focus_goto = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_Slash)) state.request_focus_filter = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_L)) state.request_focus_label = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_D)) { if (io.KeyShift) state.request_focus_doc = 1; else { uint8_t b; uint32_t a; if (selected_address(document, &state, &b, &a)) apply_data_at_selection(project, &document, &state, state.edit_spec_input[0] ? state.edit_spec_input : "bytes[1]"); } }
            if (ImGui::IsKeyPressed(ImGuiKey_S)) { if (io.KeyCtrl) state.request_save_overlay = 1; else apply_string_at_selection(project, &document, &state); }
            if (ImGui::IsKeyPressed(ImGuiKey_T)) apply_table_at_selection(project, &document, &state, state.edit_spec_input[0] ? state.edit_spec_input : "counted(ptr16_data)");
            if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) state.dmd_scrub_offset += io.KeyShift ? 32 : 1;
            if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) state.dmd_scrub_offset -= io.KeyShift ? 32 : 1;
            if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) state.dmd_scrub_offset = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_M)) { uint8_t b; uint32_t a; if (selected_address(document, &state, &b, &a)) { uint32_t ta = (uint32_t)((int64_t)a + state.dmd_scrub_offset); if (apex_project_set_kind(project, 1, b, ta, APEX_KIND_DATA, "dmd_fullframe") == 0) { rerender_and_reselect(project, &document, &state, b, ta); state.dmd_scrub_offset = 0; } } }
            if ((io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) || ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) history_jump(&state, 1);
            if ((io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) || ImGui::IsKeyPressed(ImGuiKey_RightBracket)) history_jump(&state, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_B)) { uint8_t b; uint32_t a; if (selected_address(document, &state, &b, &a)) { char n[64]; snprintf(n, 64, "Bookmark @ B%02x_%04x", b, a); state.bookmarks.push_back({b, a, n}); state.request_focus_new_bookmark = 1; } }
            if (ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move_selection_relative(document, &state, 1);
            if (ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move_selection_relative(document, &state, -1);
            if (ImGui::IsKeyPressed(ImGuiKey_N)) jump_primary_transition(document, &state, 1);
            if (ImGui::IsKeyPressed(ImGuiKey_P)) jump_primary_transition(document, &state, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_C)) { if (io.KeyCtrl) copy_selection_to_clipboard(document, &state); else apply_code_at_selection(project, &document, &state); }
            if (ImGui::IsKeyPressed(ImGuiKey_X)) { if (state.selected_line < document->line_count) { const auto *l = &document->lines[state.selected_line]; if (l->has_location) { state.request_xref_popup = true; state.xref_popup_bank = l->bank; state.xref_popup_addr = l->cpu_addr; } } }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) clear_kind_at_selection(project, &document, &state);
            if (ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_Enter)) { if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) { state.show_search_window = true; state.request_focus_global_search = 1; } else if (state.selected_line < document->line_count) { const auto *l = &document->lines[state.selected_line]; uint8_t tb; uint32_t ta; int tf; size_t tli; if (resolve_pointer_target(project, l, &tb, &ta, &tf) && apex_render_find_line_by_address(document, tb, ta, &tli)) select_line(&state, tli, 1); else follow_selected_link(document, &state); } }
        }
        ImGui::Render(); glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y); glClearColor(0.10f, 0.10f, 0.12f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); SDL_GL_SwapWindow(window);
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL2_Shutdown(); save_session(rom_path, config_path, &state, document);
    ImGui::DestroyContext(); SDL_GL_DeleteContext(gl_context); SDL_DestroyWindow(window); SDL_Quit();
    apex_project_free(project); return 0;
}
