// ============================================================================
//  Localization.cpp — встроенные словари ru-RU / en-US и горячая смена языка.
//
//  Соглашения по ключам:
//    window.*   — заголовки окон; содержат "###Id" для стабильного докинга;
//    cmd.*      — названия команд (меню, палитра, редактор клавиш);
//    menu.*, settings.*, theme.*, net.*, keys.* ... — по модулям.
// ============================================================================
#include "ui/Localization.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace ide {

namespace {
struct Entry { const char* key; const char* ru; const char* en; };

// clang-format off
const Entry kEntries[] = {
    // --- общее --------------------------------------------------------------
    {"common.cancel", "Отмена", "Cancel"},

    // --- окна (### — стабильный ID для докинга) -----------------------------
    {"window.explorer",    "Проводник###Explorer",        "Explorer###Explorer"},
    {"window.properties",  "Свойства###Properties",       "Properties###Properties"},
    {"window.git",         "Git###Git",                   "Git###Git"},
    {"window.diff",        "Сравнение###Diff",            "Diff###Diff"},
    {"window.terminal",    "Терминал###Terminal",         "Terminal###Terminal"},
    {"window.output",      "Вывод###Output",              "Output###Output"},
    {"window.problems",    "Проблемы###Problems",         "Problems###Problems"},
    {"window.callstack",   "Стек вызовов###CallStack",    "Call Stack###CallStack"},
    {"window.variables",   "Переменные###Variables",      "Variables###Variables"},
    {"window.watch",       "Контрольные значения###Watch", "Watch###Watch"},
    {"window.breakpoints", "Точки останова###Breakpoints", "Breakpoints###Breakpoints"},
    {"window.memory",      "Память###Memory",             "Memory###Memory"},
    {"window.console",     "Консоль отладки###DebugConsole", "Debug Console###DebugConsole"},
    {"window.telemetry",   "Телеметрия###Telemetry",      "Telemetry###Telemetry"},
    {"window.welcome",     "Добро пожаловать###Welcome",  "Welcome###Welcome"},

    // --- меню ---------------------------------------------------------------
    {"menu.file", "Файл", "File"}, {"menu.edit", "Правка", "Edit"}, {"menu.view", "Вид", "View"},
    {"menu.build_menu", "Сборка", "Build"}, {"menu.debug", "Отладка", "Debug"}, {"menu.tools", "Инструменты", "Tools"},
    {"menu.help", "Справка", "Help"}, {"menu.recent", "Недавние папки", "Recent folders"},
    {"menu.theme", "Тема", "Theme"}, {"menu.language", "Язык / Language", "Language / Язык"},
    {"menu.debug_windows", "Окна отладки", "Debug windows"},
    {"menu.imgui_demo", "Демо Dear ImGui", "Dear ImGui demo"}, {"menu.implot_demo", "Демо ImPlot", "ImPlot demo"},
    {"menu.build", "Собрать", "Build"}, {"menu.run", "Запустить", "Run"}, {"menu.clean", "Очистить", "Clean"},

    // --- команды ------------------------------------------------------------
    {"cmd.file.new", "Новый файл", "New File"}, {"cmd.file.open", "Открыть файл…", "Open File…"},
    {"cmd.file.openFolder", "Открыть папку…", "Open Folder…"}, {"cmd.file.save", "Сохранить", "Save"},
    {"cmd.file.saveAll", "Сохранить всё", "Save All"}, {"cmd.file.close", "Закрыть вкладку", "Close Tab"},
    {"cmd.project.new", "Новый проект…", "New Project…"}, {"cmd.app.quit", "Выход", "Exit"},
    {"cmd.view.commandPalette", "Палитра команд", "Command Palette"},
    {"cmd.view.explorer", "Показать проводник", "Toggle Explorer"}, {"cmd.view.git", "Показать Git", "Toggle Git"},
    {"cmd.view.terminal", "Показать терминал", "Toggle Terminal"}, {"cmd.view.problems", "Показать проблемы", "Toggle Problems"},
    {"cmd.view.output", "Показать вывод", "Toggle Output"}, {"cmd.view.telemetry", "Телеметрия", "Telemetry"},
    {"cmd.view.settings", "Настройки", "Settings"}, {"cmd.view.keybindings", "Сочетания клавиш", "Keyboard Shortcuts"},
    {"cmd.view.extensions", "Сеть и расширения", "Network & Extensions"},
    {"cmd.view.debug", "Показать окна отладки", "Show Debug Windows"},
    {"cmd.view.resetLayout", "Сбросить раскладку окон", "Reset Window Layout"},
    {"cmd.theme.next", "Следующая тема", "Next Theme"}, {"cmd.language.toggle", "Переключить язык (RU/EN)", "Toggle Language (RU/EN)"},
    {"cmd.build.build", "Собрать проект", "Build Project"}, {"cmd.build.run", "Запустить без отладки", "Run Without Debugging"},
    {"cmd.build.clean", "Очистить сборку", "Clean Build"}, {"cmd.build.test", "Запустить тесты", "Run Tests"},
    {"cmd.build.cancel", "Прервать сборку", "Cancel Build"},
    {"cmd.debug.start", "Начать / продолжить отладку", "Start / Continue Debugging"},
    {"cmd.debug.stop", "Остановить отладку", "Stop Debugging"}, {"cmd.debug.restart", "Перезапустить отладку", "Restart Debugging"},
    {"cmd.debug.pause", "Приостановить", "Pause"}, {"cmd.debug.stepOver", "Шаг с обходом", "Step Over"},
    {"cmd.debug.stepInto", "Шаг с заходом", "Step Into"}, {"cmd.debug.stepOut", "Шаг с выходом", "Step Out"},
    {"cmd.editor.undo", "Отменить", "Undo"}, {"cmd.editor.redo", "Повторить", "Redo"},
    {"cmd.editor.copy", "Копировать", "Copy"}, {"cmd.editor.cut", "Вырезать", "Cut"}, {"cmd.editor.paste", "Вставить", "Paste"},
    {"cmd.editor.selectAll", "Выделить всё", "Select All"}, {"cmd.editor.find", "Найти", "Find"},
    {"cmd.editor.addNextOccurrence", "Добавить следующее вхождение", "Add Next Occurrence"},
    {"cmd.editor.addCursorAbove", "Курсор выше", "Add Cursor Above"}, {"cmd.editor.addCursorBelow", "Курсор ниже", "Add Cursor Below"},
    {"cmd.editor.toggleComment", "Закомментировать строку", "Toggle Line Comment"},
    {"cmd.editor.duplicateLine", "Дублировать строку", "Duplicate Line"},
    {"cmd.editor.toggleFold", "Свернуть/развернуть блок", "Toggle Fold"},
    {"cmd.editor.foldAll", "Свернуть всё", "Fold All"}, {"cmd.editor.unfoldAll", "Развернуть всё", "Unfold All"},
    {"cmd.editor.triggerCompletion", "Подсказать автодополнение", "Trigger Suggest"},
    {"cmd.editor.goToDefinition", "Перейти к определению", "Go to Definition"},
    {"cmd.editor.toggleBreakpoint", "Точка останова", "Toggle Breakpoint"},
    {"cmd.editor.zoomIn", "Увеличить шрифт редактора", "Zoom In"}, {"cmd.editor.zoomOut", "Уменьшить шрифт редактора", "Zoom Out"},
    {"cmd.git.refresh", "Git: обновить", "Git: Refresh"}, {"cmd.git.pull", "Git: pull", "Git: Pull"},
    {"cmd.git.push", "Git: push", "Git: Push"}, {"cmd.help.about", "О программе", "About"},

    // --- статус-строка / выход / приветствие ------------------------------
    {"status.building", "сборка…", "building…"}, {"status.line", "Стр", "Ln"}, {"status.col", "Стб", "Col"},
    {"status.debug_running", "▶ отладка", "▶ debugging"}, {"status.debug_paused", "⏸ пауза", "⏸ paused"},
    {"quit.title", "Несохранённые изменения", "Unsaved changes"},
    {"quit.message", "Следующие файлы не сохранены:", "The following files have unsaved changes:"},
    {"quit.save_all", "Сохранить всё и выйти", "Save all and exit"}, {"quit.discard", "Выйти без сохранения", "Discard and exit"},
    {"welcome.subtitle", "Универсальная IDE: C++, Rust, Python, Go, Node.js", "Universal IDE: C++, Rust, Python, Go, Node.js"},
    {"palette.hint", "Введите команду…", "Type a command…"},

    // --- диалог файлов -------------------------------------------------------
    {"dialog.open_file", "Открыть файл", "Open file"}, {"dialog.open_folder", "Открыть папку", "Open folder"},
    {"dialog.save_file", "Сохранить как", "Save as"}, {"dialog.name", "Имя файла", "File name"},
    {"dialog.open", "Открыть", "Open"}, {"dialog.save", "Сохранить", "Save"}, {"dialog.not_found", "Путь не найден", "Path not found"},

    // --- Git -----------------------------------------------------------------
    {"git.no_folder", "Откройте папку проекта", "Open a project folder"},
    {"git.not_repo", "Папка не является Git-репозиторием", "This folder is not a Git repository"},
    {"git.init", "Инициализировать репозиторий", "Initialize Repository"}, {"git.new_branch", "Новая ветка", "New branch"},
    {"git.pull", "Pull", "Pull"}, {"git.push", "Push", "Push"}, {"git.refresh", "Обновить", "Refresh"},
    {"git.behind", "Отстаёт от origin на коммитов", "Commits behind origin"},
    {"git.changes", "Изменения", "Changes"}, {"git.commit", "Коммит", "Commit"}, {"git.stage_all", "Индексировать всё", "Stage All"},
    {"git.staged", "Проиндексировано", "Staged"}, {"git.unstaged", "Не проиндексировано", "Unstaged"},
    {"git.stage", "Добавить в индекс", "Stage"}, {"git.unstage", "Убрать из индекса", "Unstage"},
    {"git.history", "История", "History"}, {"git.graph", "Граф", "Graph"}, {"git.message", "Сообщение", "Message"},
    {"git.author", "Автор", "Author"}, {"git.date", "Дата", "Date"}, {"git.side_by_side", "Бок о бок", "Side by side"},

    // --- вывод / проблемы ----------------------------------------------------
    {"output.clear", "Очистить", "Clear"}, {"output.autoscroll", "Автопрокрутка", "Auto-scroll"},
    {"problems.errors", "Ошибки", "Errors"}, {"problems.warnings", "Предупреждения", "Warnings"},
    {"problems.only_errors", "Только ошибки", "Errors only"}, {"problems.errors_warnings", "Ошибки и предупреждения", "Errors and warnings"},
    {"problems.all", "Все", "All"}, {"problems.message", "Сообщение", "Message"}, {"problems.file", "Файл", "File"},
    {"problems.line", "Строка", "Line"},

    // --- проводник / свойства ------------------------------------------------
    {"explorer.open", "Открыть", "Open"}, {"explorer.new_file", "Новый файл", "New File"},
    {"explorer.new_folder", "Новая папка", "New Folder"}, {"explorer.rename", "Переименовать", "Rename"},
    {"explorer.delete", "Удалить", "Delete"}, {"explorer.copy_path", "Копировать путь", "Copy Path"},
    {"explorer.refresh", "Обновить", "Refresh"}, {"explorer.no_folder", "Папка не открыта", "No folder opened"},
    {"explorer.confirm_delete", "Удалить безвозвратно?", "Delete permanently?"},
    {"props.none", "Нет", "None"}, {"props.name", "Имя", "Name"}, {"props.path", "Путь", "Path"},
    {"props.type", "Тип", "Type"}, {"props.folder", "Папка", "Folder"}, {"props.size", "Размер", "Size"},
    {"props.perms", "Права", "Permissions"},

    // --- отладчик ------------------------------------------------------------
    {"debug.running", "Программа выполняется…", "Program is running…"},
    {"debug.not_stopped", "Отладчик не остановлен", "Debugger is not paused"},
    {"debug.name", "Имя", "Name"}, {"debug.value", "Значение", "Value"}, {"debug.type", "Тип", "Type"},
    {"debug.add_watch", "Добавить выражение…", "Add expression…"}, {"debug.remove_all", "Удалить все", "Remove All"},
    {"debug.mem_address", "Адрес / memoryReference", "Address / memoryReference"}, {"debug.read", "Прочитать", "Read"},
    {"debug.continue", "Продолжить", "Continue"}, {"debug.step_over", "Шаг с обходом", "Step Over"},
    {"debug.step_in", "Шаг с заходом", "Step Into"}, {"debug.step_out", "Шаг с выходом", "Step Out"},
    {"debug.pause", "Пауза", "Pause"}, {"debug.stop", "Стоп", "Stop"}, {"debug.evaluate", "Вычислить…", "Evaluate…"},

    // --- мастер проектов ---------------------------------------------------
    {"wizard.title", "Новый проект###Wizard", "New Project###Wizard"}, {"wizard.name", "Имя проекта", "Project name"},
    {"wizard.invalid_name", "Допустимы латиница, цифры, '-' и '_'", "Use letters, digits, '-' and '_'"},
    {"wizard.location", "Расположение", "Location"}, {"wizard.author", "Автор", "Author"},
    {"wizard.init_git", "Инициализировать Git", "Initialize Git"}, {"wizard.mit", "Лицензия MIT", "MIT license"},
    {"wizard.result", "Будет создано", "Will be created"}, {"wizard.create", "Создать", "Create"},

    // --- телеметрия ----------------------------------------------------------
    {"telemetry.title", "Телеметрия###Telemetry", "Telemetry###Telemetry"},
    {"telemetry.no_gpu", "нет данных (NVML / sysfs недоступны)", "no data (NVML / sysfs unavailable)"},
    {"telemetry.ide_memory", "Память IDE", "IDE memory"}, {"telemetry.history", "История", "History"},

    // --- клавиши -------------------------------------------------------------
    {"keys.title", "Сочетания клавиш###Keybindings", "Keyboard Shortcuts###Keybindings"},
    {"keys.preset", "Пресет", "Preset"}, {"keys.reset_all", "Сбросить всё", "Reset All"},
    {"keys.search", "Поиск команды…", "Search commands…"}, {"keys.command", "Команда", "Command"},
    {"keys.binding", "Сочетание", "Keybinding"}, {"keys.when", "Контекст", "When"},
    {"keys.press", "Нажмите сочетание (Esc — отмена):", "Press keys (Esc to cancel):"},
    {"keys.conflict", "Конфликт с:", "Conflicts with:"}, {"keys.add", "Добавить", "Add"},
    {"keys.clear", "Очистить", "Clear"}, {"keys.reset", "Сброс", "Reset"},
    {"keys.pending", "Нажато (%s). Ожидание второй клавиши…", "(%s) was pressed. Waiting for second key…"},

    // --- настройки -------------------------------------------------------------
    {"settings.title", "Настройки###Settings", "Settings###Settings"},
    {"settings.tab_general", "Общие", "General"}, {"settings.tab_appearance", "Оформление", "Appearance"},
    {"settings.tab_keyboard", "Клавиатура", "Keyboard"}, {"settings.tab_network", "Сеть и расширения", "Network & Extensions"},
    {"settings.language", "Язык интерфейса", "Interface language"}, {"settings.interface", "Интерфейс", "Interface"},
    {"settings.font_size", "Размер шрифта", "Font size"}, {"settings.ui_scale", "Масштаб интерфейса", "UI scale"},
    {"settings.vsync", "Вертикальная синхронизация", "VSync"}, {"settings.recent", "Недавние папки", "Recent folders"},
    {"settings.clear_recent", "Очистить список", "Clear list"},
    {"settings.keymap_desc", "Выберите раскладку по умолчанию. Ваши изменения хранятся отдельно и сохраняются при смене пресета.",
                             "Choose a default keymap. Your own changes are stored separately and survive preset switches."},
    {"settings.open_keybindings", "Открыть редактор сочетаний…", "Open keybindings editor…"},

    // --- сеть / GitHub -------------------------------------------------------
    {"net.token", "GitHub Personal Access Token", "GitHub Personal Access Token"},
    {"net.token_desc", "Токен хранится в защищённом хранилище ОС (DPAPI в Windows, файл 0600 в Linux/macOS) и не попадает в бэкапы. "
                       "Достаточно прав: repo (или fine-grained: Contents + Administration для создания репозитория бэкапа).",
                       "The token is kept in OS-protected storage (DPAPI on Windows, 0600 file on Linux/macOS) and never goes into backups. "
                       "Required scopes: repo (or fine-grained: Contents + Administration for creating the backup repository)."},
    {"net.token_saved", "токен сохранён — введите новый для замены", "token saved — type a new one to replace"},
    {"net.validate", "Сохранить и проверить", "Save & validate"}, {"net.forget", "Забыть токен", "Forget token"},
    {"net.checking", "Проверка…", "Checking…"}, {"net.signed_in", "Вход выполнен:", "Signed in as"},
    {"net.scopes", "Права токена", "Token scopes"},
    {"net.scopes_warning", "Внимание: у токена избыточные опасные права, рекомендуется перевыпустить без них:",
                           "Warning: the token has excessive dangerous scopes, consider re-issuing it without:"},
    {"net.secret_failed", "Не удалось сохранить токен в защищённое хранилище", "Failed to store the token securely"},
    {"net.extensions", "Расширения (GitHub topic: hybridide-extension)", "Extensions (GitHub topic: hybridide-extension)"},
    {"net.ext_search_hint", "Поиск расширений…", "Search extensions…"}, {"net.search", "Найти", "Search"},
    {"net.searching", "Поиск…", "Searching…"}, {"net.found", "найдено", "found"},
    {"net.ext_sources", "Свои источники (owner/repo)", "Custom sources (owner/repo)"},
    {"net.ext_name", "Название", "Name"}, {"net.ext_desc", "Описание", "Description"},
    {"net.install", "Установить", "Install"}, {"net.installing", "Установка…", "Installing…"},
    {"net.installed", "Установлено", "Installed"}, {"net.partial", "Установлено частично", "Partially installed"},
    {"net.failed", "Ошибка", "Failed"}, {"net.update", "Обновить", "Update"}, {"net.status", "Состояние", "Status"},
    {"net.toolchains", "Тулчейны (ассеты релизов GitHub)", "Toolchains (GitHub release assets)"},
    {"net.install_root", "Каталог установки:", "Install root:"},
    {"net.backup", "Резервная копия настроек", "Settings backup"},
    {"net.backup_desc", "Настройки, сочетания клавиш, реестр тулчейнов и workspace-файл текущего проекта сохраняются в приватный "
                        "репозиторий (создаётся автоматически через POST /user/repos).",
                        "Settings, keybindings, toolchain registry and the current workspace file are pushed to a private repository "
                        "(created automatically via POST /user/repos)."},
    {"net.backup_repo", "Репозиторий", "Repository"}, {"net.backup_now", "Создать бэкап", "Back up now"},
    {"net.restore", "Восстановить", "Restore"}, {"net.uploading", "Загрузка…", "Uploading…"},
    {"net.downloading", "Скачивание…", "Downloading…"}, {"net.backup_done", "Бэкап сохранён", "Backup saved"},
    {"net.restore_done", "Настройки восстановлены", "Settings restored"},
    {"net.need_token", "нужен проверенный токен", "a validated token is required"},

    // --- темы ----------------------------------------------------------------
    {"theme.name.Aquarium", "Аквариум", "Aquarium"}, {"theme.name.Steampunk", "Стимпанк", "Steampunk"},
    {"theme.name.Hacker", "Хакер", "Hacker"}, {"theme.name.Cyberpunk", "Киберпанк", "Cyberpunk"},
    {"theme.select", "Тема оформления", "Theme"}, {"theme.audio", "Звук", "Audio"},
    {"theme.audio_enabled", "Звук включён", "Audio enabled"}, {"theme.typing_sounds", "Звук печати", "Typing sounds"},
    {"theme.master_volume", "Общая громкость", "Master volume"}, {"theme.effects_volume", "Громкость эффектов", "Effects volume"},
    {"theme.ambience_volume", "Громкость фона", "Ambience volume"}, {"theme.test_sound", "Проверить звук", "Test sound"},
    {"theme.visual", "Визуальные параметры", "Visual parameters"}, {"theme.animations", "Анимации и фон", "Animations & background"},
    {"theme.editor_opacity", "Непрозрачность окон", "Window opacity"}, {"theme.intensity", "Яркость фона", "Background intensity"},
    {"theme.background", "Фоновое изображение", "Background image"},
    {"theme.media_hint", "Путь к PNG/JPG (до 4K)", "Path to PNG/JPG (up to 4K)"},
    {"theme.media_note", "Изображение смешивается с шейдером темы; пустой путь — только процедурный фон.",
                         "The image is blended with the theme shader; empty path — procedural background only."},
    {"theme.apply", "Применить", "Apply"}, {"theme.clear", "Убрать", "Clear"},
    {"theme.fish_count", "Количество рыб", "Fish count"}, {"theme.fish_speed", "Скорость рыб", "Fish speed"},
    {"theme.scare", "Пугливость рыб", "Fish scare factor"}, {"theme.fish_fog", "Глубина (туман)", "Depth fog"},
    {"theme.rays", "Солнечные лучи", "Sun rays"}, {"theme.ripple", "Сила ряби от клика", "Click ripple strength"},
    {"theme.wave_damping", "Затухание волн", "Wave damping"}, {"theme.deep_color", "Цвет глубины", "Deep water color"},
    {"theme.shallow_color", "Цвет мелководья", "Shallow water color"},
    {"theme.gear_speed", "Скорость шестерён", "Gear speed"}, {"theme.sepia", "Сепия", "Sepia"},
    {"theme.show_gauges", "Манометры CPU/RAM", "CPU/RAM gauges"}, {"theme.steam_on_build", "Пар при сборке", "Steam on build"},
    {"theme.bell_on_enter", "Звонок каретки на Enter", "Carriage bell on Enter"},
    {"theme.gauges_window", "Манометры###Gauges", "Gauges###Gauges"},
    {"theme.phosphor", "Цвет люминофора", "Phosphor color"}, {"theme.green", "Зелёный", "Green"}, {"theme.amber", "Янтарный", "Amber"},
    {"theme.curvature", "Кривизна CRT", "CRT curvature"}, {"theme.scanlines", "Строки развёртки", "Scanlines"},
    {"theme.glow", "Свечение", "Glow"}, {"theme.mono", "Монохром", "Monochrome"}, {"theme.rain_speed", "Скорость «дождя»", "Rain speed"},
    {"theme.neon_a", "Неон A", "Neon A"}, {"theme.neon_b", "Неон B", "Neon B"},
    {"theme.aberration", "Хроматическая аберрация", "Chromatic aberration"},
    {"theme.glitch_on_focus", "Глитч при смене панели", "Glitch on panel focus"},
    {"theme.glitch_strength", "Сила глитча", "Glitch strength"}, {"theme.test_glitch", "Проверить глитч", "Test glitch"},

    // --- о программе ----------------------------------------------------------
    {"about.title", "О программе###About", "About###About"},
    {"about.desc", "Кроссплатформенная IDE на C++20: Dear ImGui (docking + viewports), LSP/DAP, PTY-терминал, Git, "
                   "живые темы с шейдерами и процедурным звуком.",
                   "Cross-platform C++20 IDE: Dear ImGui (docking + viewports), LSP/DAP, PTY terminal, Git, "
                   "live shader themes with procedural audio."},
    {"about.license", "Лицензия MIT", "MIT License"},
};
// clang-format on
} // namespace

Localization& Localization::instance() {
    static Localization L;
    return L;
}

Localization::Localization() {
    auto& ru = builtin_["ru-RU"];
    auto& en = builtin_["en-US"];
    for (const auto& e : kEntries) { ru[e.key] = e.ru; en[e.key] = e.en; }
    rebuild();
}

void Localization::rebuild() {
    // Цепочка: встроенный en-US (запасной) -> встроенный язык -> JSON-переопределения.
    Table t = builtin_["en-US"];
    if (auto it = builtin_.find(lang_); it != builtin_.end()) for (auto& [k, v] : it->second) t[k] = v;
    if (auto it = overrides_.find(lang_); it != overrides_.end()) for (auto& [k, v] : it->second) t[k] = v;
    active_ = std::move(t);
}

const char* Localization::tr(const char* key) const {
    auto it = active_.find(key);
    return it != active_.end() ? it->second.c_str() : key;
}

bool Localization::has(const std::string& key) const { return active_.count(key) != 0; }

bool Localization::setLanguage(const std::string& lang) {
    if (!builtin_.count(lang) && !overrides_.count(lang)) return false;
    if (lang == lang_) return true;
    lang_ = lang;
    rebuild();
    for (auto& cb : listeners_) cb(lang_);
    return true;
}

std::vector<std::string> Localization::availableLanguages() const {
    std::vector<std::string> out{"ru-RU", "en-US"};
    for (auto& [l, _] : overrides_) if (!builtin_.count(l)) out.push_back(l);
    return out;
}

const char* Localization::languageDisplayName(const std::string& lang) {
    if (lang == "ru-RU") return "Русский";
    if (lang == "en-US") return "English";
    return "Custom";
}

void Localization::loadOverrides(const std::filesystem::path& dir) {
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        std::ifstream f(it->path());
        auto j = nlohmann::json::parse(f, nullptr, false);
        if (!j.is_object()) continue;
        auto& table = overrides_[it->path().stem().string()];
        for (auto& [k, v] : j.items()) if (v.is_string()) table[k] = v.get<std::string>();
    }
    rebuild();
}

std::vector<std::string> Localization::builtinKeys() const {
    std::vector<std::string> keys;
    for (const auto& e : kEntries) keys.emplace_back(e.key);
    return keys;
}

} // namespace ide
