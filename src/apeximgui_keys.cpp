/* User-configurable keyboard shortcuts.
 *
 * Every global hotkey is an action with up to two key chords (key + exact
 * modifiers).  The defaults are the historic hard-wired keys; changes are kept
 * in imgui.ini ([ApexII][Keys], only the bindings that differ from the
 * default) and edited in the Keyboard Shortcuts window.
 */
#include "ImGuiFileDialog.h"
#include "apeximgui_core.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct KeyActionDef {
    const char *id;       /* stable name in imgui.ini */
    const char *category; /* section in the editor */
    const char *desc;
    ImGuiKeyChord defaults[KEY_BINDINGS_PER_ACTION];
    bool repeat; /* fires again while held (navigation) */
};

constexpr ImGuiKeyChord C = ImGuiMod_Ctrl;
constexpr ImGuiKeyChord S = ImGuiMod_Shift;
constexpr ImGuiKeyChord A = ImGuiMod_Alt;

/* Order = KeyAction enum order. */
const KeyActionDef k_actions[KEY_ACTION_COUNT] = {
    {"MoveDown", "Navigate", "Move selection down", {ImGuiKey_J, ImGuiKey_DownArrow}, true},
    {"MoveUp", "Navigate", "Move selection up", {ImGuiKey_K, ImGuiKey_UpArrow}, true},
    {"PageDown", "Navigate", "Page down", {ImGuiKey_PageDown, 0}, true},
    {"PageUp", "Navigate", "Page up", {ImGuiKey_PageUp, 0}, true},
    {"NextBlock", "Navigate", "Next block boundary", {ImGuiKey_N, 0}, true},
    {"PrevBlock", "Navigate", "Previous block boundary", {ImGuiKey_P, 0}, true},
    {"Follow", "Navigate", "Follow link / jump to target", {ImGuiKey_F, ImGuiKey_Enter}, false},
    {"HistoryBack",
     "Navigate",
     "History back",
     {ImGuiKey_LeftBracket, A | ImGuiKey_LeftArrow},
     false},
    {"HistoryForward",
     "Navigate",
     "History forward",
     {ImGuiKey_RightBracket, A | ImGuiKey_RightArrow},
     false},
    {"GoTo", "Navigate", "Go to address", {ImGuiKey_G, 0}, false},
    {"CommandPalette",
     "Navigate",
     "Command palette (jump to label / address)",
     {C | ImGuiKey_P, 0},
     false},
    {"FocusFilter", "Navigate", "Focus filter bar", {ImGuiKey_Slash, 0}, false},
    {"TableHeader", "Navigate", "Jump to table header (inside a table)", {ImGuiKey_Home, 0}, false},
    {"MarkCode", "Classify", "Mark as code", {ImGuiKey_C, 0}, false},
    {"MarkData", "Classify", "Mark as data", {ImGuiKey_D, 0}, false},
    {"MarkString", "Classify", "Mark as string", {ImGuiKey_S, 0}, false},
    {"MarkTable", "Classify", "Mark as table (uses current schema)", {ImGuiKey_T, 0}, false},
    {"RepeatClassify",
     "Classify",
     "Repeat last classification",
     {ImGuiKey_1, ImGuiKey_Keypad1},
     false},
    {"ClearKind", "Classify", "Clear classification", {ImGuiKey_Delete, 0}, false},
    {"EditLabel", "Edit", "Edit label", {ImGuiKey_L, 0}, false},
    {"EditDoc", "Edit", "Edit comment / doc", {S | ImGuiKey_D, 0}, false},
    {"Bookmark", "Edit", "Add bookmark", {ImGuiKey_B, 0}, false},
    {"Undo", "Edit", "Undo last edit", {C | ImGuiKey_Z, 0}, false},
    {"Redo", "Edit", "Redo", {C | ImGuiKey_Y, C | S | ImGuiKey_Z}, false},
    {"Save", "Edit", "Save config overlay", {C | ImGuiKey_S, 0}, false},
    {"BlockStart", "Block selection", "Set block start (at cursor)", {ImGuiKey_A, 0}, false},
    {"BlockEnd", "Block selection", "Set block end (at cursor)", {ImGuiKey_E, 0}, false},
    {"BlockClear", "Block selection", "Clear block", {ImGuiKey_Escape, 0}, false},
    {"Copy",
     "Block selection",
     "Copy block / selection (hex bytes in hex view)",
     {C | ImGuiKey_C, 0},
     false},
    {"XRefs", "View", "Show incoming XRefs", {ImGuiKey_X, 0}, false},
    {"Help", "View", "Show / hide the shortcuts window", {ImGuiKey_H, 0}, false},
    {"GlobalSearch", "View", "Global search (hex search in hex view)", {C | ImGuiKey_F, 0}, false},
    {"Reanalyze", "View", "Re-analyze", {ImGuiKey_F5, 0}, false},
    {"FullReanalyze", "View", "Force full re-analyze", {S | ImGuiKey_F5, 0}, false},
    {"ZoomIn", "View", "Font zoom in", {C | ImGuiKey_Equal, C | ImGuiKey_KeypadAdd}, true},
    {"ZoomOut", "View", "Font zoom out", {C | ImGuiKey_Minus, C | ImGuiKey_KeypadSubtract}, true},
    {"ZoomReset", "View", "Reset font zoom", {C | ImGuiKey_0, C | ImGuiKey_Keypad0}, false},
    {"DmdNext", "DMD scrub", "Advance 1 byte", {ImGuiKey_Equal, ImGuiKey_KeypadAdd}, true},
    {"DmdPrev", "DMD scrub", "Rewind 1 byte", {ImGuiKey_Minus, ImGuiKey_KeypadSubtract}, true},
    {"DmdNext32",
     "DMD scrub",
     "Advance 32 bytes",
     {S | ImGuiKey_Equal, S | ImGuiKey_KeypadAdd},
     true},
    {"DmdPrev32",
     "DMD scrub",
     "Rewind 32 bytes",
     {S | ImGuiKey_Minus, S | ImGuiKey_KeypadSubtract},
     true},
    {"DmdReset", "DMD scrub", "Reset offset", {ImGuiKey_0, ImGuiKey_Keypad0}, false},
    {"DmdMark", "DMD scrub", "Mark DMD fullframe at current offset", {ImGuiKey_M, 0}, false},
    {"MoveLeft", "Navigate", "Hex view: cursor one byte left", {ImGuiKey_LeftArrow, 0}, true},
    {"MoveRight", "Navigate", "Hex view: cursor one byte right", {ImGuiKey_RightArrow, 0}, true},
};

ImGuiKeyChord g_bindings[KEY_ACTION_COUNT][KEY_BINDINGS_PER_ACTION];
bool g_bindings_init = false;

/* Editor capture state: the binding slot waiting for a key press. */
int g_capture_action = -1;
int g_capture_slot = 0;
/* Frame in which a capture completed: the captured key press must not also
   trigger an action that same frame (the editor renders before the dispatch). */
int g_capture_done_frame = -1;

void init_bindings() {
    if (g_bindings_init)
        return;
    for (int a = 0; a < KEY_ACTION_COUNT; a++)
        for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++)
            g_bindings[a][s] = k_actions[a].defaults[s];
    g_bindings_init = true;
}

bool is_modifier_key(ImGuiKey k) {
    return ImGui::IsLRModKey(k) || k == ImGuiKey_ReservedForModCtrl ||
           k == ImGuiKey_ReservedForModShift || k == ImGuiKey_ReservedForModAlt ||
           k == ImGuiKey_ReservedForModSuper;
}

/* "Ctrl+Shift+Z" -> chord; 0 if unknown.  Names are ImGui's key names. */
ImGuiKeyChord parse_chord(const char *s) {
    ImGuiKeyChord mods = 0;
    std::string rest(s);
    for (;;) {
        size_t plus = rest.find('+');
        /* A trailing "+" belongs to a key name only for keys named "+", which
           ImGui does not have, so every '+' separates. */
        if (plus == std::string::npos)
            break;
        std::string m = rest.substr(0, plus);
        if (m == "Ctrl")
            mods |= ImGuiMod_Ctrl;
        else if (m == "Shift")
            mods |= ImGuiMod_Shift;
        else if (m == "Alt")
            mods |= ImGuiMod_Alt;
        else if (m == "Super")
            mods |= ImGuiMod_Super;
        else
            return 0;
        rest.erase(0, plus + 1);
    }
    for (int k = ImGuiKey_Keyboard_BEGIN; k < ImGuiKey_Keyboard_END; k++) {
        if (rest == ImGui::GetKeyName((ImGuiKey)k))
            return mods | k;
    }
    return 0;
}

}  // namespace

const char *key_action_id(KeyAction a) {
    return k_actions[a].id;
}

const char *key_action_desc(KeyAction a) {
    return k_actions[a].desc;
}

ImGuiKeyChord key_binding(KeyAction a, int slot) {
    init_bindings();
    return g_bindings[a][slot];
}

void key_set_binding(KeyAction a, int slot, ImGuiKeyChord chord) {
    init_bindings();
    g_bindings[a][slot] = chord;
    ImGui::MarkIniSettingsDirty();
}

void key_reset_action(KeyAction a) {
    init_bindings();
    for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++)
        g_bindings[a][s] = k_actions[a].defaults[s];
    ImGui::MarkIniSettingsDirty();
}

bool key_capturing() {
    return g_capture_action >= 0;
}

/* Exact match: the chord's key went down this frame (or repeats, for
   navigation actions) and the held modifiers are exactly the chord's.  Escape
   is read from the raw key state because keyboard navigation claims its
   ownership while a child window (the hex view) is focused. */
bool key_action_pressed(KeyAction a) {
    init_bindings();
    if (g_capture_action >= 0 || g_capture_done_frame == ImGui::GetFrameCount())
        return false;
    const ImGuiIO &io = ImGui::GetIO();
    for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++) {
        ImGuiKeyChord ch = g_bindings[a][s];
        if (!ch)
            continue;
        ImGuiKey key = (ImGuiKey)(ch & ~ImGuiMod_Mask_);
        if ((ImGuiKeyChord)(io.KeyMods & ImGuiMod_Mask_) != (ch & ImGuiMod_Mask_))
            continue;
        if (key == ImGuiKey_Escape) {
            const ImGuiKeyData *kd = ImGui::GetKeyData(key);
            if (kd && kd->Down && kd->DownDuration == 0.0f)
                return true;
        } else if (ImGui::IsKeyPressed(key, k_actions[a].repeat)) {
            return true;
        }
    }
    return false;
}

const char *key_hint(KeyAction a) {
    static std::string hints[KEY_ACTION_COUNT];
    init_bindings();
    std::string h;
    for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++) {
        if (!g_bindings[a][s])
            continue;
        if (!h.empty())
            h += " / ";
        h += ImGui::GetKeyChordName(g_bindings[a][s]);
    }
    hints[a] = h;
    return hints[a].c_str();
}

void keys_read_ini_line(const char *line) {
    init_bindings();
    const char *eq = strchr(line, '=');
    if (!eq)
        return;
    std::string id(line, (size_t)(eq - line));
    for (int a = 0; a < KEY_ACTION_COUNT; a++) {
        if (id != k_actions[a].id)
            continue;
        /* "chord,chord"; an empty value unbinds the action. */
        std::string v(eq + 1);
        int slot = 0;
        for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++)
            g_bindings[a][s] = 0;
        size_t start = 0;
        while (start < v.size() && slot < KEY_BINDINGS_PER_ACTION) {
            size_t comma = v.find(',', start);
            std::string tok =
                v.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            ImGuiKeyChord ch = parse_chord(tok.c_str());
            if (ch)
                g_bindings[a][slot++] = ch;
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return;
    }
}

void keys_write_ini(ImGuiTextBuffer *buf) {
    init_bindings();
    std::string lines;
    for (int a = 0; a < KEY_ACTION_COUNT; a++) {
        bool is_default = true;
        for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++)
            is_default = is_default && g_bindings[a][s] == k_actions[a].defaults[s];
        if (is_default)
            continue;
        lines += k_actions[a].id;
        lines += "=";
        bool first = true;
        for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++) {
            if (!g_bindings[a][s])
                continue;
            if (!first)
                lines += ",";
            lines += ImGui::GetKeyChordName(g_bindings[a][s]);
            first = false;
        }
        lines += "\n";
    }
    if (lines.empty())
        return;
    buf->appendf("[ApexII][Keys]\n%s\n", lines.c_str());
}

/* Other actions bound to the same chord (a conflict), as "Desc, Desc". */
static std::string key_conflicts(int action, ImGuiKeyChord chord) {
    std::string out;
    if (!chord)
        return out;
    for (int a = 0; a < KEY_ACTION_COUNT; a++) {
        if (a == action)
            continue;
        for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++) {
            if (g_bindings[a][s] == chord) {
                if (!out.empty())
                    out += ", ";
                out += k_actions[a].desc;
            }
        }
    }
    return out;
}

void render_keys_editor() {
    init_bindings();
    const ImGuiIO &io = ImGui::GetIO();

    /* Capture: the first non-modifier key pressed, with the held modifiers. */
    if (g_capture_action >= 0) {
        for (int k = ImGuiKey_Keyboard_BEGIN; k < ImGuiKey_Keyboard_END; k++) {
            if (is_modifier_key((ImGuiKey)k) || !ImGui::IsKeyPressed((ImGuiKey)k, false))
                continue;
            ImGuiKeyChord ch = (ImGuiKeyChord)(io.KeyMods & ImGuiMod_Mask_) | k;
            key_set_binding((KeyAction)g_capture_action, g_capture_slot, ch);
            g_capture_action = -1;
            g_capture_done_frame = ImGui::GetFrameCount();
            break;
        }
    }

    ImGui::TextDisabled("Click a binding and press the new key (with modifiers). "
                        "x clears a binding; changes are saved in imgui.ini.");
    bool any_custom = false;
    for (int a = 0; a < KEY_ACTION_COUNT && !any_custom; a++)
        for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++)
            any_custom = any_custom || g_bindings[a][s] != k_actions[a].defaults[s];
    ImGui::BeginDisabled(!any_custom);
    if (ImGui::Button("Reset all to defaults")) {
        for (int a = 0; a < KEY_ACTION_COUNT; a++)
            key_reset_action((KeyAction)a);
        g_capture_action = -1;
    }
    ImGui::EndDisabled();
    if (g_capture_action >= 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.15f, 1.0f), "press a key for \"%s\"...",
                           k_actions[g_capture_action].desc);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            g_capture_action = -1;
    }

    const char *cat = nullptr;
    bool table_open = false;
    for (int a = 0; a < KEY_ACTION_COUNT; a++) {
        if (!cat || strcmp(cat, k_actions[a].category) != 0) {
            if (table_open)
                ImGui::EndTable();
            cat = k_actions[a].category;
            ImGui::SeparatorText(cat);
            table_open = ImGui::BeginTable(
                cat, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX);
            if (table_open) {
                ImGui::TableSetupColumn("##d", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##k1", ImGuiTableColumnFlags_WidthFixed, ui_w(150.0f));
                ImGui::TableSetupColumn("##k2", ImGuiTableColumnFlags_WidthFixed, ui_w(150.0f));
                ImGui::TableSetupColumn("##r", ImGuiTableColumnFlags_WidthFixed, ui_w(50.0f));
            }
        }
        if (!table_open)
            continue;
        ImGui::PushID(a);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(k_actions[a].desc);
        for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++) {
            ImGui::TableSetColumnIndex(1 + s);
            ImGui::PushID(s);
            ImGuiKeyChord ch = g_bindings[a][s];
            bool capturing = g_capture_action == a && g_capture_slot == s;
            std::string label = capturing ? "..." : ch ? ImGui::GetKeyChordName(ch) : "-";
            std::string conflict = key_conflicts(a, ch);
            if (!conflict.empty())
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
            if (ImGui::Button((label + "##b").c_str(), ImVec2(118.0f, 0))) {
                g_capture_action = a;
                g_capture_slot = s;
            }
            if (!conflict.empty()) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("also bound to: %s", conflict.c_str());
            }
            if (ch) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    key_set_binding((KeyAction)a, s, 0);
            }
            ImGui::PopID();
        }
        ImGui::TableSetColumnIndex(3);
        bool is_default = true;
        for (int s = 0; s < KEY_BINDINGS_PER_ACTION; s++)
            is_default = is_default && g_bindings[a][s] == k_actions[a].defaults[s];
        if (!is_default && ImGui::SmallButton("reset"))
            key_reset_action((KeyAction)a);
        ImGui::PopID();
    }
    if (table_open)
        ImGui::EndTable();

    ImGui::SeparatorText("Fixed");
    ImGui::BulletText("Shift+Click: set / extend block;  double-click: rename a label in place");
    ImGui::BulletText("Hex view: arrow keys / PageUp / PageDown move the byte cursor");
    ImGui::BulletText("Alt+F4: exit");
}

void center_file_dialog(const char *key) {
    if (ImGuiFileDialog::Instance()->IsOpened(key))
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
}
