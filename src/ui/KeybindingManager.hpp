#pragma once
// ============================================================================
//  KeybindingManager — переназначаемые горячие клавиши и палитра команд.
//
//  Модель:
//    * Команда  — строковый id ("file.save", "debug.stepOver") + обработчик +
//                 контекст ("global" — везде, "editor" — только когда фокус
//                 в редакторе и не активно другое поле ввода).
//    * Сочетание (KeySequence) — 1..2 «аккорда» (ImGuiKeyChord): поддержаны
//                 двухшаговые сочетания в стиле VS Code / Visual Studio,
//                 например "Ctrl+K Ctrl+C".
//    * Пресет   — набор привязок по умолчанию: "vscode" или "visualstudio".
//                 Загружается из resources/keymaps/<name>.json, а при его
//                 отсутствии — из встроенной таблицы.
//    * Пользовательские переопределения хранятся отдельно
//                 (<config>/keybindings.json) и накладываются поверх пресета,
//                 поэтому смена пресета не теряет ручные настройки.
//
//  process() вызывается один раз за кадр ДО отрисовки панелей: он ловит
//  нажатия, ведёт состояние двухшаговых сочетаний и вызывает обработчики.
// ============================================================================
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ide {

using KeySequence = std::vector<ImGuiKeyChord>;   // 1 или 2 аккорда

struct Command {
    std::string           id;
    std::string           when = "global";          // "global" | "editor"
    std::function<void()> handler;
};

class KeybindingManager {
public:
    KeybindingManager();

    // --- Команды ---------------------------------------------------------
    void registerCommand(const std::string& id, std::function<void()> handler, const std::string& when = "global");
    bool execute(const std::string& id);                       // вызвать вручную (меню, палитра)
    [[nodiscard]] const std::map<std::string, Command>& commands() const { return commands_; }

    // --- Пресеты и привязки ---------------------------------------------
    static std::vector<std::string> presetNames() { return {"vscode", "visualstudio"}; }
    bool loadPreset(const std::string& name, const std::filesystem::path& resourceDir);
    [[nodiscard]] const std::string& preset() const { return preset_; }

    void setBinding(const std::string& cmd, std::vector<KeySequence> seqs);  // пользовательская привязка
    void resetBinding(const std::string& cmd);
    void resetAll() { overrides_.clear(); }
    [[nodiscard]] std::vector<KeySequence> bindingsFor(const std::string& cmd) const;
    [[nodiscard]] std::string shortcutText(const std::string& cmd) const;    // для пунктов меню
    // Найти другие команды с тем же сочетанием (конфликты).
    [[nodiscard]] std::vector<std::string> conflicts(const std::string& cmd, const KeySequence& seq) const;

    bool loadUserOverrides(const std::filesystem::path& file);
    bool saveUserOverrides(const std::filesystem::path& file) const;
    static nlohmann::json builtinPresetJson(const std::string& name);       // для экспорта/тестов

    // --- Кадр ------------------------------------------------------------
    // contextActive("editor") должен вернуть true, если фокус в редакторе.
    std::function<bool(const std::string& when)> contextActive;
    void process();
    // Подсказка «Нажата Ctrl+K, ожидание второй клавиши…» для статус-строки.
    [[nodiscard]] std::string pendingHint() const;

    // --- UI ----------------------------------------------------------------
    void renderEditor(bool* open, const std::filesystem::path& resourceDir, const std::filesystem::path& userFile);
    void openPalette() { paletteOpen_ = true; paletteFocus_ = true; paletteFilter_.clear(); paletteSel_ = 0; }
    void renderPalette();

    // --- Преобразования строк ------------------------------------------
    static std::string chordToString(ImGuiKeyChord c);
    static std::string sequenceToString(const KeySequence& s);
    static std::optional<ImGuiKeyChord> parseChord(const std::string& s);
    static std::optional<KeySequence>  parseSequence(const std::string& s);

private:
    struct Effective { std::string cmd; KeySequence seq; };
    [[nodiscard]] std::vector<Effective> effective() const;
    static std::optional<ImGuiKeyChord> pollChord();          // новый нажатый аккорд в этом кадре
    bool contextOk(const Command& c) const;

    std::map<std::string, Command>                  commands_;
    std::map<std::string, std::vector<KeySequence>> presetBindings_;
    std::map<std::string, std::vector<KeySequence>> overrides_;
    std::string                                     preset_ = "vscode";

    // Состояние двухшаговых сочетаний.
    std::optional<ImGuiKeyChord> pending_;
    double                       pendingSince_ = 0;

    // Состояние UI переназначения.
    std::string  filter_;
    std::string  capturingCmd_;
    KeySequence  captured_;
    double       capturedAt_ = 0;

    // Палитра команд.
    bool        paletteOpen_ = false, paletteFocus_ = false;
    std::string paletteFilter_;
    int         paletteSel_ = 0;
};

} // namespace ide
