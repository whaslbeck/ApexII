/* Patch window: edit a .patch.ini (see apex_patch.h) against the open ROM,
   build it live, mark the patched bytes in the disassembly, switch the project
   between the original and the patched bytes, export the patched ROM and run it
   in PinMAME. */
#include "ImGuiFileDialog.h"
#include "apex_ips.h"
#include "apex_rominfo.h"
#include "apeximgui_core.h"
#include "apeximgui_views_internal.h"
#include "cpu6809.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#else
#include <process.h>
#endif
#ifdef _WIN32
#include <direct.h>
#define apex_mkdir(p) _mkdir(p)
#else
#define apex_mkdir(p) mkdir(p, 0777)
#endif

/* ---- paths ---- */

static ImVec4 status_color(int st);
static const char *status_word(int st);
static void plan_data_traces(PatchUiState &ps);
static void pump_data_traces(UiState *state);

static std::string dir_of(const char *path) {
    const char *b = apex_path_basename(path);
    return std::string(path, (size_t)(b - path));
}

static std::string abs_path(const char *path) {
#ifdef _WIN32
    char buf[1024];
    if (_fullpath(buf, path, sizeof(buf)))
        return buf;
#else
    char buf[4096];
    if (realpath(path, buf))
        return buf;
#endif
    return path;
}

/* path as written into the patch file: relative when it lies in the patch
   file's directory (or below), else absolute. */
static std::string rel_to(const char *patch_path, const char *path) {
    std::string a = abs_path(path);
    std::string d = dir_of(abs_path(patch_path).c_str());
    if (!d.empty() && a.compare(0, d.size(), d) == 0)
        return a.substr(d.size());
    return a;
}

static bool make_dirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            std::string sub = path.substr(0, i);
            if (apex_mkdir(sub.c_str()) != 0 && errno != EEXIST)
                return false;
        }
    }
    return true;
}

/* ---- build ---- */

/* Record an undo step when the patch differs from the last snapshot. */
static void record_undo(PatchUiState &ps) {
    char *t = apex_patch_to_text(&ps.patch);
    if (!t)
        return;
    if (ps.snap != t) {
        double now = ImGui::GetTime();
        /* typing in a field: one step per pause, not per character */
        if (ps.snap_time == 0.0 || now - ps.snap_time > 1.0 || ps.undo.empty())
            ps.undo.push_back(ps.snap);
        if (ps.undo.size() > 200)
            ps.undo.erase(ps.undo.begin());
        ps.redo.clear();
        ps.snap = t;
        ps.snap_time = now;
    }
    free(t);
}

static void reset_undo(PatchUiState &ps) {
    ps.undo.clear();
    ps.redo.clear();
    char *t = ps.loaded ? apex_patch_to_text(&ps.patch) : nullptr;
    ps.snap = t ? t : "";
    ps.saved = ps.snap;
    ps.snap_time = 0.0;
    free(t);
}

static void patch_rebuild(ApexProject *project, UiState *state) {
    PatchUiState &ps = state->patch_state;
    if (!ps.loaded)
        return;
    if (!ps.snap.empty())
        record_undo(ps);
    if (ps.orig_rom.empty())
        ps.orig_rom.assign(project->rom.data, project->rom.data + project->rom.size);
    if (ps.built)
        apex_patch_build_free(&ps.build);
    /* The free space comes from the original's analysis (see PatchUiState). */
    if (!ps.patched_view) {
        apex_patch_free_space_free(&ps.detected);
        apex_patch_detect_free(project, ps.orig_rom.data(), &ps.detected);
        ps.detected_ok = true;
    }
    apex_patch_build_free_hint(&ps.patch, project, ps.orig_rom.data(), ps.orig_rom.size(),
                               ps.detected_ok ? &ps.detected : nullptr, &ps.build);
    ps.built = true;
    ps.cov_hit.clear();
    ps.cov_total.clear();
    ps.data_traces.clear(); /* they belong to the tested build */
    ps.data_tracing = false;
    ps.data_trace_pending = false;
    ps.ranges.clear();
    for (size_t i = 0; i < ps.build.count; i++) {
        const ApexPatchOpResult &r = ps.build.ops[i];
        if (r.status != APEX_PATCH_OK && r.status != APEX_PATCH_WARN)
            continue;
        if (r.length)
            ps.ranges.push_back({r.rom_off, r.length, (int)i});
        if (r.has_alloc && r.alloc_length && r.alloc_rom_off != r.rom_off)
            ps.ranges.push_back({r.alloc_rom_off, r.alloc_length, (int)i});
        for (size_t k = 0; k < r.fixup_count; k++) {
            const ApexPatchFixup &f = ps.build.fixups[r.fixup_first + k];
            if (f.status == APEX_FIXUP_OK)
                ps.ranges.push_back({f.rom_off, f.far ? 3u : 2u, (int)i});
        }
    }
    std::sort(ps.ranges.begin(), ps.ranges.end(),
              [](const PatchRange &a, const PatchRange &b) { return a.off < b.off; });
    if (ps.patched_view)
        ps.view_sync = true;
}

int patch_op_at(const UiState *state, size_t rom_off, size_t rom_end) {
    const PatchUiState &ps = state->patch_state;
    if (ps.ranges.empty())
        return -1;
    /* the last range starting before rom_end */
    auto it = std::upper_bound(ps.ranges.begin(), ps.ranges.end(), rom_end,
                               [](size_t v, const PatchRange &r) { return v <= r.off; });
    if (it == ps.ranges.begin())
        return -1;
    --it;
    return (it->off + it->len > rom_off) ? it->op : -1;
}

static void load_editor(PatchUiState &ps) {
    ps.editor_for = ps.selected;
    if (ps.selected < 0 || (size_t)ps.selected >= ps.patch.count)
        return;
    const ApexPatchOp &o = ps.patch.ops[ps.selected];
    auto cp = [](char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); };
    cp(ps.ed_name, sizeof(ps.ed_name), o.name);
    cp(ps.ed_at, sizeof(ps.ed_at), o.at);
    cp(ps.ed_expect, sizeof(ps.ed_expect), o.expect);
    cp(ps.ed_bytes, sizeof(ps.ed_bytes), o.bytes);
    cp(ps.ed_code, sizeof(ps.ed_code), o.code);
    cp(ps.ed_file, sizeof(ps.ed_file), o.file);
    cp(ps.ed_note, sizeof(ps.ed_note), o.note);
    ps.ed_count = (int)o.count;
    ps.ed_length = (int)o.length;
    cp(ps.ed_alloc, sizeof(ps.ed_alloc), o.alloc);
    cp(ps.ed_placed, sizeof(ps.ed_placed), o.placed);
    cp(ps.ed_target, sizeof(ps.ed_target), o.target);
    cp(ps.ed_label, sizeof(ps.ed_label), o.label);
    ps.ed_width = o.width ? (int)o.width : (o.type == APEX_PATCH_VALUE ? 1 : 3);
    cp(ps.ed_value, sizeof(ps.ed_value), o.value);
    cp(ps.ed_vtype, sizeof(ps.ed_vtype), o.vtype);
    cp(ps.ed_text, sizeof(ps.ed_text), o.text);
    ps.ed_mode = !o.mode                       ? 0
                 : !strcmp(o.mode, "inplace")  ? 1
                 : !strcmp(o.mode, "shift")    ? 2
                 : !strcmp(o.mode, "relocate") ? 3
                                               : 0;
    if ((o.type == APEX_PATCH_DMD || o.type == APEX_PATCH_SPRITE) && ps.ed_mode == 3)
        ps.ed_mode = 2; /* images have no shift */
    ps.ed_planes = (int)o.planes;
}

static void changed(ApexProject *project, UiState *state) {
    state->patch_state.dirty = true;
    patch_rebuild(project, state);
}

/* ---- open / new / save ---- */

static void patch_close(PatchUiState &ps) {
    if (ps.built)
        apex_patch_build_free(&ps.build);
    if (ps.loaded)
        apex_patch_free(&ps.patch);
    ps.built = false;
    ps.loaded = false;
    ps.dirty = false;
    ps.selected = -1;
    ps.editor_for = -1;
    ps.ranges.clear();
}

void patch_open_path(ApexProject *project, UiState *state, const char *path_in) {
    PatchUiState &ps = state->patch_state;
    std::string keep(path_in); /* path_in may be ps.path itself */
    const char *path = keep.c_str();
    char err[512];
    ApexPatch p;
    if (apex_patch_load(path, &p, err, sizeof(err)) != 0) {
        set_status(state, err);
        return;
    }
    patch_close(ps);
    ps.patch = p;
    ps.loaded = true;
    reset_undo(ps);
    snprintf(ps.path, sizeof(ps.path), "%s", path);
    snprintf(ps.ed_output, sizeof(ps.ed_output), "%s", p.output ? p.output : "");
    patch_rebuild(project, state);
    if (ps.build.msg[0])
        set_status(state, ps.build.msg);
}

static void patch_new(ApexProject *project, UiState *state, const char *path_in) {
    PatchUiState &ps = state->patch_state;
    std::string keep(path_in); /* path_in may be ps.path itself */
    const char *path = keep.c_str();
    patch_close(ps);
    memset(&ps.patch, 0, sizeof(ps.patch));
    snprintf(ps.path, sizeof(ps.path), "%s", path);
    ps.patch.path = strdup(path);
    ps.patch.base_rom = strdup(rel_to(path, project->rom_path).c_str());
    if (project->config_path)
        ps.patch.analysis = strdup(rel_to(path, project->config_path).c_str());
    const uint8_t *orig = ps.orig_rom.empty() ? project->rom.data : ps.orig_rom.data();
    ps.patch.base_crc32 = ari_crc32(orig, project->rom.size);
    ps.patch.has_crc = 1;
    ps.patch.checksum = APEX_PATCH_CHECKSUM_FIX;
    ps.patch.comment = strdup("; ApexII patch file -- see apexpatch / apex_patch.h");
    ps.loaded = true;
    ps.dirty = true;
    ps.ed_output[0] = '\0';
    reset_undo(ps);
    patch_rebuild(project, state);
}

static bool patch_save(UiState *state) {
    PatchUiState &ps = state->patch_state;
    if (!ps.loaded)
        return false;
    if (apex_patch_save(&ps.patch, ps.path) != 0) {
        set_status(state, "cannot write the patch file");
        return false;
    }
    ps.dirty = false;
    char *t = apex_patch_to_text(&ps.patch);
    ps.saved = t ? t : "";
    free(t);
    set_status(state, "patch saved");
    return true;
}

static std::string default_patch_path(const ApexProject *project) {
    std::string rp = project->rom_path;
    size_t dot = rp.find_last_of('.');
    size_t slash = rp.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        rp.erase(dot);
    return rp + ".patch.ini";
}

/* ---- original / patched view ---- */

/* In the patched view the patch's code labels (and the start of every
   allocated block, as a code entry) take part in the analysis, without
   becoming part of the config. */
static void patch_set_transient(ApexProject *project, UiState *state, bool on) {
    PatchUiState &ps = state->patch_state;
    std::vector<ConfigLabel> labels;
    std::vector<ConfigEntry> entries;
    if (on && ps.built) {
        for (size_t i = 0; i < ps.build.sym_count; i++) {
            const ApexPatchSym &sy = ps.build.syms[i];
            if (sy.is_ram)
                continue;
            ConfigLabel l;
            memset(&l, 0, sizeof(l));
            l.has_bank = 1;
            l.bank = sy.bank;
            l.addr = sy.addr;
            l.name = sy.name;
            labels.push_back(l);
            if (sy.starts_code) {
                ConfigEntry e;
                memset(&e, 0, sizeof(e));
                e.has_bank = 1;
                e.bank = sy.bank;
                e.addr = sy.addr;
                entries.push_back(e);
            }
        }
    }
    ConfigLabels cl;
    ConfigEntries ce;
    cl.items = labels.empty() ? nullptr : labels.data();
    cl.count = cl.cap = labels.size();
    ce.items = entries.empty() ? nullptr : entries.data();
    ce.count = ce.cap = entries.size();
    apex_project_set_transient(project, &cl, &ce);
}

static void request_view(ApexProject *project, UiState *state, bool patched) {
    PatchUiState &ps = state->patch_state;
    if (patched && (!ps.built || !ps.build.rom))
        return;
    if (ps.orig_rom.empty())
        ps.orig_rom.assign(project->rom.data, project->rom.data + project->rom.size);
    ps.patched_view = patched;
    ps.view_sync = true;
}

void patch_pump(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state) {
    PatchUiState &ps = state->patch_state;
    if (ps.data_trace_pending || ps.data_tracing)
        pump_data_traces(state);
    /* free-RAM finder: one RAM snapshot every half second while sampling */
    if (ps.ram_sampling && state->pinmame.connected && ImGui::GetTime() >= ps.ram_next) {
        ps.ram_next = ImGui::GetTime() + 0.5;
        std::vector<uint8_t> ram;
        bool ok = true; /* the debugger returns at most 2 KB per read */
        for (unsigned a = 0; ok && a < APEX_RAM_SIZE; a += 0x800u)
            ok = apex_pinmame_memory(state->pinmame.port, a, 0x800, -1, ram);
        if (ok && ram.size() >= APEX_RAM_SIZE) {
            apex_ram_map_sample(&ps.ram, ram.data());
            apex_ram_map_runs(&ps.ram, (unsigned)ps.ram_min);
        }
    }
    if (ps.open_pending && project->analysis_ready && !state->rendering) {
        ps.open_pending = false;
        FILE *f = fopen(ps.path, "r");
        if (f) {
            fclose(f);
            patch_open_path(project, state, ps.path);
        }
    }
    if (!ps.view_sync || state->rendering || state->async_pending)
        return;
    ps.view_sync = false;
    const uint8_t *src = nullptr;
    if (ps.patched_view && ps.built && ps.build.rom && ps.build.rom_size == project->rom.size)
        src = ps.build.rom;
    else if (!ps.orig_rom.empty())
        src = ps.orig_rom.data();
    if (!src || memcmp(project->rom.data, src, project->rom.size) == 0)
        return;
    memcpy((void *)project->rom.data, src, project->rom.size);
    patch_set_transient(project, state, ps.patched_view);
    uint8_t b = 0xffu;
    uint32_t a = 0;
    selected_address(*document_ptr, state, &b, &a);
    apex_project_invalidate(project, APEX_DIRTY_ANALYSIS);
    rerender_and_reselect(project, document_ptr, state, b, a);
}

/* ---- export / PinMAME ---- */

static bool export_rom(UiState *state, const char *dst) {
    PatchUiState &ps = state->patch_state;
    if (!ps.built || !ps.build.rom || ps.build.errors) {
        set_status(state, "the patch has errors; nothing written");
        return false;
    }
    FILE *f = fopen(dst, "wb");
    if (!f || fwrite(ps.build.rom, 1, ps.build.rom_size, f) != ps.build.rom_size) {
        if (f)
            fclose(f);
        set_status(state, "cannot write the patched ROM");
        return false;
    }
    fclose(f);
    char m[1200];
    snprintf(m, sizeof(m), "wrote %s (CRC-32 %08X)", dst,
             (unsigned)ari_crc32(ps.build.rom, ps.build.rom_size));
    set_status(state, m);
    return true;
}

/* <patch path without .patch.ini / .ini><ext> */
static std::string patch_sibling(const char *patch_path, const char *ext) {
    std::string b = patch_path;
    for (const char *suf : {".patch.ini", ".ini"}) {
        size_t n = strlen(suf);
        if (b.size() > n && b.compare(b.size() - n, n, suf) == 0) {
            b.erase(b.size() - n);
            break;
        }
    }
    return b + ext;
}

/* IPS / BPS of the patch, or its Markdown report, next to the patch file. */
static void export_patchfile(ApexProject *project, UiState *state, int kind) {
    PatchUiState &ps = state->patch_state;
    if (!ps.built || !ps.build.rom || (kind != 2 && ps.build.errors)) {
        set_status(state, "the patch has errors; nothing written");
        return;
    }
    const uint8_t *orig = ps.orig_rom.empty() ? project->rom.data : ps.orig_rom.data();
    std::string dst = patch_sibling(ps.path, kind == 0 ? ".ips" : kind == 1 ? ".bps" : ".md");
    char m[1200];
    if (kind == 2) {
        FILE *f = fopen(dst.c_str(), "w");
        if (!f) {
            set_status(state, "cannot write the report");
            return;
        }
        apex_patch_write_report(f, ps.path, &ps.patch, project, orig, &ps.build);
        fclose(f);
        snprintf(m, sizeof(m), "wrote %s", dst.c_str());
    } else {
        size_t n = 0;
        uint8_t *pf = kind == 0
                          ? apex_ips_make(orig, ps.build.rom, ps.build.rom_size, &n)
                          : apex_bps_make(orig, ps.build.rom, ps.build.rom_size, "apeximgui", &n);
        FILE *f = pf ? fopen(dst.c_str(), "wb") : nullptr;
        bool ok = f && fwrite(pf, 1, n, f) == n;
        if (f)
            fclose(f);
        free(pf);
        if (!ok) {
            set_status(state, "cannot write the patch file");
            return;
        }
        snprintf(m, sizeof(m), "wrote %s (%lu bytes)", dst.c_str(), (unsigned long)n);
    }
    set_status(state, m);
}

/* Rebase the open patch onto another version of the ROM; the new patch goes
   next to that ROM (<rom>.patch.ini), its analysis is <rom>.ini when that
   exists, else labels matched by apexmatch. */
static void rebase_to(ApexProject *project, UiState *state, const std::string &trom) {
    PatchUiState &ps = state->patch_state;
    if (ps.patched_view) {
        set_status(state, "rebase: switch off \"show patched ROM\" first");
        return;
    }
    if (!ps.built || ps.build.errors) {
        set_status(state, "rebase: fix the patch's errors first");
        return;
    }
    std::string base = trom;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && base.find_last_of("/\\") < dot)
        base.erase(dot);
    std::string out = base + ".patch.ini", tini = base + ".ini";
    FILE *f = fopen(tini.c_str(), "r");
    if (f)
        fclose(f);
    char err[512], labels[1200];
    ApexPatch np;
    ApexRebaseOp *ro = nullptr;
    int failed = apex_patch_rebase_files(&ps.patch, project, &ps.build, trom.c_str(),
                                         f ? tini.c_str() : nullptr, out.c_str(), &np, &ro, labels,
                                         sizeof(labels), err, sizeof(err));
    if (failed < 0) {
        set_status(state, err);
        return;
    }
    ps.rebase_out = out;
    ps.rebase_labels = labels;
    ps.rebase_rom = trom;
    ps.rebase_ini = f ? tini : (labels[0] ? patch_sibling(out.c_str(), ".labels.ini") : "");
    ps.rebase_failed = failed;
    ps.rebase_ops.assign(ro, ro + np.count);
    ps.rebase_names.clear();
    for (size_t i = 0; i < np.count; i++)
        ps.rebase_names.push_back(np.ops[i].name);
    ps.rebase_open = true;
    free(ro);
    apex_patch_free(&np);
    char m[1300];
    snprintf(m, sizeof(m), "rebase: wrote %s, %d op(s) not found", out.c_str(), failed);
    set_status(state, m);
}

/* Start another apeximgui on rom (+ ini) with a patch file: the GUI opens one
   ROM per process. */
static bool spawn_gui(const std::string &exe_dir, const std::string &rom, const std::string &ini,
                      const std::string &patch, std::string &err) {
#ifdef _WIN32
    std::string exe = exe_dir + "apeximgui.exe";
    intptr_t rc = ini.empty() ? _spawnl(_P_NOWAIT, exe.c_str(), exe.c_str(), rom.c_str(), "--patch",
                                        patch.c_str(), (char *)NULL)
                              : _spawnl(_P_NOWAIT, exe.c_str(), exe.c_str(), rom.c_str(),
                                        ini.c_str(), "--patch", patch.c_str(), (char *)NULL);
    if (rc == -1) {
        err = "cannot start " + exe;
        return false;
    }
    return true;
#else
    std::string exe = exe_dir + "apeximgui";
    std::vector<char *> av; /* built before fork: no allocation in the child */
    av.push_back(const_cast<char *>(exe.c_str()));
    av.push_back(const_cast<char *>(rom.c_str()));
    if (!ini.empty())
        av.push_back(const_cast<char *>(ini.c_str()));
    av.push_back(const_cast<char *>("--patch"));
    av.push_back(const_cast<char *>(patch.c_str()));
    av.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork failed";
        return false;
    }
    if (pid == 0) { /* double fork: the GUI is not left with a zombie */
        if (fork() == 0) {
            setsid();
            execv(exe.c_str(), av.data());
        }
        _exit(0);
    }
    waitpid(pid, nullptr, 0);
    return true;
#endif
}

void render_rebase_result(ApexProject *project, UiState *state) {
    (void)project;
    PatchUiState &ps = state->patch_state;
    if (!ps.rebase_open)
        return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760, 360), ImGuiCond_Appearing);
    if (!ImGui::Begin("Rebase Result", &ps.rebase_open, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", ps.rebase_out.c_str());
    if (!ps.rebase_labels.empty())
        ImGui::TextDisabled("analysis: %s, matched by apexmatch", ps.rebase_labels.c_str());
    ImGui::Text("%lu op(s), %d not found (switched off in the new patch)",
                (unsigned long)ps.rebase_ops.size(), ps.rebase_failed);
    ImGui::SameLine();
    if (ImGui::Button("Open the target ROM with it")) {
        std::string err;
        if (spawn_gui(ps.exe_dir, ps.rebase_rom, ps.rebase_ini, ps.rebase_out, err))
            set_status(state, "started a second ApexII window for the target ROM");
        else
            set_status(state, err.c_str());
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A new window: the target ROM, its analysis and the rebased patch");
    if (ImGui::BeginTable("rebase", 4,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Op", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Found by", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < ps.rebase_ops.size(); i++) {
            const ApexRebaseOp &r = ps.rebase_ops[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(ps.rebase_names[i].c_str());
            ImGui::TableSetColumnIndex(1);
            if (r.method[0] && (r.old_bank || r.old_addr))
                ImGui::Text("B%02x_A%04x -> B%02x_A%04x", r.old_bank, (unsigned)r.old_addr,
                            r.new_bank, (unsigned)r.new_addr);
            else if (r.old_bank || r.old_addr)
                ImGui::Text("B%02x_A%04x", r.old_bank, (unsigned)r.old_addr);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.method);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(status_color(r.status), "%s%s%s", status_word(r.status),
                               r.msg[0] ? ": " : "", r.msg);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

/* Write the patched game ROM into <patchdir>/pinmame/<game>/<member> and run
   PinMAME with that directory in front of the original rom path, so the
   romset's other files (sound ROMs) still come from the original zip. */
static void test_in_pinmame(ApexProject *project, UiState *state) {
    PatchUiState &ps = state->patch_state;
    PinmameState &pm = state->pinmame;
    if (!pm.game[0] || !pm.rompath[0]) {
        set_status(state, "set up PinMAME first (Windows -> PinMAME (dynamic): rom path + game)");
        state->show_pinmame = true;
        return;
    }
    if (!ps.built || !ps.build.rom || ps.build.errors) {
        set_status(state, "the patch has errors; fix them before testing");
        return;
    }
    uint32_t crc =
        ari_crc32(ps.orig_rom.empty() ? project->rom.data : ps.orig_rom.data(), project->rom.size);
    std::string zip = std::string(pm.rompath) + "/" + pm.game + ".zip";
    char member[256];
    if (!apex_patch_romset_member(zip.c_str(), crc, member, sizeof(member))) {
        set_status(state, ("no file with this ROM's CRC in " + zip).c_str());
        return;
    }
    std::string base = dir_of(abs_path(ps.path).c_str()) + "pinmame";
    std::string dir = base + "/" + pm.game;
    if (!make_dirs(dir)) {
        set_status(state, ("cannot create " + dir).c_str());
        return;
    }
    if (!export_rom(state, (dir + "/" + member).c_str()))
        return;
    if (pm.alive) {
        if (pm.external)
            apex_pinmame_events_stop(pm.pm);
        else
            apex_pinmame_stop(pm.pm);
        pm.external = false;
        pm.connected = false;
        pm.alive = false;
    }
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string rompath = base + sep + pm.rompath;
    if (apex_pinmame_launch(pm.pm, pm.bin_path, rompath, pm.game, pm.port)) {
        pm.status = "launched the patched ROM — waiting for debugger…";
        pm.next_poll = 0.0;
        pm.connected = false;
        pm.launching = true;
        pm.resume_on_connect = true;
        pm.coverage_on_connect = true; /* for "Check run" */
        ps.cov_hit.clear();
        ps.cov_total.clear();
        plan_data_traces(ps); /* data ops: is the new data read? */
        ps.data_trace_pending = true;
        ps.data_tracing = false;
        state->show_pinmame = true;
        set_status(state, "PinMAME runs the patched ROM");
    } else {
        pm.status = "launch failed: " + pm.pm.last_error;
        set_status(state, pm.status.c_str());
    }
}

/* Ops whose result the game reads as data (not code). */
static bool data_op_type(int t) {
    return t == APEX_PATCH_VALUE || t == APEX_PATCH_STRING || t == APEX_PATCH_MOVE ||
           t == APEX_PATCH_DMD || t == APEX_PATCH_SPRITE || t == APEX_PATCH_BLOB ||
           t == APEX_PATCH_PTR;
}

/* The read traces for the data ops of the current build (one byte each:
   where the game starts reading -- the moved place for relocated data). */
static void plan_data_traces(PatchUiState &ps) {
    ps.data_traces.clear();
    for (size_t i = 0; i < ps.build.count && ps.data_traces.size() < 120; i++) {
        const ApexPatchOpResult &r = ps.build.ops[i];
        if (!data_op_type(ps.patch.ops[i].type) ||
            (r.status != APEX_PATCH_OK && r.status != APEX_PATCH_WARN))
            continue;
        PatchUiState::DataTrace t{};
        t.op = (int)i;
        t.bank = r.has_alloc ? r.alloc_bank : r.bank;
        t.addr = r.has_alloc ? r.alloc_addr : r.addr;
        if (!r.has_alloc && !r.length)
            continue;
        ps.data_traces.push_back(t);
    }
}

void patch_pinmame_connected(UiState *state) {
    PatchUiState &ps = state->patch_state;
    PinmameState &pm = state->pinmame;
    if (!ps.data_trace_pending)
        return;
    ps.data_trace_pending = false;
    apex_pinmame_trace_clear(pm.port);
    for (const auto &t : ps.data_traces)
        apex_pinmame_trace_add(pm.port, t.addr, 1, 1 /* read */,
                               t.bank == 0xffu ? -1 : (int)t.bank);
    ps.data_tracing = !ps.data_traces.empty();
    ps.data_next = 0.0;
}

/* Once connected: install the traces; while running: collect the reads. */
static void pump_data_traces(UiState *state) {
    PatchUiState &ps = state->patch_state;
    PinmameState &pm = state->pinmame;
    if (!pm.connected)
        return;
    patch_pinmame_connected(state); /* if the connect hook did not run */
    if (!ps.data_tracing || ImGui::GetTime() < ps.data_next)
        return;
    ps.data_next = ImGui::GetTime() + 0.5;
    std::vector<ApexPinmameAccess> log;
    long total = 0;
    bool has_total = false;
    if (!apex_pinmame_trace_read(pm.port, log, total, has_total))
        return;
    for (const auto &a : log) {
        if (a.write)
            continue;
        for (auto &t : ps.data_traces) {
            if ((uint32_t)a.adr != t.addr ||
                (t.addr >= 0x4000u && t.addr < 0x8000u && a.bank >= 0 && a.bank != (int)t.bank))
                continue;
            t.read = true;
            if (std::find(t.pcs.begin(), t.pcs.end(), a.pc) == t.pcs.end() && t.pcs.size() < 16)
                t.pcs.push_back(a.pc);
        }
    }
}

/* Executed instructions of each op's code (site and allocated / detour code)
   in the running PinMAME's coverage, which flags the first byte of every
   executed instruction.  Data ops have no code to check. */
static void check_run(UiState *state) {
    PatchUiState &ps = state->patch_state;
    PinmameState &pm = state->pinmame;
    ps.cov_hit.assign(ps.build.count, -1);
    ps.cov_total.assign(ps.build.count, -1);
    int checked = 0, never = 0;
    for (size_t i = 0; i < ps.build.count; i++) {
        const ApexPatchOpResult &r = ps.build.ops[i];
        int t = ps.patch.ops[i].type;
        bool code = t == APEX_PATCH_ASM || t == APEX_PATCH_HOOK || t == APEX_PATCH_NOP ||
                    t == APEX_PATCH_BYTES || t == APEX_PATCH_BRANCH || t == APEX_PATCH_REDIRECT;
        if (!code || (r.status != APEX_PATCH_OK && r.status != APEX_PATCH_WARN))
            continue;
        int hit = 0, total = 0;
        /* max_insns: a hook site counts only its JMP (the NOP pad is skipped) */
        auto count = [&](uint8_t bank, uint32_t addr, const uint8_t *b, size_t len, int max_insns) {
            std::vector<uint8_t> f;
            if (!len || !b ||
                !apex_pinmame_coverage_window(pm.port, bank == 0xffu ? -1 : bank, addr,
                                              (unsigned)len, f))
                return;
            size_t pos = 0;
            for (int n = 0; pos < len && (max_insns <= 0 || n < max_insns); n++) {
                char txt[64];
                Cpu6809InstrInfo in = cpu6809_disassemble_info(
                    b + pos, len - pos, addr + (uint32_t)pos, txt, sizeof(txt));
                total++;
                hit += pos < f.size() && f[pos] ? 1 : 0;
                pos += in.size ? in.size : 1u;
            }
        };
        bool same = r.has_alloc && r.length && r.rom_off == r.alloc_rom_off;
        count(r.bank, r.addr, r.patched, r.length, t == APEX_PATCH_HOOK ? 1 : 0);
        if (r.has_alloc && !same)
            count(r.alloc_bank, r.alloc_addr, r.alloc_bytes, r.alloc_length, 0);
        ps.cov_hit[i] = hit;
        ps.cov_total[i] = total;
        checked++;
        never += total && !hit;
    }
    char m[160];
    snprintf(m, sizeof(m), "coverage: %d code op(s) checked, %d never ran", checked, never);
    set_status(state, m);
}

/* ---- image preview (dmd / sprite ops) ---- */

/* An image decoded for the preview: up to two planes, w x h pixels. */
struct PreviewImage {
    bool ok = false;
    unsigned w = 0, h = 0, rb = 0; /* rb: bytes per row */
    bool lsb = false;              /* DMD: bit 0 = leftmost pixel; sprites: bit 7 */
    bool two = false;
    std::vector<uint8_t> p0, p1;
};

/* Bytes of a ROM image at (bank, addr): file offset, or SIZE_MAX. */
static size_t image_offset(const ApexProject *p, const uint8_t *img, size_t size, uint8_t bank,
                           uint32_t addr) {
    size_t paged = size - 0x8000u;
    if (bank == 0xffu)
        return addr >= 0x8000u ? paged + (addr - 0x8000u) : SIZE_MAX;
    int bi = bank_index_for_id(img, p->banks, bank);
    if (bi < 0 || addr < 0x4000u || addr >= 0x8000u)
        return SIZE_MAX;
    return (size_t)bi * 0x4000u + (addr - 0x4000u);
}

/* (bank, addr): where the image is in `img`; (rbank, raddr): the original
   place, whose data range says the plane count / no-header height. */
static PreviewImage decode_preview(const ApexProject *p, const ApexPatchOp &o, const uint8_t *img,
                                   size_t size, uint8_t bank, uint32_t addr, uint8_t rbank,
                                   uint32_t raddr) {
    PreviewImage pi;
    size_t off = image_offset(p, img, size, bank, addr);
    if (off == SIZE_MAX || off >= size)
        return pi;
    const uint8_t *src = img + off;
    size_t avail = size - off;
    if (o.type == APEX_PATCH_DMD) {
        const DataRange *dr = data_range_at(rbank, raddr, &p->data_ranges);
        unsigned planes =
            o.planes ? o.planes
            : (dr && dr->addr == raddr && dr->kind == DATA_DMD_FULLFRAME && dr->length)
                ? (unsigned)dr->length
                : 1u;
        pi.w = APEX_DMD_WIDTH;
        pi.h = APEX_DMD_HEIGHT;
        pi.rb = APEX_DMD_ROW_BYTES;
        pi.lsb = true;
        pi.p0.assign(APEX_DMD_PAGE_BYTES, 0);
        size_t used = 0;
        if (!apexdmd_decode_fullframe(src, avail, pi.p0.data(), &used, nullptr))
            return pi;
        if (planes >= 2) {
            pi.p1.assign(APEX_DMD_PAGE_BYTES, 0);
            size_t u2 = 0;
            pi.two = used < avail &&
                     apexdmd_decode_fullframe(src + used, avail - used, pi.p1.data(), &u2, nullptr);
        }
        pi.ok = true;
        return pi;
    }
    uint8_t tmp[APEX_SPRITE_MAX_BYTES], hdr = 0, w = 0, h = 0, enc = 0;
    size_t used = 0;
    const DataRange *dr = data_range_at(rbank, raddr, &p->data_ranges);
    if (dr && dr->addr == raddr && dr->kind == DATA_SPRITE_NOHEADER) {
        h = (uint8_t)dr->length;
        if (!apexsprite_decode_noheader(src, avail, tmp, h, &w, &used))
            return pi;
    } else if (!apexsprite_decode(src, avail, tmp, &hdr, nullptr, nullptr, &w, &h, &enc, &used)) {
        return pi;
    }
    pi.w = w;
    pi.h = h;
    pi.rb = (w + 7u) / 8u;
    size_t page = (size_t)pi.rb * h;
    pi.p0.assign(tmp, tmp + page);
    if (hdr == 0xffu && avail >= 5u + page) {
        pi.p1.assign(src + 5, src + 5 + page);
        pi.two = true;
    } else if (hdr == 0xfeu && avail >= 7u) {
        uint32_t pa = ((uint32_t)src[5] << 8) | src[6];
        size_t po = image_offset(p, img, size, pa >= 0x8000u ? 0xffu : bank, pa);
        if (po != SIZE_MAX && po + page <= size) {
            pi.p1.assign(img + po, img + po + page);
            pi.two = true;
        }
    }
    pi.ok = true;
    return pi;
}

/* Draw at the cursor; plane `bright` (of two) counts double: four greys. */
static void draw_preview(const PreviewImage &pi, int bright, float scale, const char *caption) {
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", caption);
    ImVec2 o = ImGui::GetCursorScreenPos();
    float bw = pi.ok ? pi.w * scale : 128.0f * scale, bh = pi.ok ? pi.h * scale : 32.0f * scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + bw, o.y + bh), IM_COL32(20, 12, 4, 255));
    if (pi.ok) {
        for (unsigned y = 0; y < pi.h; y++) {
            for (unsigned x = 0; x < pi.w; x++) {
                size_t byte = (size_t)y * pi.rb + x / 8u;
                uint8_t bit = pi.lsb ? (uint8_t)(1u << (x % 8u)) : (uint8_t)(0x80u >> (x % 8u));
                int a = (pi.p0[byte] & bit) ? 1 : 0, b = pi.two && (pi.p1[byte] & bit) ? 1 : 0;
                int level = pi.two ? (bright ? a + 2 * b : 2 * a + b) : 3 * a;
                if (!level)
                    continue;
                int c = 60 + level * 65;
                ImVec2 q(o.x + x * scale, o.y + y * scale);
                dl->AddRectFilled(q, ImVec2(q.x + scale, q.y + scale),
                                  IM_COL32(c, c * 55 / 100, 0, 255));
            }
        }
    }
    ImGui::Dummy(ImVec2(bw, bh));
    ImGui::EndGroup();
}

/* ---- adding ops from the disassembly ---- */

/* Open the ROM's default patch file (or start it) when none is open. */
static void ensure_patch(ApexProject *project, UiState *state) {
    PatchUiState &ps = state->patch_state;
    if (ps.loaded)
        return;
    std::string def = default_patch_path(project);
    FILE *f = fopen(def.c_str(), "r");
    if (f) {
        fclose(f);
        patch_open_path(project, state, def.c_str());
    } else {
        patch_new(project, state, def.c_str());
    }
}

static int add_op(ApexProject *project, UiState *state, int type, const char *name,
                  const ApexRenderedLine *line) {
    PatchUiState &ps = state->patch_state;
    ensure_patch(project, state);
    char at[160];
    apex_patch_format_at(project, line->bank, line->cpu_addr, at, sizeof(at));
    long i = apex_patch_add_op(&ps.patch, type, name, at);
    if (i < 0)
        return -1;
    ps.selected = (int)i;
    ps.editor_for = -1;
    state->show_patch = true;
    return (int)i;
}

static std::string hex_bytes(const uint8_t *b, size_t n) {
    std::string s;
    char t[4];
    for (size_t i = 0; i < n; i++) {
        snprintf(t, sizeof(t), i ? " %02x" : "%02x", b[i]);
        s += t;
    }
    return s;
}

/* After adding an op: build and pin its expect guard from the original bytes
   (only this op's -- other ops stay as the user wrote them). */
static void finish_add(ApexProject *project, UiState *state) {
    PatchUiState &ps = state->patch_state;
    patch_rebuild(project, state);
    if (ps.selected >= 0 && (size_t)ps.selected < ps.build.count) {
        ApexPatchOp &o = ps.patch.ops[ps.selected];
        const ApexPatchOpResult &r = ps.build.ops[ps.selected];
        const uint8_t *g = r.guard ? r.guard : r.orig;
        size_t gl = r.guard ? r.guard_len : r.length;
        if (!o.expect && g && gl &&
            !((o.type == APEX_PATCH_ASM || o.type == APEX_PATCH_BLOB) && o.alloc) &&
            (r.status == APEX_PATCH_OK || r.status == APEX_PATCH_WARN)) {
            apex_patch_set_field(&o.expect, hex_bytes(g, gl).c_str());
            patch_rebuild(project, state);
        }
    }
    ps.dirty = true;
    ps.editor_for = -1;
}

/* ---- snippet library ---- */

static void open_snippet_dialog(UiState *state, const char *at) {
    PatchUiState &ps = state->patch_state;
    /* reload on every open: snippet files may have been edited */
    apex_snippet_list_free(&ps.snippets);
    apex_snippet_load_default(&ps.snippets, ps.path[0] ? ps.path : nullptr,
                              ps.exe_dir.empty() ? nullptr : ps.exe_dir.c_str());
    ps.snippets_loaded = true;
    if (ps.snippet_sel >= (int)ps.snippets.count)
        ps.snippet_sel = 0;
    snprintf(ps.snippet_at, sizeof(ps.snippet_at), "%s", at ? at : "");
    ps.snippet_alloc[0] = '\0';
    ps.snippet_name[0] = '\0';
    memset(ps.snippet_vals, 0, sizeof(ps.snippet_vals));
    ps.snippet_open = true;
    state->show_patch = true;
}

void render_snippet_dialog(ApexProject *project, UiState *state) {
    PatchUiState &ps = state->patch_state;
    if (!ps.snippet_open)
        return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 0), ImVec2(560, FLT_MAX));
    if (!ImGui::Begin("Insert Snippet", &ps.snippet_open,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (!ps.snippets.count) {
        ImGui::TextWrapped("No snippets found. They are .asm files in snippets/ next to the "
                           "patch file, in $APEX_SNIPPETS, in ./snippets, or bundled with "
                           "ApexII (see README, apexpatch).");
        ImGui::End();
        return;
    }
    const ApexSnippet *sn = &ps.snippets.items[ps.snippet_sel];
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##snippet", sn->title)) {
        for (size_t i = 0; i < ps.snippets.count; i++) {
            const ApexSnippet *x = &ps.snippets.items[i];
            char lb[256];
            snprintf(lb, sizeof(lb), "%s  [%s]##%s", x->title, x->type, x->id);
            if (ImGui::Selectable(lb, (int)i == ps.snippet_sel)) {
                ps.snippet_sel = (int)i;
                memset(ps.snippet_vals, 0, sizeof(ps.snippet_vals));
            }
        }
        ImGui::EndCombo();
    }
    sn = &ps.snippets.items[ps.snippet_sel];
    bool hook = !strcmp(sn->type, "hook");
    if (sn->about)
        ImGui::TextWrapped("%s", sn->about);
    ImGui::TextDisabled("%s, %s", hook ? "hook (detour)" : "asm", sn->path);
    ImGui::Separator();
    const char *vals[8] = {};
    for (size_t k = 0; k < sn->param_count && k < 8; k++) {
        ImGui::SetNextItemWidth(ui_w(220));
        ImGui::InputTextWithHint(sn->params[k].name, sn->params[k].def ? sn->params[k].def : "",
                                 ps.snippet_vals[k], sizeof(ps.snippet_vals[k]));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", sn->params[k].desc);
        vals[k] = ps.snippet_vals[k];
    }
    ImGui::SetNextItemWidth(ui_w(220));
    ImGui::InputTextWithHint("at", hook ? "the instruction to hook" : "empty = allocate",
                             ps.snippet_at, sizeof(ps.snippet_at));
    if (!hook && !ps.snippet_at[0]) {
        ImGui::SetNextItemWidth(ui_w(220));
        ImGui::InputTextWithHint("alloc", "system, any, bank:NN", ps.snippet_alloc,
                                 sizeof(ps.snippet_alloc));
    }
    ImGui::SetNextItemWidth(ui_w(220));
    ImGui::InputTextWithHint("op name", sn->id, ps.snippet_name, sizeof(ps.snippet_name));
    char err[256] = "";
    char *code = apex_snippet_expand(sn, vals, err, sizeof(err));
    ImGui::Separator();
    if (code) {
        ImGui::TextDisabled("code:");
        ImGui::TextUnformatted(code);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", err);
    }
    bool can = code && (ps.snippet_at[0] || (!hook && ps.snippet_alloc[0]));
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Insert")) {
        ensure_patch(project, state);
        int type = hook ? APEX_PATCH_HOOK : APEX_PATCH_ASM;
        long i = apex_patch_add_op(&ps.patch, type, ps.snippet_name[0] ? ps.snippet_name : sn->id,
                                   ps.snippet_at[0] ? ps.snippet_at : nullptr);
        if (i >= 0) {
            ApexPatchOp &o = ps.patch.ops[i];
            char note[256];
            snprintf(note, sizeof(note), "snippet %s: %s", sn->id, sn->title);
            apex_patch_set_field(&o.code, code);
            apex_patch_set_field(&o.note, note);
            if (!ps.snippet_at[0])
                apex_patch_set_field(&o.alloc, ps.snippet_alloc);
            ps.selected = (int)i;
            ps.editor_for = -1;
            finish_add(project, state);
            ps.snippet_open = false;
        }
    }
    ImGui::EndDisabled();
    if (!can && code)
        ImGui::SetItemTooltip(hook ? "a hook needs an address (at)"
                                   : "give an address (at) or where to allocate (alloc)");
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ps.snippet_open = false;
    free(code);
    ImGui::End();
}

/* The string at ROM offset off as patch text (\n \t \a escapes). */
static std::string string_text_at(const uint8_t *rom, size_t size, size_t off) {
    std::string t;
    for (size_t k = off; k < size && rom[k] && t.size() < 256; k++) {
        uint8_t ch = rom[k];
        if (ch == 0x0a)
            t += "\\n";
        else if (ch == 0x09)
            t += "\\t";
        else if (ch == 0x07)
            t += "\\a";
        else
            t += (char)ch;
    }
    return t;
}

/* Instruction text of a rendered line without comment and indentation. */
static std::string line_insn(const ApexRenderedLine *l) {
    std::string t(l->text, l->length);
    size_t c = t.find(';');
    if (c != std::string::npos)
        t.erase(c);
    size_t a = t.find_first_not_of(" \t");
    size_t b = t.find_last_not_of(" \t");
    return a == std::string::npos ? "" : t.substr(a, b - a + 1);
}

/* Patch menu of a data line: a string, a value (table field, data byte,
   inline argument), or a whole table / data block to move. */
static void patch_data_menu(ApexProject *project, const ApexRenderedDocument **document_ptr,
                            UiState *state, const ApexRenderedLine *line) {
    PatchUiState &ps = state->patch_state;
    (void)document_ptr;
    if (line->kind != APEX_RENDER_LINE_DIRECTIVE && line->kind != APEX_RENDER_LINE_LABEL)
        return;
    std::string t = line_insn(line);
    bool is_string = t.compare(0, 6, "STRING") == 0;
    bool is_value = t.compare(0, 3, ".DB") == 0 || t.compare(0, 6, "TABLE_") == 0 ||
                    t.compare(0, 7, "INLINE_") == 0 || t.compare(0, 3, ".DW") == 0;
    bool movable =
        line->kind == APEX_RENDER_LINE_LABEL &&
        (line->block_kind == APEX_RENDER_BLOCK_DATA || line->block_kind == APEX_RENDER_BLOCK_TABLE);
    const DataRange *dr = data_range_at(line->bank, line->cpu_addr, &project->data_ranges);
    bool is_frame = line->kind == APEX_RENDER_LINE_LABEL && dr && dr->addr == line->cpu_addr &&
                    dr->kind == DATA_DMD_FULLFRAME;
    bool is_sprite = line->kind == APEX_RENDER_LINE_LABEL && dr && dr->addr == line->cpu_addr &&
                     (dr->kind == DATA_SPRITE || dr->kind == DATA_SPRITE_NOHEADER);
    if (!is_string && !is_value && !movable && !is_frame && !is_sprite)
        return;
    if (!ImGui::BeginMenu("Patch"))
        return;
    const uint8_t *rom = ps.orig_rom.empty() ? project->rom.data : ps.orig_rom.data();
    if (is_string && ImGui::MenuItem("Edit string...")) {
        std::string old = string_text_at(rom, project->rom.size, line->rom_addr);
        if (add_op(project, state, APEX_PATCH_STRING, "text", line) >= 0) {
            apex_patch_set_field(&ps.patch.ops[ps.selected].text, old.c_str());
            finish_add(project, state);
        }
    }
    if (is_value && ImGui::MenuItem("Edit value...")) {
        /* the line's first byte; width / offset can be changed in the editor */
        if (add_op(project, state, APEX_PATCH_VALUE, "value", line) >= 0) {
            char v[8];
            snprintf(v, sizeof(v), "0x%02x", rom[line->rom_addr]);
            apex_patch_set_field(&ps.patch.ops[ps.selected].value, v);
            ps.patch.ops[ps.selected].width = 1;
            finish_add(project, state);
        }
    }
    if (is_frame && ImGui::MenuItem("Replace frame from image...")) {
        /* the editor's file field and browse button pick the image */
        if (add_op(project, state, APEX_PATCH_DMD, "frame", line) >= 0)
            finish_add(project, state);
    }
    if (is_sprite && ImGui::MenuItem("Replace sprite from image...")) {
        if (add_op(project, state, APEX_PATCH_SPRITE, "sprite", line) >= 0)
            finish_add(project, state);
    }
    if ((movable || is_string) && ImGui::MenuItem("Move to free space...")) {
        if (add_op(project, state, APEX_PATCH_MOVE, "move", line) >= 0)
            finish_add(project, state);
    }
    ImGui::EndMenu();
}

void patch_context_menu(ApexProject *project, const ApexRenderedDocument **document_ptr,
                        UiState *state, const ApexRenderedLine *line) {
    (void)document_ptr;
    PatchUiState &ps = state->patch_state;
    if (!line || !line->has_location)
        return;
    if (line->kind != APEX_RENDER_LINE_INSTRUCTION) {
        patch_data_menu(project, document_ptr, state, line);
        return;
    }
    if (!ImGui::BeginMenu("Patch"))
        return;
    if (ImGui::MenuItem("From snippet...")) {
        char at[160];
        apex_patch_format_at(project, line->bank, line->cpu_addr, at, sizeof(at));
        open_snippet_dialog(state, at);
    }
    const uint8_t *rom = ps.orig_rom.empty() ? project->rom.data : ps.orig_rom.data();
    size_t off = line->rom_addr;
    const uint8_t *op = rom + off;
    size_t avail = project->rom.size - off;
    char inst[64];
    Cpu6809InstrInfo info = cpu6809_disassemble_info(op, avail, line->cpu_addr, inst, sizeof(inst));
    bool short_cond = info.size == 2 && op[0] >= 0x22u && op[0] <= 0x2fu;
    bool long_cond = info.size == 4 && op[0] == 0x10u && op[1] >= 0x22u && op[1] <= 0x2fu;

    if (ImGui::MenuItem("NOP out instruction")) {
        if (add_op(project, state, APEX_PATCH_NOP, "nop", line) >= 0) {
            ps.patch.ops[ps.selected].count = 1;
            finish_add(project, state);
        }
    }
    if ((short_cond || long_cond) && ImGui::BeginMenu("Branch")) {
        static const char *const modes[] = {"invert", "always", "never"};
        int which = -1;
        if (ImGui::MenuItem("Invert condition"))
            which = 0;
        if (ImGui::MenuItem("Always branch"))
            which = 1;
        if (ImGui::MenuItem("Never branch"))
            which = 2;
        if (which >= 0 && add_op(project, state, APEX_PATCH_BRANCH, modes[which], line) >= 0) {
            apex_patch_set_field(&ps.patch.ops[ps.selected].mode, modes[which]);
            finish_add(project, state);
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Hook here (detour)...")) {
        if (add_op(project, state, APEX_PATCH_HOOK, "hook", line) >= 0) {
            apex_patch_set_field(&ps.patch.ops[ps.selected].code,
                                 "    ; your code: runs before the replaced instructions\n"
                                 "    NOP");
            finish_add(project, state);
        }
    }
    /* JSR / JMP extended, BSR / BRA / LBSR / LBRA and conditional branches */
    bool redirectable =
        info.has_target &&
        ((info.size == 3 &&
          (op[0] == 0xbdu || op[0] == 0x7eu || op[0] == 0x17u || op[0] == 0x16u)) ||
         (info.size == 2 && (op[0] == 0x8du || op[0] == 0x20u)) || short_cond || long_cond);
    if (redirectable && ImGui::MenuItem("Change target...")) {
        if (add_op(project, state, APEX_PATCH_REDIRECT, "redirect", line) >= 0) {
            char t[160];
            uint8_t tb = info.target >= 0x8000u ? 0xffu : line->bank;
            apex_patch_format_at(project, tb, info.target, t, sizeof(t));
            apex_patch_set_field(&ps.patch.ops[ps.selected].target, t);
            finish_add(project, state);
        }
    }
    if (redirectable && ImGui::IsItemHovered())
        ImGui::SetTooltip("A redirect op: set the new target (a label, also of patch code) in the "
                          "editor");
    if (ImGui::MenuItem("Replace with asm...")) {
        if (add_op(project, state, APEX_PATCH_ASM, "asm", line) >= 0) {
            apex_patch_set_field(&ps.patch.ops[ps.selected].code, line_insn(line).c_str());
            finish_add(project, state);
        }
    }
    if (ImGui::MenuItem("Edit bytes...")) {
        if (add_op(project, state, APEX_PATCH_BYTES, "bytes", line) >= 0) {
            apex_patch_set_field(&ps.patch.ops[ps.selected].bytes,
                                 hex_bytes(op, info.size ? info.size : 1).c_str());
            finish_add(project, state);
        }
    }
    ImGui::EndMenu();
}

void patch_menu(ApexProject *project, const ApexRenderedDocument **document_ptr, UiState *state) {
    (void)document_ptr;
    PatchUiState &ps = state->patch_state;
    if (!ImGui::BeginMenu("Patch"))
        return;
    ImGui::MenuItem("Patch Window", NULL, &state->show_patch);
    bool pv = ps.patched_view;
    if (ImGui::MenuItem("Show Patched ROM", NULL, &pv, ps.built && ps.build.rom != nullptr))
        request_view(project, state, pv);
    if (ImGui::MenuItem("Test in PinMAME", NULL, false, ps.built && !ps.build.errors))
        test_in_pinmame(project, state);
    ImGui::EndMenu();
}

/* ---- window ---- */

static ImVec4 status_color(int st) {
    switch (st) {
    case APEX_PATCH_OK:
        return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
    case APEX_PATCH_WARN:
        return ImVec4(0.95f, 0.75f, 0.2f, 1.0f);
    case APEX_PATCH_ERROR:
        return ImVec4(1.0f, 0.4f, 0.35f, 1.0f);
    default:
        return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    }
}

static const char *status_word(int st) {
    return st == APEX_PATCH_OK      ? "ok"
           : st == APEX_PATCH_WARN  ? "warn"
           : st == APEX_PATCH_ERROR ? "error"
                                    : "off";
}

/* Original vs patched code of one op result, one instruction per line. */
static void show_code(const char *tag, const uint8_t *b, size_t n, uint32_t addr, ImVec4 col) {
    size_t pos = 0;
    while (pos < n) {
        char inst[64];
        size_t k = cpu6809_disassemble(b + pos, n - pos, addr + (uint32_t)pos, inst, sizeof(inst));
        if (!k || pos + k > n) {
            snprintf(inst, sizeof(inst), ".DB 0x%02x", b[pos]);
            k = 1;
        }
        ImGui::TextColored(col, "%s %04x  %s", tag, (unsigned)(addr + pos), inst);
        pos += k;
    }
}

/* Review: the references a data op rewrites, and the ones it cannot. */
static void render_fixups(const ApexRenderedDocument **document_ptr, UiState *state,
                          const ApexPatchOpResult *r) {
    PatchUiState &ps = state->patch_state;
    int manual = 0;
    for (size_t k = 0; k < r->fixup_count; k++)
        manual += ps.build.fixups[r->fixup_first + k].status != APEX_FIXUP_OK;
    ImGui::Text("References: %lu, %d need a manual fix", (unsigned long)r->fixup_count, manual);
    ImGui::SameLine();
    ImGui::TextDisabled("(computed addresses are not found -- test the result)");
    if (!ImGui::BeginTable("fixups", 4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                           ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6)))
        return;
    ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, ui_w(100.0f));
    ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthFixed, ui_w(110.0f));
    ImGui::TableSetupColumn("Pointer", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, ui_w(110.0f));
    ImGui::TableHeadersRow();
    static const char *const st[] = {"ok", "manual", "unreachable"};
    for (size_t k = 0; k < r->fixup_count; k++) {
        const ApexPatchFixup &f = ps.build.fixups[r->fixup_first + k];
        ImGui::PushID((int)k);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char b[24];
        snprintf(b, sizeof(b), "B%02x_A%04x", f.src_bank, (unsigned)f.src_addr);
        if (ImGui::Selectable(b, false, ImGuiSelectableFlags_SpanAllColumns)) {
            size_t li;
            if (apex_render_find_line_by_address(*document_ptr, f.src_bank, f.src_addr, &li))
                select_line(state, li, 1);
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(f.what);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s B%02x_A%04x -> B%02x_A%04x", f.far ? "far " : "word", f.old_bank,
                    (unsigned)f.old_addr, f.new_bank, (unsigned)f.new_addr);
        ImGui::TableSetColumnIndex(3);
        ImGui::TextColored(
            status_color(f.status == APEX_FIXUP_OK ? APEX_PATCH_OK : APEX_PATCH_ERROR), "%s",
            st[f.status]);
        if (f.status == APEX_FIXUP_UNREACHABLE && ImGui::IsItemHovered())
            ImGui::SetTooltip("a 16-bit pointer only reaches its own bank: keep the data in "
                              "bank 0x%02x",
                              f.old_bank);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void render_editor(ApexProject *project, const ApexRenderedDocument **document_ptr,
                          UiState *state) {
    PatchUiState &ps = state->patch_state;
    if (ps.selected < 0 || (size_t)ps.selected >= ps.patch.count)
        return;
    if (ps.editor_for != ps.selected)
        load_editor(ps);
    ApexPatchOp &o = ps.patch.ops[ps.selected];
    bool edit = false;

    ImGui::SeparatorText("Op");
    ImGui::SetNextItemWidth(ui_w(200));
    if (ImGui::InputText("name", ps.ed_name, sizeof(ps.ed_name)) && ps.ed_name[0]) {
        bool clash = false;
        for (size_t i = 0; i < ps.patch.count; i++)
            clash = clash || ((int)i != ps.selected && !strcmp(ps.patch.ops[i].name, ps.ed_name));
        if (!clash) {
            apex_patch_set_field(&o.name, ps.ed_name);
            edit = true;
        }
    }
    ImGui::SameLine();
    bool en = o.enabled != 0;
    if (ImGui::Checkbox("enabled", &en)) {
        o.enabled = en ? 1 : 0;
        edit = true;
    }
    ImGui::SameLine();
    int type = o.type;
    ImGui::SetNextItemWidth(ui_w(90));
    if (ImGui::Combo("type", &type,
                     "bytes\0nop\0asm\0hook\0ptr\0ram\0free\0reserve\0value\0string\0move\0"
                     "blob\0dmd\0sprite\0branch\0redirect\0")) {
        o.type = type;
        edit = true;
    }
    bool allocated_asm = (o.type == APEX_PATCH_ASM || o.type == APEX_PATCH_BLOB) && o.alloc;
    if (!allocated_asm) {
        ImGui::SetNextItemWidth(ui_w(260));
        if (ImGui::InputTextWithHint("at",
                                     o.type == APEX_PATCH_RAM ? "RAM address, e.g. 0x0100"
                                                              : "label+offset or Bxx_Ayyyy",
                                     ps.ed_at, sizeof(ps.ed_at))) {
            apex_patch_set_field(&o.at, ps.ed_at);
            edit = true;
        }
    }
    if (o.type == APEX_PATCH_ASM || o.type == APEX_PATCH_HOOK || o.type == APEX_PATCH_MOVE ||
        o.type == APEX_PATCH_STRING || o.type == APEX_PATCH_BLOB || o.type == APEX_PATCH_DMD ||
        o.type == APEX_PATCH_SPRITE) {
        ImGui::SetNextItemWidth(ui_w(160));
        if (ImGui::InputTextWithHint(o.type == APEX_PATCH_HOOK ? "detour code in" : "alloc",
                                     o.type == APEX_PATCH_HOOK
                                         ? "same (default), system, bank:NN"
                                         : "empty = in place; system, any, bank:NN",
                                     ps.ed_alloc, sizeof(ps.ed_alloc))) {
            apex_patch_set_field(&o.alloc, ps.ed_alloc);
            edit = true;
        }
        if (o.placed) {
            ImGui::SameLine();
            ImGui::TextDisabled("placed %s", o.placed);
            ImGui::SameLine();
            if (ImGui::SmallButton("unpin")) {
                apex_patch_set_field(&o.placed, nullptr);
                edit = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Let the next build choose the place again");
        }
    }
    if (o.type == APEX_PATCH_REDIRECT) {
        ImGui::SetNextItemWidth(ui_w(260));
        if (ImGui::InputTextWithHint("target", "label: where the jump / call goes now",
                                     ps.ed_target, sizeof(ps.ed_target))) {
            apex_patch_set_field(&o.target, ps.ed_target);
            edit = true;
        }
    }
    if (o.type == APEX_PATCH_BRANCH) {
        static const char *const bm[] = {"always", "never", "invert"};
        int cur = !o.mode || !strcmp(o.mode, "always") ? 0 : !strcmp(o.mode, "never") ? 1 : 2;
        ImGui::SetNextItemWidth(ui_w(120));
        if (ImGui::Combo("branch", &cur, "always\0never\0invert\0")) {
            apex_patch_set_field(&o.mode, bm[cur]);
            edit = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("always: BRA / LBRA; never: BRN / LBRN; invert: the opposite "
                              "condition");
    }
    if (o.type == APEX_PATCH_PTR) {
        ImGui::SetNextItemWidth(ui_w(260));
        if (ImGui::InputTextWithHint("target", "label the pointer points to", ps.ed_target,
                                     sizeof(ps.ed_target))) {
            apex_patch_set_field(&o.target, ps.ed_target);
            edit = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_w(90));
        if (ImGui::Combo("width", &ps.ed_width, "\0\0word (2)\0far (3)\0")) {
            if (ps.ed_width < 2)
                ps.ed_width = 3;
            o.width = (unsigned)ps.ed_width;
            edit = true;
        }
    }
    if (o.type == APEX_PATCH_RAM || o.type == APEX_PATCH_BLOB) {
        ImGui::SetNextItemWidth(ui_w(200));
        if (ImGui::InputTextWithHint("label",
                                     o.type == APEX_PATCH_RAM ? "variable name for patch code"
                                                              : "name for patch code / ptr ops",
                                     ps.ed_label, sizeof(ps.ed_label))) {
            apex_patch_set_field(&o.label, ps.ed_label);
            edit = true;
        }
    }
    if (o.type == APEX_PATCH_VALUE) {
        ImGui::SetNextItemWidth(ui_w(160));
        if (ImGui::InputTextWithHint("value", "number, enum name, equate, label", ps.ed_value,
                                     sizeof(ps.ed_value))) {
            apex_patch_set_field(&o.value, ps.ed_value);
            edit = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_w(70));
        if (ImGui::Combo("width##val", &ps.ed_width, "\0byte\0word\0far\0")) {
            if (ps.ed_width < 1)
                ps.ed_width = 1;
            o.width = (unsigned)ps.ed_width;
            edit = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_w(170));
        if (ImGui::BeginCombo("enum type", o.vtype ? o.vtype : "-")) {
            if (ImGui::Selectable("-", !o.vtype)) {
                apex_patch_set_field(&o.vtype, nullptr);
                edit = true;
            }
            for (size_t t = 0; t < project->config_types.count; t++) {
                const char *tn = project->config_types.items[t].name;
                if (ImGui::Selectable(tn, o.vtype && !strcmp(o.vtype, tn))) {
                    apex_patch_set_field(&o.vtype, tn);
                    edit = true;
                }
            }
            ImGui::EndCombo();
        }
        /* the type's enum names, one click to use */
        const ConfigType *ct =
            o.vtype ? find_config_type(&project->config_types, o.vtype) : nullptr;
        if (ct && ct->value_count && ImGui::BeginCombo("##enumval", "pick a value")) {
            for (size_t v = 0; v < ct->value_count; v++) {
                char lb[160];
                snprintf(lb, sizeof(lb), "0x%02x  %s", (unsigned)ct->values[v].value,
                         ct->values[v].name);
                if (ImGui::Selectable(lb)) {
                    apex_patch_set_field(&o.value, ct->values[v].name);
                    snprintf(ps.ed_value, sizeof(ps.ed_value), "%s", ct->values[v].name);
                    edit = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    if (o.type == APEX_PATCH_STRING) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##text", "new text (\\n \\t \\a escapes)", ps.ed_text,
                                     sizeof(ps.ed_text))) {
            apex_patch_set_field(&o.text, ps.ed_text);
            edit = true;
        }
        ImGui::SetNextItemWidth(ui_w(110));
        if (ImGui::Combo("mode", &ps.ed_mode, "auto\0inplace\0shift\0relocate\0")) {
            static const char *const modes[] = {nullptr, "inplace", "shift", "relocate"};
            apex_patch_set_field(&o.mode, modes[ps.ed_mode]);
            edit = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("auto: in place if it fits, else move the strings behind it up\n"
                              "(if the bytes after them are free), else relocate it");
    }
    bool image = o.type == APEX_PATCH_DMD || o.type == APEX_PATCH_SPRITE;
    if (o.type == APEX_PATCH_BLOB || image) {
        ImGui::SetNextItemWidth(ui_w(320));
        if (ImGui::InputTextWithHint("file",
                                     o.type == APEX_PATCH_DMD ? "128x32 PNG / PBM / PGM (relative)"
                                     : o.type == APEX_PATCH_SPRITE ? "PNG / PBM / PGM (relative)"
                                                                   : "binary file (relative)",
                                     ps.ed_file, sizeof(ps.ed_file))) {
            apex_patch_set_field(&o.file, ps.ed_file);
            edit = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("browse##asset")) {
            IGFD::FileDialogConfig cfg;
            cfg.path = dir_of(abs_path(ps.path).c_str());
            ImGuiFileDialog::Instance()->OpenDialog("PatchAsset",
                                                    image ? "Select image" : "Select file",
                                                    image ? ".png,.pbm,.pgm" : ".*", cfg);
        }
        center_file_dialog("PatchAsset");
        if (ImGuiFileDialog::Instance()->Display("PatchAsset", ImGuiWindowFlags_NoCollapse,
                                                 ImVec2(640, 420))) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string rel =
                    rel_to(ps.path, ImGuiFileDialog::Instance()->GetFilePathName().c_str());
                snprintf(ps.ed_file, sizeof(ps.ed_file), "%s", rel.c_str());
                apex_patch_set_field(&o.file, ps.ed_file);
                edit = true;
            }
            ImGuiFileDialog::Instance()->Close();
        }
    }
    if (image) {
        ImGui::SetNextItemWidth(ui_w(100));
        if (ImGui::Combo("mode##dmd", &ps.ed_mode, "auto\0inplace\0relocate\0")) {
            static const char *const modes[] = {nullptr, "inplace", "relocate"};
            apex_patch_set_field(&o.mode, modes[ps.ed_mode]);
            edit = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("auto: in place if the new image is not longer, else relocate it\n"
                              "and rewrite the pointers to it");
    }
    if (o.type == APEX_PATCH_DMD) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_w(80));
        if (ImGui::InputInt("planes", &ps.ed_planes)) {
            ps.ed_planes = ps.ed_planes < 0 ? 0 : ps.ed_planes > 4 ? 4 : ps.ed_planes;
            o.planes = (unsigned)ps.ed_planes;
            edit = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = as the analysis says (dmd_fullframe[N]), else 1");
        ImGui::SameLine();
        int raw = o.encoding && !strcmp(o.encoding, "raw");
        ImGui::SetNextItemWidth(ui_w(130));
        if (ImGui::Combo("encoding", &raw, "smallest\0raw\0")) {
            apex_patch_set_field(&o.encoding, raw ? "raw" : nullptr);
            edit = true;
        }
        ImGui::SameLine();
        int br = o.bright < 0 ? 1 : o.bright;
        ImGui::SetNextItemWidth(ui_w(110));
        if (ImGui::Combo("bright plane", &br, "first\0second\0")) {
            o.bright = br == 1 ? -1 : 0;
            edit = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("2-plane frames: the plane shown longer (the brighter level)");
    } else if (o.type == APEX_PATCH_SPRITE) {
        ImGui::SameLine();
        int br = o.bright < 0 ? 1 : o.bright;
        ImGui::SetNextItemWidth(ui_w(110));
        if (ImGui::Combo("bright plane", &br, "plane 0\0plane 1\0")) {
            o.bright = br == 1 ? -1 : 0;
            edit = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Bicolor sprites (0xFE / 0xFF): the plane that gives the brighter "
                              "grey level.\nThe size, offsets and header type stay; the image "
                              "sets width and height.");
    }
    if (o.type == APEX_PATCH_STRING || o.type == APEX_PATCH_MOVE || image) {
        if (o.type == APEX_PATCH_STRING)
            ImGui::SameLine();
        bool acc = o.accept != 0;
        if (ImGui::Checkbox("accept unresolved references", &acc)) {
            o.accept = acc ? 1 : 0;
            edit = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Build even if some references cannot be rewritten (fix them by "
                              "hand with other ops)");
    }
    const ApexPatchOpResult *r =
        ps.built && (size_t)ps.selected < ps.build.count ? &ps.build.ops[ps.selected] : nullptr;
    if (r && r->length) {
        ImGui::SameLine();
        ImGui::TextDisabled("= B%02x_A%04x, %lu byte(s)", r->bank, (unsigned)r->addr,
                            (unsigned long)r->length);
        ImGui::SameLine();
        if (ImGui::SmallButton("jump")) {
            size_t li;
            if (apex_render_find_line_by_address(*document_ptr, r->bank, r->addr, &li))
                select_line(state, li, 1);
        }
    }
    bool writes_at = !allocated_asm && o.type != APEX_PATCH_RAM && o.type != APEX_PATCH_FREE &&
                     o.type != APEX_PATCH_RESERVE;
    if (writes_at)
        ImGui::SetNextItemWidth(ui_w(260));
    if (writes_at && ImGui::InputTextWithHint("expect", "original bytes (guard)", ps.ed_expect,
                                              sizeof(ps.ed_expect))) {
        apex_patch_set_field(&o.expect, ps.ed_expect);
        edit = true;
    }
    if (writes_at && r && r->orig && r->length) {
        ImGui::SameLine();
        if (ImGui::SmallButton("from ROM")) {
            std::string h =
                r->guard ? hex_bytes(r->guard, r->guard_len) : hex_bytes(r->orig, r->length);
            apex_patch_set_field(&o.expect, h.c_str());
            snprintf(ps.ed_expect, sizeof(ps.ed_expect), "%s", h.c_str());
            edit = true;
        }
    }
    if (o.type == APEX_PATCH_BYTES) {
        ImGui::SetNextItemWidth(ui_w(260));
        if (ImGui::InputText("bytes", ps.ed_bytes, sizeof(ps.ed_bytes))) {
            apex_patch_set_field(&o.bytes, ps.ed_bytes);
            edit = true;
        }
    } else if (o.type == APEX_PATCH_RAM || o.type == APEX_PATCH_FREE ||
               o.type == APEX_PATCH_RESERVE || o.type == APEX_PATCH_MOVE) {
        ImGui::SetNextItemWidth(ui_w(90));
        if (ImGui::InputInt("length", &ps.ed_length)) {
            if (ps.ed_length < 0)
                ps.ed_length = 0;
            o.length = (unsigned)ps.ed_length;
            edit = true;
        }
    } else if (o.type == APEX_PATCH_NOP || (o.type == APEX_PATCH_ASM && !o.alloc)) {
        ImGui::SetNextItemWidth(ui_w(90));
        if (o.type == APEX_PATCH_NOP && ImGui::InputInt("count", &ps.ed_count)) {
            if (ps.ed_count < 0)
                ps.ed_count = 0;
            o.count = (unsigned)ps.ed_count;
            edit = true;
        }
        if (o.type == APEX_PATCH_NOP)
            ImGui::SameLine();
        ImGui::SetNextItemWidth(ui_w(90));
        if (ImGui::InputInt("length", &ps.ed_length)) {
            if (ps.ed_length < 0)
                ps.ed_length = 0;
            o.length = (unsigned)ps.ed_length;
            edit = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(o.type == APEX_PATCH_NOP ? "(instructions / bytes; 0 = 1 instr.)"
                                                     : "(bytes replaced; 0 = up to the next "
                                                       "instruction boundary)");
    }
    if (o.type == APEX_PATCH_HOOK)
        ImGui::TextDisabled("code runs before the replaced instructions, then execution "
                            "continues after them");
    if (o.type == APEX_PATCH_ASM || o.type == APEX_PATCH_HOOK) {
        ImGui::SetNextItemWidth(ui_w(260));
        if (ImGui::InputTextWithHint("file", "or a .asm file (relative)", ps.ed_file,
                                     sizeof(ps.ed_file))) {
            apex_patch_set_field(&o.file, ps.ed_file);
            edit = true;
        }
        if (ImGui::InputTextMultiline("##code", ps.ed_code, sizeof(ps.ed_code),
                                      ImVec2(-1, ImGui::GetTextLineHeight() * 8),
                                      ImGuiInputTextFlags_AllowTabInput)) {
            apex_patch_set_field(&o.code, ps.ed_code);
            edit = true;
        }
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##note", "note", ps.ed_note, sizeof(ps.ed_note))) {
        apex_patch_set_field(&o.note, ps.ed_note);
        edit = true;
    }
    if (o.type == APEX_PATCH_RAM && o.at) {
        /* Is the RAM used by the game?  Static references in the listing, and
           the dynamic check through PinMAME's access trace. */
        uint32_t ra = (uint32_t)strtoul(o.at, nullptr, 0);
        char hx[16];
        snprintf(hx, sizeof(hx), "0x%04x", (unsigned)ra);
        size_t refs = find_ram_refs(*document_ptr, hx).size();
        if (refs)
            ImGui::TextColored(status_color(APEX_PATCH_WARN),
                               "%zu instruction(s) of the game access 0x%04x directly", refs,
                               (unsigned)ra);
        else
            ImGui::TextDisabled("no direct access to 0x%04x in the analysed code (indexed "
                                "access is possible)",
                                (unsigned)ra);
        ImGui::SameLine();
        if (ImGui::SmallButton("check in PinMAME")) {
            snprintf(state->pinmame.xref_addr, sizeof(state->pinmame.xref_addr), "%04x",
                     (unsigned)ra);
            state->pinmame.xref_mode = 3;
            state->show_pinmame = true;
            set_status(state, "PinMAME: RAM xref prepared -- run the game and Collect accessors");
        }
    }
    if (edit)
        changed(project, state);

    r = ps.built && (size_t)ps.selected < ps.build.count ? &ps.build.ops[ps.selected] : nullptr;
    if (r) {
        ImGui::TextColored(status_color(r->status), "%s", status_word(r->status));
        if (r->msg[0]) {
            ImGui::SameLine();
            ImGui::TextWrapped("%s", r->msg);
        }
        if (r->length && r->orig && r->patched) {
            if (o.type == APEX_PATCH_BYTES || o.type == APEX_PATCH_PTR ||
                o.type == APEX_PATCH_VALUE) {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.5f, 1.0f), "- %s",
                                   hex_bytes(r->orig, r->length).c_str());
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.55f, 1.0f), "+ %s",
                                   hex_bytes(r->patched, r->length).c_str());
            } else if (!(o.type == APEX_PATCH_ASM && o.alloc) && o.type != APEX_PATCH_STRING &&
                       o.type != APEX_PATCH_MOVE && o.type != APEX_PATCH_DMD &&
                       o.type != APEX_PATCH_SPRITE && o.type != APEX_PATCH_BLOB) {
                show_code("-", r->orig, r->length, r->addr, ImVec4(1.0f, 0.55f, 0.5f, 1.0f));
                show_code("+", r->patched, r->length, r->addr, ImVec4(0.5f, 0.9f, 0.55f, 1.0f));
            }
        }
        if (r->has_alloc && r->alloc_bytes && o.type != APEX_PATCH_STRING &&
            o.type != APEX_PATCH_MOVE && o.type != APEX_PATCH_DMD && o.type != APEX_PATCH_SPRITE &&
            o.type != APEX_PATCH_BLOB) {
            ImGui::TextDisabled(
                "%s at B%02x_A%04x (%lu byte(s)):", o.type == APEX_PATCH_HOOK ? "detour" : "code",
                r->alloc_bank, (unsigned)r->alloc_addr, (unsigned long)r->alloc_length);
            show_code("+", r->alloc_bytes, r->alloc_length, r->alloc_addr,
                      ImVec4(0.5f, 0.9f, 0.55f, 1.0f));
        }
        if (o.type == APEX_PATCH_STRING && r->guard)
            ImGui::TextDisabled("\"%.*s\" -> \"%s\"  (%s%s)", (int)r->guard_len - 1,
                                (const char *)r->guard, o.text ? o.text : "", r->mode_used,
                                r->has_alloc ? ", relocated" : "");
        if (image && r->status != APEX_PATCH_ERROR && r->length) {
            /* the image before and after the patch */
            const uint8_t *orig = ps.orig_rom.empty() ? project->rom.data : ps.orig_rom.data();
            int br = o.bright < 0 ? 1 : o.bright;
            float sc = o.type == APEX_PATCH_DMD ? 2.0f : 4.0f;
            PreviewImage before = decode_preview(project, o, orig, project->rom.size, r->bank,
                                                 r->addr, r->bank, r->addr);
            PreviewImage after =
                ps.build.rom
                    ? decode_preview(project, o, ps.build.rom, ps.build.rom_size,
                                     r->has_alloc ? r->alloc_bank : r->bank,
                                     r->has_alloc ? r->alloc_addr : r->addr, r->bank, r->addr)
                    : PreviewImage();
            if (o.type == APEX_PATCH_SPRITE) { /* small sprites: larger */
                unsigned mw = std::max(before.ok ? before.w : 1u, after.ok ? after.w : 1u);
                sc = std::min(16.0f, std::max(2.0f, 160.0f / (float)mw));
            }
            draw_preview(before, br, sc, "original");
            ImGui::SameLine(0, 16);
            draw_preview(after, br, sc, "patched");
        }
        if (image && r->status != APEX_PATCH_ERROR)
            ImGui::TextDisabled(
                "%s %lu -> %lu byte(s), %s", o.type == APEX_PATCH_DMD ? "frame" : "sprite",
                (unsigned long)r->old_length,
                (unsigned long)(r->has_alloc ? r->alloc_length : r->length), r->mode_used);
        if (o.type == APEX_PATCH_BLOB && r->status != APEX_PATCH_ERROR)
            ImGui::TextDisabled("%lu byte(s) at B%02x_A%04x",
                                (unsigned long)(r->has_alloc ? r->alloc_length : r->length),
                                r->has_alloc ? r->alloc_bank : r->bank,
                                (unsigned)(r->has_alloc ? r->alloc_addr : r->addr));
        if (r->has_alloc && (o.type == APEX_PATCH_STRING || o.type == APEX_PATCH_MOVE || image))
            ImGui::TextDisabled("new place B%02x_A%04x, %lu byte(s)%s", r->alloc_bank,
                                (unsigned)r->alloc_addr, (unsigned long)r->alloc_length,
                                o.type == APEX_PATCH_STRING ? "" : "; old place cleared to 0xFF");
        if (r->fixup_count)
            render_fixups(document_ptr, state, r);
    }
}

/* (Re)build the static RAM map from the analysis; snapshots are kept. */
static void ram_analyse(ApexProject *project, const ApexRenderedDocument *doc, PatchUiState &ps) {
    unsigned samples = ps.ram.samples;
    uint8_t dyn[APEX_RAM_SIZE];
    for (uint32_t a = 0; a < APEX_RAM_SIZE; a++)
        dyn[a] = ps.ram.use[a] & APEX_RAM_DYNAMIC;
    apex_ram_map_free(&ps.ram);
    memset(&ps.ram, 0, sizeof(ps.ram));
    apex_ram_map_static(project, doc, &ps.ram, 0, 0);
    for (uint32_t a = 0; a < APEX_RAM_SIZE; a++)
        ps.ram.use[a] |= dyn[a];
    ps.ram.samples = samples;
    for (size_t i = 0; i < ps.patch.count; i++) {
        const ApexPatchOp &o = ps.patch.ops[i];
        if (o.type == APEX_PATCH_RAM && o.enabled && o.at)
            apex_ram_map_mark(&ps.ram, (uint32_t)strtoul(o.at, nullptr, 0),
                              o.length ? o.length : 1u, APEX_RAM_PATCH);
    }
    apex_ram_map_runs(&ps.ram, (unsigned)ps.ram_min);
    ps.ram_ready = true;
}

static void render_free_ram(ApexProject *project, const ApexRenderedDocument **document_ptr,
                            UiState *state) {
    PatchUiState &ps = state->patch_state;
    if (ImGui::Button(ps.ram_ready ? "Re-analyse" : "Analyse"))
        ram_analyse(project, *document_ptr, ps);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("RAM the analysed code, [symbols], pointer bases and stacks use, and "
                          "the patch's ram ops");
    ImGui::SameLine();
    ImGui::BeginDisabled(!state->pinmame.connected || !ps.ram_ready);
    if (ImGui::Checkbox("sample PinMAME", &ps.ram_sampling))
        ps.ram_next = 0.0;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Read the RAM twice a second while the game runs: a byte that is ever\n"
                          "non-zero is used.  Play the game (start, multiball, modes) for the\n"
                          "best result -- attract mode alone leaves much RAM untouched.");
    ImGui::SameLine();
    if (ImGui::SmallButton("forget samples")) {
        for (uint32_t a = 0; a < APEX_RAM_SIZE; a++)
            ps.ram.use[a] &= (uint8_t)~APEX_RAM_DYNAMIC;
        ps.ram.samples = 0;
        apex_ram_map_runs(&ps.ram, (unsigned)ps.ram_min);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_w(80));
    if (ImGui::InputInt("min bytes", &ps.ram_min)) {
        ps.ram_min = ps.ram_min < 1 ? 1 : ps.ram_min;
        apex_ram_map_runs(&ps.ram, (unsigned)ps.ram_min);
    }
    if (!ps.ram_ready)
        return;
    unsigned used = 0;
    for (uint32_t a = 0; a < APEX_RAM_SIZE; a++)
        used += ps.ram.use[a] != 0;
    if (ps.ram.samples)
        ImGui::Text("%u of %u bytes used; %u snapshot(s); %lu candidate run(s)", used,
                    APEX_RAM_SIZE, ps.ram.samples, (unsigned long)ps.ram.count);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "%u of %u bytes used by the code; %lu run(s) -- unverified: sample "
                           "the running game",
                           used, APEX_RAM_SIZE, (unsigned long)ps.ram.count);
    if (!ImGui::BeginTable("freeram", 4,
                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY,
                           ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("RAM", ImGuiTableColumnFlags_WidthFixed, ui_w(70.0f));
    ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, ui_w(50.0f));
    ImGui::TableSetupColumn("Evidence", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ui_w(40.0f));
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < ps.ram.count; i++) {
        const ApexRamRun &r = ps.ram.runs[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("0x%04x", (unsigned)r.addr);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u", (unsigned)r.len);
        ImGui::TableSetColumnIndex(2);
        if (r.nvram)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f),
                               "NVRAM area (audits / settings?) -- risky");
        else if (r.verified)
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.55f, 1.0f), "no code use, zero while sampled");
        else
            ImGui::TextDisabled("no code use (not sampled)");
        ImGui::TableSetColumnIndex(3);
        if (ImGui::SmallButton("use")) {
            ensure_patch(project, state);
            char at[16];
            snprintf(at, sizeof(at), "0x%04x", (unsigned)r.addr);
            long k = apex_patch_add_op(&ps.patch, APEX_PATCH_RAM, "var", at);
            if (k >= 0) {
                apex_patch_set_field(&ps.patch.ops[k].label, "NewVar");
                ps.selected = (int)k;
                ps.editor_for = -1;
                changed(project, state);
                ram_analyse(project, *document_ptr, ps); /* the new op claims it */
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Add a ram op (1 byte, label NewVar) here");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/* Free space per bank: before the patch and what is left. */
static void render_free_map(UiState *state) {
    PatchUiState &ps = state->patch_state;
    if (!ps.built)
        return;
    unsigned long tot[256] = {0}, left[256] = {0}, big[256] = {0};
    for (size_t i = 0; i < ps.build.free_total.count; i++)
        tot[ps.build.free_total.items[i].bank] += ps.build.free_total.items[i].len;
    for (size_t i = 0; i < ps.build.free_left.count; i++) {
        const ApexFreeBlock &b = ps.build.free_left.items[i];
        left[b.bank] += b.len;
        if (b.len > big[b.bank])
            big[b.bank] = b.len;
    }
    if (ImGui::BeginTable("freemap", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Bank", ImGuiTableColumnFlags_WidthFixed, ui_w(44.0f));
        ImGui::TableSetupColumn("Free", ImGuiTableColumnFlags_WidthFixed, ui_w(64.0f));
        ImGui::TableSetupColumn("Patch", ImGuiTableColumnFlags_WidthFixed, ui_w(64.0f));
        ImGui::TableSetupColumn("Largest left", ImGuiTableColumnFlags_WidthFixed, ui_w(90.0f));
        ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int b = 0; b < 256; b++) {
            if (!tot[b])
                continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text(b == 0xff ? "sys" : "%02x", b);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%lu", tot[b]);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%lu", tot[b] - left[b]);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%lu", big[b]);
            ImGui::TableSetColumnIndex(4);
            char ov[32];
            snprintf(ov, sizeof(ov), "%lu%%", (tot[b] - left[b]) * 100ul / tot[b]);
            ImGui::ProgressBar((float)(tot[b] - left[b]) / (float)tot[b], ImVec2(-1, 0), ov);
        }
        ImGui::EndTable();
    }
}

bool patch_undo_active(const UiState *state) {
    return state->patch_state.focused && state->patch_state.loaded;
}

void patch_undo(ApexProject *project, UiState *state, bool redo) {
    PatchUiState &ps = state->patch_state;
    std::vector<std::string> &from = redo ? ps.redo : ps.undo;
    std::vector<std::string> &to = redo ? ps.undo : ps.redo;
    if (!ps.loaded || from.empty())
        return;
    record_undo(ps); /* an edit not yet built */
    if (from.empty())
        return;
    std::string text = from.back();
    char err[256];
    ApexPatch p;
    if (apex_patch_load_text(ps.path, text.c_str(), &p, err, sizeof(err)) != 0) {
        apex_patch_free(&p);
        set_status(state, err);
        return;
    }
    from.pop_back();
    to.push_back(ps.snap);
    int sel = ps.selected;
    if (ps.built)
        apex_patch_build_free(&ps.build);
    ps.built = false;
    apex_patch_free(&ps.patch);
    ps.patch = p;
    ps.snap = text;
    ps.snap_time = 0.0;
    ps.selected = sel < (int)p.count ? sel : (int)p.count - 1;
    ps.editor_for = -1;
    ps.dirty = ps.snap != ps.saved;
    snprintf(ps.ed_output, sizeof(ps.ed_output), "%s", p.output ? p.output : "");
    patch_rebuild(project, state);
    set_status(state, redo ? "patch: redone" : "patch: undone");
}

void render_patch_window(ApexProject *project, const ApexRenderedDocument **document_ptr,
                         UiState *state) {
    PatchUiState &ps = state->patch_state;
    ps.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!ps.path[0])
        snprintf(ps.path, sizeof(ps.path), "%s", default_patch_path(project).c_str());

    ImGui::SetNextItemWidth(-ui_w(260));
    ImGui::InputText("##patchpath", ps.path, sizeof(ps.path));
    ImGui::SameLine();
    if (ImGui::Button("Open"))
        patch_open_path(project, state, ps.path);
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        FILE *f = fopen(ps.path, "r");
        if (f) {
            fclose(f);
            set_status(state, "the file exists; Open it, or choose another name for New");
        } else {
            patch_new(project, state, ps.path);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!ps.loaded);
    if (ImGui::Button(ps.dirty ? "Save*" : "Save"))
        patch_save(state);
    ImGui::EndDisabled();
    if (!ps.loaded) {
        ImGui::TextWrapped("No patch open.  Open or create one -- or use the disassembly's "
                           "right-click Patch menu, which creates %s.",
                           default_patch_path(project).c_str());
        return;
    }

    /* settings + actions */
    int cs = ps.patch.checksum;
    ImGui::SetNextItemWidth(ui_w(110));
    if (ImGui::Combo("checksum", &cs, "fix\0disable\0keep\0")) {
        ps.patch.checksum = cs;
        changed(project, state);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_w(220));
    if (ImGui::InputTextWithHint("output", "patched ROM file", ps.ed_output,
                                 sizeof(ps.ed_output))) {
        apex_patch_set_field(&ps.patch.output, ps.ed_output);
        ps.dirty = true;
    }
    ImGui::BeginDisabled(!ps.built || ps.build.errors);
    if (ImGui::Button("Build ROM")) {
        char dst[1100];
        if (ps.patch.output)
            apex_patch_path(&ps.patch, ps.patch.output, dst, sizeof(dst));
        else
            snprintf(dst, sizeof(dst), "%s.rom", ps.path);
        export_rom(state, dst);
    }
    ImGui::SameLine();
    if (ImGui::Button("Test in PinMAME"))
        test_in_pinmame(project, state);
    ImGui::SameLine();
    ImGui::BeginDisabled(!state->pinmame.connected || !ps.built);
    if (ImGui::Button("Check run"))
        check_run(state);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("After Test in PinMAME (which starts the coverage) and playing:\n"
                          "how much of each op's code the game executed");
    ImGui::SameLine();
    if (ImGui::Button("Export..."))
        ImGui::OpenPopup("PatchExport");
    if (ImGui::BeginPopup("PatchExport")) {
        ImGui::TextDisabled("next to the patch file:");
        if (ImGui::MenuItem("IPS patch (.ips)"))
            export_patchfile(project, state, 0);
        if (ImGui::MenuItem("BPS patch (.bps, CRC-checked)"))
            export_patchfile(project, state, 1);
        if (ImGui::MenuItem("Report (.md)"))
            export_patchfile(project, state, 2);
        ImGui::Separator();
        if (ImGui::MenuItem("Rebase onto another ROM version...")) {
            IGFD::FileDialogConfig cfg;
            cfg.path = dir_of(abs_path(project->rom_path).c_str());
            ImGuiFileDialog::Instance()->OpenDialog("PatchRebase", "Target ROM (other version)",
                                                    ".rom,.bin,.*", cfg);
        }
        ImGui::EndPopup();
    }
    center_file_dialog("PatchRebase");
    if (ImGuiFileDialog::Instance()->Display("PatchRebase", ImGuiWindowFlags_NoCollapse,
                                             ImVec2(640, 420))) {
        if (ImGuiFileDialog::Instance()->IsOk())
            rebase_to(project, state, ImGuiFileDialog::Instance()->GetFilePathName());
        ImGuiFileDialog::Instance()->Close();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(ps.undo.empty());
    if (ImGui::Button("Undo"))
        patch_undo(project, state, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(ps.redo.empty());
    if (ImGui::Button("Redo"))
        patch_undo(project, state, true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Pin guards")) {
        int n = apex_patch_pin(&ps.patch, &ps.build);
        if (n > 0)
            changed(project, state);
        load_editor(ps);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fill in the missing expect guards from the original bytes and pin\n"
                          "every allocation (placed = ...) so later builds keep it there");
    ImGui::SameLine();
    bool pv = ps.patched_view;
    ImGui::BeginDisabled(!ps.built || !ps.build.rom);
    if (ImGui::Checkbox("show patched ROM", &pv))
        request_view(project, state, pv);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Disassemble the patched bytes instead of the original ones");

    if (ps.built) {
        if (ps.build.msg[0])
            ImGui::TextColored(status_color(APEX_PATCH_ERROR), "%s", ps.build.msg);
        ImGui::Text("%lu op(s): %d error(s), %d warning(s)", (unsigned long)ps.patch.count,
                    ps.build.errors, ps.build.warnings);
        if (ps.patch.has_crc) {
            ImGui::SameLine();
            ImGui::TextDisabled("base CRC-32 %08X", (unsigned)ps.patch.base_crc32);
        }
    }

    /* add an op of any type */
    ImGui::SetNextItemWidth(ui_w(100));
    ImGui::Combo("##addtype", &ps.add_type,
                 "bytes\0nop\0asm\0hook\0ptr\0ram\0free\0reserve\0value\0string\0move\0"
                 "blob\0dmd\0sprite\0branch\0redirect\0");
    ImGui::SameLine();
    if (ImGui::Button("Add op")) {
        char at[160] = "";
        const ApexRenderedDocument *d = *document_ptr;
        if (state->selected_line < d->line_count && d->lines[state->selected_line].has_location)
            apex_patch_format_at(project, d->lines[state->selected_line].bank,
                                 d->lines[state->selected_line].cpu_addr, at, sizeof(at));
        int t = ps.add_type;
        const char *nm = apex_patch_type_name(t);
        long i = apex_patch_add_op(&ps.patch, t, t == APEX_PATCH_ASM ? "code" : nm,
                                   t == APEX_PATCH_ASM   ? nullptr
                                   : t == APEX_PATCH_RAM ? "0x0000"
                                                         : at);
        if (i >= 0) {
            ApexPatchOp &o = ps.patch.ops[i];
            if (t == APEX_PATCH_ASM) {
                apex_patch_set_field(&o.alloc, "system");
                apex_patch_set_field(&o.code, "NewCode:\n    ; your code\n    RTS");
            } else if (t == APEX_PATCH_HOOK) {
                apex_patch_set_field(&o.code, "    ; your code\n    NOP");
            } else if (t == APEX_PATCH_PTR) {
                apex_patch_set_field(&o.target, "NewCode");
            } else if (t == APEX_PATCH_RAM) {
                apex_patch_set_field(&o.label, "NewVar");
            } else if (t == APEX_PATCH_FREE || t == APEX_PATCH_RESERVE) {
                o.length = 16;
            } else if (t == APEX_PATCH_BYTES) {
                apex_patch_set_field(&o.bytes, "12");
            } else if (t == APEX_PATCH_STRING && state->selected_line < d->line_count &&
                       d->lines[state->selected_line].has_location) {
                /* start from the text at the cursor */
                const uint8_t *rom = ps.orig_rom.empty() ? project->rom.data : ps.orig_rom.data();
                std::string old =
                    string_text_at(rom, project->rom.size, d->lines[state->selected_line].rom_addr);
                apex_patch_set_field(&o.text, old.empty() ? "TEXT" : old.c_str());
            } else if (t == APEX_PATCH_VALUE) {
                apex_patch_set_field(&o.value, "0x00");
            }
            ps.selected = (int)i;
            ps.editor_for = -1;
            changed(project, state);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("At the disassembly cursor (asm: new code in free space)");
    ImGui::SameLine();
    if (ImGui::Button("From snippet...")) {
        char at[160] = "";
        const ApexRenderedDocument *d = *document_ptr;
        if (state->selected_line < d->line_count && d->lines[state->selected_line].has_location)
            apex_patch_format_at(project, d->lines[state->selected_line].bank,
                                 d->lines[state->selected_line].cpu_addr, at, sizeof(at));
        open_snippet_dialog(state, at);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Insert code from the snippet library (at the disassembly cursor)");
    ImGui::SameLine();
    ImGui::Checkbox("free space", &ps.show_map);
    ImGui::SameLine();
    ImGui::Checkbox("free RAM", &ps.show_ram);
    if (ps.show_ram)
        render_free_ram(project, document_ptr, state);
    if (ps.show_map)
        render_free_map(state);

    /* op list */
    float list_h = ImGui::GetTextLineHeightWithSpacing() * 8.0f;
    int move = 0;
    bool del = false;
    bool show_cov = !ps.cov_total.empty() || !ps.data_traces.empty();
    if (ImGui::BeginTable("patch_ops", show_cov ? 6 : 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                          ImVec2(0, list_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, ui_w(26.0f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ui_w(62.0f));
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        if (show_cov)
            ImGui::TableSetupColumn("Ran", ImGuiTableColumnFlags_WidthFixed, ui_w(70.0f));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < ps.patch.count; i++) {
            ApexPatchOp &o = ps.patch.ops[i];
            const ApexPatchOpResult *r =
                ps.built && i < ps.build.count ? &ps.build.ops[i] : nullptr;
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool en = o.enabled != 0;
            if (ImGui::Checkbox("##en", &en)) {
                o.enabled = en ? 1 : 0;
                ps.editor_for = -1;
                changed(project, state);
            }
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Selectable(o.name, ps.selected == (int)i,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                ps.selected = (int)i;
                if (ImGui::IsMouseDoubleClicked(0) && r && r->length) {
                    size_t li;
                    if (apex_render_find_line_by_address(*document_ptr, r->bank, r->addr, &li))
                        select_line(state, li, 1);
                }
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(apex_patch_type_name(o.type));
            ImGui::TableSetColumnIndex(3);
            if (r && r->length)
                ImGui::Text("B%02x_A%04x +%lu%s", r->bank, (unsigned)r->addr,
                            (unsigned long)r->length,
                            r->has_alloc && o.type == APEX_PATCH_HOOK ? " +detour" : "");
            else if (o.type == APEX_PATCH_RAM && o.label)
                ImGui::TextDisabled("%s = %s", o.label, o.at ? o.at : "?");
            else
                ImGui::TextDisabled("%s", o.at ? o.at : o.alloc ? o.alloc : "-");
            ImGui::TableSetColumnIndex(4);
            if (r) {
                ImGui::TextColored(status_color(r->status), "%s%s%s", status_word(r->status),
                                   r->msg[0] ? ": " : "", r->msg);
                if (r->msg[0] && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", r->msg);
            }
            const PatchUiState::DataTrace *dt = nullptr;
            for (const auto &t : ps.data_traces)
                if (t.op == (int)i)
                    dt = &t;
            if (show_cov && dt) {
                ImGui::TableSetColumnIndex(5);
                if (dt->read)
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.55f, 1.0f), "read");
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "not read");
                if (ImGui::IsItemHovered()) {
                    if (dt->read) {
                        std::string pcs;
                        for (long pc : dt->pcs) {
                            char b[16];
                            snprintf(b, sizeof(b), "%s0x%04lx", pcs.empty() ? "" : ", ", pc);
                            pcs += b;
                        }
                        ImGui::SetTooltip("The game read the new data at B%02x_A%04x\n(from PC "
                                          "%s)",
                                          dt->bank, (unsigned)dt->addr, pcs.c_str());
                    } else {
                        ImGui::SetTooltip("No read of B%02x_A%04x yet in this PinMAME test: play "
                                          "until the game shows / uses it",
                                          dt->bank, (unsigned)dt->addr);
                    }
                }
            } else if (show_cov && i < ps.cov_total.size()) {
                ImGui::TableSetColumnIndex(5);
                int h = ps.cov_hit[i], t = ps.cov_total[i];
                if (t < 0)
                    ImGui::TextDisabled("-");
                else
                    ImGui::TextColored(h ? ImVec4(0.5f, 0.9f, 0.55f, 1.0f)
                                         : ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                                       "%d/%d", h, t);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(t < 0 ? "no code to check (data op)"
                                            : "instructions of the op's code that ran in the "
                                              "PinMAME test (a patched site that never runs\n"
                                              "is not reached by that play: test it, or "
                                              "check the site)");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(ps.selected < 0);
    if (ImGui::SmallButton("up"))
        move = -1;
    ImGui::SameLine();
    if (ImGui::SmallButton("down"))
        move = 1;
    ImGui::SameLine();
    if (ImGui::SmallButton("delete"))
        del = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("ops apply in this order; right-click an instruction for Patch actions");
    if (move && ps.selected >= 0 && apex_patch_move_op(&ps.patch, (size_t)ps.selected, move)) {
        ps.selected += move;
        ps.editor_for = -1;
        changed(project, state);
    }
    if (del && ps.selected >= 0) {
        apex_patch_remove_op(&ps.patch, (size_t)ps.selected);
        if (ps.selected >= (int)ps.patch.count)
            ps.selected = (int)ps.patch.count - 1;
        ps.editor_for = -1;
        changed(project, state);
    }
    render_editor(project, document_ptr, state);
}

void patch_redirect_slot(ApexProject *project, UiState *state, uint32_t slot_addr,
                         uint8_t target_bank, uint32_t target_addr, bool target_is_stub,
                         const char *slot_name) {
    PatchUiState &ps = state->patch_state;
    if (!ps.loaded) {
        std::string def = default_patch_path(project);
        FILE *f = fopen(def.c_str(), "r");
        if (f) {
            fclose(f);
            patch_open_path(project, state, def.c_str());
        } else {
            patch_new(project, state, def.c_str());
        }
    }
    char base[64], label[96], tgt[160], code[512], at[32];
    snprintf(base, sizeof(base), "hook_%04x", (unsigned)slot_addr);
    if (slot_name && *slot_name)
        snprintf(base, sizeof(base), "%s", slot_name);
    long ci = apex_patch_add_op(&ps.patch, APEX_PATCH_ASM, base, nullptr);
    if (ci < 0)
        return;
    /* the label is the code op's (unique) name, made a valid identifier */
    snprintf(label, sizeof(label), "My_%s", ps.patch.ops[ci].name);
    for (char *q = label; *q; q++) {
        if (!isalnum((unsigned char)*q) && *q != '_')
            *q = '_';
    }
    apex_patch_format_at(project, target_bank, target_addr, tgt, sizeof(tgt));
    /* The OS far-calls the slot with the target's bank mapped: the new code
       lives in that bank (or the system bank for a system target) and can
       continue at the original target with a plain JMP. */
    if (target_is_stub)
        snprintf(code, sizeof(code), "%s:\n    ; your code (the slot was unused)\n    RTS", label);
    else
        snprintf(code, sizeof(code), "%s:\n    ; your code\n    JMP %s    ; the original hook",
                 label, tgt);
    ApexPatchOp *o = &ps.patch.ops[ci];
    apex_patch_set_field(&o->code, code);
    char alloc[16];
    if (target_bank == 0xffu)
        snprintf(alloc, sizeof(alloc), "system");
    else
        snprintf(alloc, sizeof(alloc), "bank:%02x", target_bank);
    apex_patch_set_field(&o->alloc, alloc);
    snprintf(at, sizeof(at), "0x%04x", (unsigned)slot_addr);
    long pi = apex_patch_add_op(&ps.patch, APEX_PATCH_PTR, "slot", at);
    if (pi >= 0) {
        apex_patch_set_field(&ps.patch.ops[pi].target, label);
        ps.patch.ops[pi].width = 3;
    }
    ps.selected = (int)ci;
    ps.editor_for = -1;
    state->show_patch = true;
    patch_rebuild(project, state);
    /* guard the slot with its original pointer */
    if (pi >= 0 && (size_t)pi < ps.build.count && ps.build.ops[pi].orig &&
        ps.build.ops[pi].length && !ps.patch.ops[pi].expect) {
        apex_patch_set_field(&ps.patch.ops[pi].expect,
                             hex_bytes(ps.build.ops[pi].orig, ps.build.ops[pi].length).c_str());
        patch_rebuild(project, state);
    }
    ps.dirty = true;
    set_status(state, "hook slot redirected -- write your code in the Patch window");
}
