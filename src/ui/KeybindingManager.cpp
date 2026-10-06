// ============================================================================
//  KeybindingManager.cpp
// ============================================================================
#include "ui/KeybindingManager.hpp"
#include "ui/Localization.hpp"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace ide {

namespace {

// ---------------------------------------------------------------------------
//  Таблица имён клавиш (двусторонняя). Имена совпадают с нотацией VS Code,
//  чтобы JSON-файлы раскладок было удобно писать руками.
// ---------------------------------------------------------------------------
struct KeyName { ImGuiKey key; const char* name; };
const KeyName kKeyNames[] = {
    {ImGuiKey_Tab, "Tab"}, {ImGuiKey_LeftArrow, "Left"}, {ImGuiKey_RightArrow, "Right"},
    {ImGuiKey_UpArrow, "Up"}, {ImGuiKey_DownArrow, "Down"}, {ImGuiKey_PageUp, "PageUp"},
    {ImGuiKey_PageDown, "PageDown"}, {ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"},
    {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Delete, "Delete"}, {ImGuiKey_Backspace, "Backspace"},
    {ImGuiKey_Space, "Space"}, {ImGuiKey_Enter, "Enter"}, {ImGuiKey_Escape, "Escape"},
    {ImGuiKey_Apostrophe, "'"}, {ImGuiKey_Comma, ","}, {ImGuiKey_Minus, "-"}, {ImGuiKey_Period, "."},
    {ImGuiKey_Slash, "/"}, {ImGuiKey_Semicolon, ";"}, {ImGuiKey_Equal, "="}, {ImGuiKey_LeftBracket, "["},
    {ImGuiKey_Backslash, "\\"}, {ImGuiKey_RightBracket, "]"}, {ImGuiKey_GraveAccent, "`"},
    {ImGuiKey_Pause, "Pause"}, {ImGuiKey_KeypadAdd, "NumPad+"}, {ImGuiKey_KeypadSubtract, "NumPad-"},
    {ImGuiKey_F1, "F1"}, {ImGuiKey_F2, "F2"}, {ImGuiKey_F3, "F3"}, {ImGuiKey_F4, "F4"},
    {ImGuiKey_F5, "F5"}, {ImGuiKey_F6, "F6"}, {ImGuiKey_F7, "F7"}, {ImGuiKey_F8, "F8"},
    {ImGuiKey_F9, "F9"}, {ImGuiKey_F10, "F10"}, {ImGuiKey_F11, "F11"}, {ImGuiKey_F12, "F12"},
};

std::optional<ImGuiKey> keyFromName(std::string n) {
    if (n.size() == 1 && std::isalpha((unsigned char)n[0]))
        return ImGuiKey(ImGuiKey_A + (std::toupper((unsigned char)n[0]) - 'A'));
    if (n.size() == 1 && std::isdigit((unsigned char)n[0])) return ImGuiKey(ImGuiKey_0 + (n[0] - '0'));
    for (auto& k : kKeyNames) {
        std::string a = k.name, b = n;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        if (a == b) return k.key;
    }
    if (n == "Break") return ImGuiKey_Pause;
    return std::nullopt;
}

std::string nameFromKey(ImGuiKey k) {
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return std::string(1, char('A' + (k - ImGuiKey_A)));
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return std::string(1, char('0' + (k - ImGuiKey_0)));
    for (auto& e : kKeyNames) if (e.key == k) return e.name;
    return ImGui::GetKeyName(k);
}

bool isModifierKey(ImGuiKey k) {
    return k == ImGuiKey_LeftCtrl || k == ImGuiKey_RightCtrl || k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift ||
           k == ImGuiKey_LeftAlt || k == ImGuiKey_RightAlt || k == ImGuiKey_LeftSuper || k == ImGuiKey_RightSuper ||
           k == ImGuiKey_ReservedForModCtrl || k == ImGuiKey_ReservedForModShift ||
           k == ImGuiKey_ReservedForModAlt || k == ImGuiKey_ReservedForModSuper;
}

// ---------------------------------------------------------------------------
//  Встроенные пресеты. Формат строки: "команда", "контекст", "сочетание".
//  Несколько сочетаний одной команды — несколько строк.
// ---------------------------------------------------------------------------
struct Def { const char* cmd; const char* when; const char* keys; };

const Def kVsCode[] = {
    {"file.new", "global", "Ctrl+N"}, {"file.open", "global", "Ctrl+O"},
    {"file.openFolder", "global", "Ctrl+K Ctrl+O"}, {"file.save", "editor", "Ctrl+S"},
    {"file.saveAll", "global", "Ctrl+K S"}, {"file.close", "editor", "Ctrl+W"},
    {"project.new", "global", "Ctrl+Alt+N"}, {"app.quit", "global", "Ctrl+Q"},
    {"view.commandPalette", "global", "Ctrl+Shift+P"}, {"view.commandPalette", "global", "F1"},
    {"view.explorer", "global", "Ctrl+Shift+E"}, {"view.git", "global", "Ctrl+Shift+G"},
    {"view.terminal", "global", "Ctrl+`"}, {"view.problems", "global", "Ctrl+Shift+M"},
    {"view.output", "global", "Ctrl+Shift+U"}, {"view.telemetry", "global", "Ctrl+Alt+T"},
    {"view.settings", "global", "Ctrl+,"}, {"view.keybindings", "global", "Ctrl+K Ctrl+S"},
    {"view.debug", "global", "Ctrl+Shift+D"}, {"view.extensions", "global", "Ctrl+Shift+X"},
    {"theme.next", "global", "Ctrl+K Ctrl+T"}, {"language.toggle", "global", "Ctrl+Alt+Shift+L"},
    {"build.build", "global", "Ctrl+Shift+B"}, {"build.run", "global", "Ctrl+F5"},
    {"build.cancel", "global", "Ctrl+Pause"},
    {"debug.start", "global", "F5"}, {"debug.stop", "global", "Shift+F5"},
    {"debug.restart", "global", "Ctrl+Shift+F5"}, {"debug.pause", "global", "F6"},
    {"debug.stepOver", "global", "F10"}, {"debug.stepInto", "global", "F11"},
    {"debug.stepOut", "global", "Shift+F11"}, {"editor.toggleBreakpoint", "editor", "F9"},
    {"editor.undo", "editor", "Ctrl+Z"}, {"editor.redo", "editor", "Ctrl+Y"},
    {"editor.redo", "editor", "Ctrl+Shift+Z"}, {"editor.copy", "editor", "Ctrl+C"},
    {"editor.cut", "editor", "Ctrl+X"}, {"editor.paste", "editor", "Ctrl+V"},
    {"editor.selectAll", "editor", "Ctrl+A"}, {"editor.find", "editor", "Ctrl+F"},
    {"editor.addNextOccurrence", "editor", "Ctrl+D"}, {"editor.addCursorAbove", "editor", "Ctrl+Alt+Up"},
    {"editor.addCursorBelow", "editor", "Ctrl+Alt+Down"}, {"editor.toggleComment", "editor", "Ctrl+/"},
    {"editor.duplicateLine", "editor", "Shift+Alt+Down"}, {"editor.toggleFold", "editor", "Ctrl+Shift+["},
    {"editor.foldAll", "editor", "Ctrl+K Ctrl+0"}, {"editor.unfoldAll", "editor", "Ctrl+K Ctrl+J"},
    {"editor.triggerCompletion", "editor", "Ctrl+Space"}, {"editor.goToDefinition", "editor", "F12"},
    {"editor.zoomIn", "global", "Ctrl+="}, {"editor.zoomOut", "global", "Ctrl+-"},
};

const Def kVisualStudio[] = {
    {"file.new", "global", "Ctrl+N"}, {"file.open", "global", "Ctrl+O"},
    {"file.openFolder", "global", "Ctrl+Shift+Alt+O"}, {"file.save", "editor", "Ctrl+S"},
    {"file.saveAll", "global", "Ctrl+Shift+S"}, {"file.close", "editor", "Ctrl+F4"},
    {"project.new", "global", "Ctrl+Shift+N"}, {"app.quit", "global", "Alt+F4"},
    {"view.commandPalette", "global", "Ctrl+Q"}, {"view.explorer", "global", "Ctrl+Alt+L"},
    {"view.git", "global", "Ctrl+0 Ctrl+G"}, {"view.terminal", "global", "Ctrl+`"},
    {"view.problems", "global", "Ctrl+\\ E"}, {"view.output", "global", "Ctrl+Alt+O"},
    {"view.telemetry", "global", "Ctrl+Alt+F2"}, {"view.settings", "global", "Ctrl+,"},
    {"view.keybindings", "global", "Ctrl+K Ctrl+S"}, {"view.debug", "global", "Ctrl+Alt+D"},
    {"view.extensions", "global", "Ctrl+Shift+X"},
    {"theme.next", "global", "Ctrl+K Ctrl+T"}, {"language.toggle", "global", "Ctrl+Alt+Shift+L"},
    {"build.build", "global", "Ctrl+Shift+B"}, {"build.run", "global", "Ctrl+F5"},
    {"build.cancel", "global", "Ctrl+Pause"},
    {"debug.start", "global", "F5"}, {"debug.stop", "global", "Shift+F5"},
    {"debug.restart", "global", "Ctrl+Shift+F5"}, {"debug.pause", "global", "Ctrl+Alt+Pause"},
    {"debug.stepOver", "global", "F10"}, {"debug.stepInto", "global", "F11"},
    {"debug.stepOut", "global", "Shift+F11"}, {"editor.toggleBreakpoint", "editor", "F9"},
    {"editor.undo", "editor", "Ctrl+Z"}, {"editor.redo", "editor", "Ctrl+Y"},
    {"editor.copy", "editor", "Ctrl+C"}, {"editor.cut", "editor", "Ctrl+X"},
    {"editor.paste", "editor", "Ctrl+V"}, {"editor.selectAll", "editor", "Ctrl+A"},
    {"editor.find", "editor", "Ctrl+F"}, {"editor.addNextOccurrence", "editor", "Shift+Alt+."},
    {"editor.addCursorAbove", "editor", "Shift+Alt+Up"}, {"editor.addCursorBelow", "editor", "Shift+Alt+Down"},
    {"editor.toggleComment", "editor", "Ctrl+K Ctrl+C"}, {"editor.toggleComment", "editor", "Ctrl+K Ctrl+U"},
    {"editor.duplicateLine", "editor", "Ctrl+D"}, {"editor.toggleFold", "editor", "Ctrl+M Ctrl+M"},
    {"editor.foldAll", "editor", "Ctrl+M Ctrl+O"}, {"editor.unfoldAll", "editor", "Ctrl+M Ctrl+L"},
    {"editor.triggerCompletion", "editor", "Ctrl+J"}, {"editor.triggerCompletion", "editor", "Ctrl+Space"},
    {"editor.goToDefinition", "editor", "F12"},
    {"editor.zoomIn", "global", "Ctrl+Shift+."}, {"editor.zoomOut", "global", "Ctrl+Shift+,"},
};

template <std::size_t N>
nlohmann::json toJson(const char* name, const Def (&defs)[N]) {
    nlohmann::json j;
    j["name"] = name;
    j["bindings"] = nlohmann::json::array();
    for (auto& d : defs) j["bindings"].push_back({{"command", d.cmd}, {"when", d.when}, {"key", d.keys}});
    return j;
}

// Простое нечёткое сопоставление для палитры: все символы фильтра по порядку.
int fuzzyScore(const std::string& text, const std::string& pattern) {
    if (pattern.empty()) return 1;
    std::size_t ti = 0; int score = 0, streak = 0;
    for (char pc : pattern) {
        bool found = false;
        while (ti < text.size()) {
            char tc = text[ti++];
            if (std::tolower((unsigned char)tc) == std::tolower((unsigned char)pc)) {
                found = true; score += 1 + streak * 2; ++streak; break;
            }
            streak = 0;
        }
        if (!found) return 0;
    }
    return score;
}

} // namespace

// ===========================================================================
KeybindingManager::KeybindingManager() { loadPreset("vscode", {}); }

void KeybindingManager::registerCommand(const std::string& id, std::function<void()> handler, const std::string& when) {
    commands_[id] = Command{id, when, std::move(handler)};
}

bool KeybindingManager::execute(const std::string& id) {
    auto it = commands_.find(id);
    if (it == commands_.end() || !it->second.handler) return false;
    it->second.handler();
    return true;
}

nlohmann::json KeybindingManager::builtinPresetJson(const std::string& name) {
    return name == "visualstudio" ? toJson("Visual Studio", kVisualStudio) : toJson("VS Code", kVsCode);
}

bool KeybindingManager::loadPreset(const std::string& name, const std::filesystem::path& resourceDir) {
    nlohmann::json j;
    bool fromFile = false;
    if (!resourceDir.empty()) {
        std::ifstream f(resourceDir / "keymaps" / (name + ".json"));
        if (f) {
            try { j = nlohmann::json::parse(f); fromFile = true; } catch (...) { fromFile = false; }
        }
    }
    if (!fromFile) {
        if (name != "vscode" && name != "visualstudio") return false;
        j = builtinPresetJson(name);
    }
    presetBindings_.clear();
    for (auto& b : j.value("bindings", nlohmann::json::array())) {
        auto seq = parseSequence(b.value("key", ""));
        if (!seq) continue;
        std::string cmd = b.value("command", "");
        presetBindings_[cmd].push_back(*seq);
        // Контекст из раскладки применяем к уже зарегистрированной команде,
        // только если команда не задала его сама.
    }
    preset_ = name;
    pending_.reset();
    return true;
}

std::vector<KeySequence> KeybindingManager::bindingsFor(const std::string& cmd) const {
    if (auto it = overrides_.find(cmd); it != overrides_.end()) return it->second;
    if (auto it = presetBindings_.find(cmd); it != presetBindings_.end()) return it->second;
    return {};
}

void KeybindingManager::setBinding(const std::string& cmd, std::vector<KeySequence> seqs) { overrides_[cmd] = std::move(seqs); }
void KeybindingManager::resetBinding(const std::string& cmd) { overrides_.erase(cmd); }

std::string KeybindingManager::shortcutText(const std::string& cmd) const {
    auto b = bindingsFor(cmd);
    return b.empty() ? std::string{} : sequenceToString(b.front());
}

std::vector<KeybindingManager::Effective> KeybindingManager::effective() const {
    std::vector<Effective> out;
    std::set<std::string> seen;
    for (auto& [cmd, seqs] : overrides_) { seen.insert(cmd); for (auto& s : seqs) out.push_back({cmd, s}); }
    for (auto& [cmd, seqs] : presetBindings_)
        if (!seen.count(cmd)) for (auto& s : seqs) out.push_back({cmd, s});
    return out;
}

std::vector<std::string> KeybindingManager::conflicts(const std::string& cmd, const KeySequence& seq) const {
    std::vector<std::string> out;
    for (auto& e : effective())
        if (e.cmd != cmd && e.seq == seq) out.push_back(e.cmd);
    return out;
}

bool KeybindingManager::loadUserOverrides(const std::filesystem::path& file) {
    std::ifstream f(file);
    if (!f) return false;
    try {
        auto j = nlohmann::json::parse(f);
        overrides_.clear();
        for (auto& [cmd, arr] : j.value("overrides", nlohmann::json::object()).items()) {
            std::vector<KeySequence> seqs;
            for (auto& s : arr) if (auto q = parseSequence(s.get<std::string>())) seqs.push_back(*q);
            overrides_[cmd] = std::move(seqs);     // пустой список = команда «отвязана»
        }
        return true;
    } catch (...) { return false; }
}

bool KeybindingManager::saveUserOverrides(const std::filesystem::path& file) const {
    nlohmann::json j;
    j["preset"] = preset_;
    j["overrides"] = nlohmann::json::object();
    for (auto& [cmd, seqs] : overrides_) {
        auto arr = nlohmann::json::array();
        for (auto& s : seqs) arr.push_back(sequenceToString(s));
        j["overrides"][cmd] = arr;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file);
    if (!f) return false;
    f << j.dump(2);
    return bool(f);
}

// ---------------------------------------------------------------------------
//  Строковое представление
// ---------------------------------------------------------------------------
std::string KeybindingManager::chordToString(ImGuiKeyChord c) {
    std::string s;
    if (c & ImGuiMod_Ctrl)  s += "Ctrl+";
    if (c & ImGuiMod_Shift) s += "Shift+";
    if (c & ImGuiMod_Alt)   s += "Alt+";
    if (c & ImGuiMod_Super) s += "Super+";
    s += nameFromKey(ImGuiKey(c & ~ImGuiMod_Mask_));
    return s;
}

std::string KeybindingManager::sequenceToString(const KeySequence& seq) {
    std::string s;
    for (std::size_t i = 0; i < seq.size(); ++i) { if (i) s += ' '; s += chordToString(seq[i]); }
    return s;
}

std::optional<ImGuiKeyChord> KeybindingManager::parseChord(const std::string& str) {
    if (str.empty()) return std::nullopt;
    ImGuiKeyChord mods = 0;
    std::optional<ImGuiKey> key;
    // Разбиваем по '+', но последний символ может сам быть '+' ("Ctrl++").
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '+' && !cur.empty()) { parts.push_back(cur); cur.clear(); }
        else cur += str[i];
    }
    if (!cur.empty()) parts.push_back(cur);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::string p = parts[i], low = p;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (i + 1 < parts.size()) {
            if (low == "ctrl" || low == "control" || low == "cmd") mods |= ImGuiMod_Ctrl;
            else if (low == "shift") mods |= ImGuiMod_Shift;
            else if (low == "alt" || low == "option") mods |= ImGuiMod_Alt;
            else if (low == "super" || low == "win" || low == "meta") mods |= ImGuiMod_Super;
            else return std::nullopt;
        } else {
            key = keyFromName(p);
        }
    }
    if (!key) return std::nullopt;
    return ImGuiKeyChord(*key) | mods;
}

std::optional<KeySequence> KeybindingManager::parseSequence(const std::string& s) {
    std::istringstream ss(s);
    std::string tok;
    KeySequence seq;
    while (ss >> tok) {
        auto c = parseChord(tok);
        if (!c) return std::nullopt;
        seq.push_back(*c);
    }
    if (seq.empty() || seq.size() > 2) return std::nullopt;
    return seq;
}

// ---------------------------------------------------------------------------
//  Обработка ввода
// ---------------------------------------------------------------------------
std::optional<ImGuiKeyChord> KeybindingManager::pollChord() {
    ImGuiIO& io = ImGui::GetIO();
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        auto key = ImGuiKey(k);
        if (isModifierKey(key) || (key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadR3) ||
            (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY))
            continue;
        if (ImGui::IsKeyPressed(key, false)) {
            ImGuiKeyChord mods = 0;
            if (io.KeyCtrl) mods |= ImGuiMod_Ctrl;
            if (io.KeyShift) mods |= ImGuiMod_Shift;
            if (io.KeyAlt) mods |= ImGuiMod_Alt;
            if (io.KeySuper) mods |= ImGuiMod_Super;
            return ImGuiKeyChord(key) | mods;
        }
    }
    return std::nullopt;
}

bool KeybindingManager::contextOk(const Command& c) const {
    if (c.when == "global") return true;
    return contextActive ? contextActive(c.when) : false;
}

void KeybindingManager::process() {
    if (!capturingCmd_.empty()) return;                 // идёт запись нового сочетания
    const double now = ImGui::GetTime();
    if (pending_ && now - pendingSince_ > 2.0) pending_.reset();   // таймаут второго шага

    auto chord = pollChord();
    if (!chord) return;
    const ImGuiKeyChord c = *chord;
    // Голые печатные клавиши без модификаторов не перехватываем (кроме F-клавиш,
    // Pause и второго шага сочетания) — иначе сломается обычный ввод текста.
    const bool hasMods = (c & ImGuiMod_Mask_) != 0;
    const ImGuiKey key = ImGuiKey(c & ~ImGuiMod_Mask_);
    const bool functionKey = (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) || key == ImGuiKey_Pause;
    if (!pending_ && !hasMods && !functionKey) return;

    auto all = effective();
    if (pending_) {
        KeySequence seq{*pending_, c};
        pending_.reset();
        for (auto& e : all) {
            if (e.seq == seq) {
                auto it = commands_.find(e.cmd);
                if (it != commands_.end() && contextOk(it->second) && it->second.handler) { it->second.handler(); return; }
            }
        }
        return;   // неизвестное продолжение — просто сбрасываем
    }
    // Есть ли двухшаговые сочетания с таким первым аккордом?
    bool prefix = false;
    for (auto& e : all) {
        if (e.seq.size() == 2 && e.seq[0] == c) {
            auto it = commands_.find(e.cmd);
            if (it != commands_.end() && contextOk(it->second)) { prefix = true; break; }
        }
    }
    for (auto& e : all) {
        if (e.seq.size() == 1 && e.seq[0] == c) {
            auto it = commands_.find(e.cmd);
            if (it == commands_.end() || !contextOk(it->second) || !it->second.handler) continue;
            // В поле ввода (не редакторе) не перехватываем Ctrl+C/V и т.п.
            if (it->second.when == "editor" && ImGui::GetIO().WantTextInput && ImGui::IsAnyItemActive()) continue;
            if (!prefix) { it->second.handler(); return; }
        }
    }
    if (prefix) { pending_ = c; pendingSince_ = now; }
}

std::string KeybindingManager::pendingHint() const {
    if (!pending_) return {};
    char buf[256];
    std::snprintf(buf, sizeof buf, Localization::instance().tr("keys.pending"), chordToString(*pending_).c_str());
    return buf;
}

// ---------------------------------------------------------------------------
//  Окно переназначения клавиш
// ---------------------------------------------------------------------------
void KeybindingManager::renderEditor(bool* open, const std::filesystem::path& resourceDir,
                                     const std::filesystem::path& userFile) {
    auto& L = Localization::instance();
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(L.tr("keys.title"), open)) { ImGui::End(); return; }

    // Пресет
    ImGui::SetNextItemWidth(200);
    const char* presetLabel = preset_ == "visualstudio" ? "Visual Studio" : "VS Code";
    if (ImGui::BeginCombo(L.tr("keys.preset"), presetLabel)) {
        for (auto& p : presetNames()) {
            const char* lbl = p == "visualstudio" ? "Visual Studio" : "VS Code";
            if (ImGui::Selectable(lbl, p == preset_)) { loadPreset(p, resourceDir); saveUserOverrides(userFile); }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(L.tr("keys.reset_all"))) { resetAll(); saveUserOverrides(userFile); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", L.tr("keys.search"), &filter_);

    // Запись сочетания: первый аккорд, затем (опционально) второй в течение 1 с.
    if (!capturingCmd_.empty()) {
        if (auto c = pollChord()) {
            if (ImGuiKey(*c & ~ImGuiMod_Mask_) == ImGuiKey_Escape && (*c & ImGuiMod_Mask_) == 0) {
                capturingCmd_.clear(); captured_.clear();
            } else if (captured_.size() < 2) {
                captured_.push_back(*c); capturedAt_ = ImGui::GetTime();
            }
        }
        const bool done = captured_.size() == 2 || (!captured_.empty() && ImGui::GetTime() - capturedAt_ > 1.0);
        if (done) {
            auto list = bindingsFor(capturingCmd_);
            list.insert(list.begin(), captured_);
            // Не дублируем одинаковые сочетания.
            std::vector<KeySequence> uniq;
            for (auto& s : list) if (std::find(uniq.begin(), uniq.end(), s) == uniq.end()) uniq.push_back(s);
            setBinding(capturingCmd_, uniq);
            saveUserOverrides(userFile);
            capturingCmd_.clear(); captured_.clear();
        }
    }

    if (ImGui::BeginTable("##keys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                           ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(L.tr("keys.command"), ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn(L.tr("keys.binding"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn(L.tr("keys.when"), ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 150.f);
        ImGui::TableHeadersRow();
        for (auto& [id, cmd] : commands_) {
            std::string title = L.tr("cmd." + id);
            if (!filter_.empty() && !fuzzyScore(title + " " + id, filter_)) continue;
            ImGui::PushID(id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(title.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", id.c_str());
            ImGui::TableNextColumn();
            if (capturingCmd_ == id) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s %s", L.tr("keys.press"),
                                   sequenceToString(captured_).c_str());
            } else {
                auto seqs = bindingsFor(id);
                std::string text;
                for (auto& s : seqs) { if (!text.empty()) text += "  |  "; text += sequenceToString(s); }
                ImGui::TextUnformatted(text.empty() ? "—" : text.c_str());
                // Подсветка конфликтов
                for (auto& s : seqs) {
                    auto cf = conflicts(id, s);
                    if (!cf.empty()) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "(!)");
                        if (ImGui::IsItemHovered()) {
                            std::string t = L.tr("keys.conflict");
                            for (auto& c : cf) t += "\n  " + std::string(L.tr("cmd." + c));
                            ImGui::SetTooltip("%s", t.c_str());
                        }
                        break;
                    }
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", cmd.when.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(L.tr("keys.add"))) { capturingCmd_ = id; captured_.clear(); }
            ImGui::SameLine();
            if (ImGui::SmallButton(L.tr("keys.clear"))) { setBinding(id, {}); saveUserOverrides(userFile); }
            ImGui::SameLine();
            if (overrides_.count(id) && ImGui::SmallButton(L.tr("keys.reset"))) { resetBinding(id); saveUserOverrides(userFile); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
//  Палитра команд (Ctrl+Shift+P): нечёткий поиск по локализованным названиям.
// ---------------------------------------------------------------------------
void KeybindingManager::renderPalette() {
    if (!paletteOpen_) return;
    auto& L = Localization::instance();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 40), ImGuiCond_Always, ImVec2(0.5f, 0));
    const float w = std::min(640.f, vp->WorkSize.x - 40);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, vp->WorkSize.y * 0.8f));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##commandPalette", nullptr, fl)) { ImGui::End(); return; }

    if (paletteFocus_) { ImGui::SetKeyboardFocusHere(); paletteFocus_ = false; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##q", L.tr("palette.hint"), &paletteFilter_);

    struct Item { int score; std::string id, title; };
    std::vector<Item> items;
    for (auto& [id, cmd] : commands_) {
        if (!cmd.handler || !contextOk(cmd)) continue;
        std::string title = L.tr("cmd." + id);
        int s = std::max(fuzzyScore(title, paletteFilter_), fuzzyScore(id, paletteFilter_));
        if (s > 0) items.push_back({s, id, title});
    }
    std::stable_sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.score > b.score; });
    if (items.size() > 14) items.resize(14);
    paletteSel_ = std::clamp(paletteSel_, 0, std::max(0, (int)items.size() - 1));
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) paletteSel_ = std::min(paletteSel_ + 1, (int)items.size() - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) paletteSel_ = std::max(paletteSel_ - 1, 0);

    std::string run;
    for (int i = 0; i < (int)items.size(); ++i) {
        std::string sc = shortcutText(items[i].id);
        if (ImGui::Selectable((items[i].title + "##" + items[i].id).c_str(), i == paletteSel_)) run = items[i].id;
        if (!sc.empty()) {
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(sc.c_str()).x - ImGui::GetStyle().WindowPadding.x);
            ImGui::TextDisabled("%s", sc.c_str());
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !items.empty()) run = items[paletteSel_].id;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !paletteFocus_))
        paletteOpen_ = false;
    ImGui::End();
    if (!run.empty()) { paletteOpen_ = false; execute(run); }
}

} // namespace ide
