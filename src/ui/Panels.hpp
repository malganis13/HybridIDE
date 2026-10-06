#pragma once
// ============================================================================
//  Panels — окна настроек и вспомогательные панели приложения:
//    * SettingsPanel — вкладки «Общие», «Оформление», «Клавиатура»,
//      «Сеть и расширения» (GitHub PAT, расширения, тулчейны, бэкап);
//    * renderAbout()  — окно «О программе».
//
//  Панели ничего не знают об Application: всё необходимое передаётся через
//  PanelContext (указатели на сервисы + колбэки). Это упрощает тестирование
//  и позволяет переиспользовать окна в других конфигурациях.
// ============================================================================
#include "core/Workspace.hpp"
#include "github/GitHubManager.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ide {

class ThemeManager;
class KeybindingManager;
class ToolchainManager;
class AudioEngine;

struct PanelContext {
    UserSettings*      settings = nullptr;
    ThemeManager*      themes = nullptr;
    KeybindingManager* keys = nullptr;
    GitHubManager*     github = nullptr;
    ToolchainManager*  toolchains = nullptr;
    AudioEngine*       audio = nullptr;
    std::filesystem::path resourceDir;

    std::function<void()>                         saveSettings;      // записать settings.json
    std::function<void()>                         reloadSettings;    // перечитать после восстановления
    std::function<void()>                         rebuildFonts;      // после смены размера шрифта
    std::function<void()>                         openKeybindings;   // окно переназначения клавиш
    std::function<void(const std::string&)>       log;               // в панель «Вывод»
    // Файлы для бэкапа: (путь в репозитории, содержимое).
    std::function<std::vector<std::pair<std::string, std::string>>()> collectBackupFiles;
    // Расширение установлено в каталог dir (обработку по типу делает приложение).
    std::function<void(const ExtensionManifest&, const std::filesystem::path& dir)> onExtensionInstalled;
};

enum class SettingsTab { General, Appearance, Keyboard, Network };

class SettingsPanel {
public:
    void render(bool* open, PanelContext& ctx);
    void focusTab(SettingsTab t) { requested_ = t; hasRequest_ = true; }

private:
    void tabGeneral(PanelContext& ctx);
    void tabKeyboard(PanelContext& ctx);
    void tabNetwork(PanelContext& ctx);
    void sectionToken(PanelContext& ctx);
    void sectionExtensions(PanelContext& ctx);
    void sectionToolchains(PanelContext& ctx);
    void sectionBackup(PanelContext& ctx);
    void installExtension(PanelContext& ctx, const GitHubRepo& repo);

    SettingsTab requested_ = SettingsTab::General;
    bool        hasRequest_ = false;

    // Токен
    std::string tokenInput_;
    std::string tokenStatus_;
    bool        tokenBusy_ = false, tokenLoaded_ = false;

    // Расширения
    std::string                             extQuery_;
    std::vector<GitHubRepo>                 extResults_;
    std::string                             extStatus_;
    bool                                    extBusy_ = false;
    std::map<std::string, std::string>      extInstallState_;   // repo -> статус

    // Бэкап
    std::string backupStatus_;
    bool        backupBusy_ = false;

    // Общие
    std::string newExtSource_;
};

void renderAbout(bool* open);

} // namespace ide
