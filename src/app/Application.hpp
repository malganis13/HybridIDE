#pragma once
// ============================================================================
//  Application — «композиционный корень» IDE.
//
//  Владеет всеми сервисами (редактор, LSP, DAP, терминал, Git, сборка,
//  аудио, шейдеры, темы, GitHub) и связывает их колбэками. Сами модули друг
//  о друге не знают — это упрощает тестирование и замену реализаций.
//
//  Кадр:
//    1. glfwPollEvents + MainThreadDispatcher::drain() (результаты фоновых задач);
//    2. ImGui::NewFrame, KeybindingManager::process();
//    3. меню, док-пространство, панели, статус-строка;
//    4. ThemeManager::update -> ShaderEngine: фон -> ImGui -> пост-эффект;
//    5. отрисовка вынесенных окон (multi-viewport) и SwapBuffers.
// ============================================================================
#include "audio/AudioEngine.hpp"
#include "core/BuildSystem.hpp"
#include "core/EditorCore.hpp"
#include "core/FileExplorer.hpp"
#include "core/GitManager.hpp"
#include "core/TerminalEngine.hpp"
#include "core/Workspace.hpp"
#include "github/GitHubManager.hpp"
#include "github/ToolchainManager.hpp"
#include "graphics/FishSimulation.hpp"
#include "graphics/ShaderEngine.hpp"
#include "graphics/WaveSimulation.hpp"
#include "lsp/DAPClient.hpp"
#include "lsp/LSPClient.hpp"
#include "templates/ProjectTemplates.hpp"
#include "themes/ThemeManager.hpp"
#include "ui/FileDialog.hpp"
#include "ui/KeybindingManager.hpp"
#include "ui/Panels.hpp"
#include "ui/TelemetryPanel.hpp"

#include <imgui.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace ide {

struct AppOptions {
    std::filesystem::path openPath;      // папка или файл из командной строки
    std::filesystem::path exeDir;        // каталог исполняемого файла (поиск resources/)
    bool                  safeMode = false;   // без шейдеров и звука (диагностика драйверов)
};

class Application {
public:
    explicit Application(AppOptions opts);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();                                   // главный цикл; код возврата процесса

private:
    // --- инициализация ------------------------------------------------------
    bool initWindow();
    void initImGui();
    void initServices();
    void wireCallbacks();
    void registerCommands();
    void rebuildFonts();
    std::filesystem::path findResourceDir() const;

    // --- кадр ---------------------------------------------------------------
    void frame();
    void renderMenuBar();
    void renderDockspace();
    void buildDefaultLayout(ImGuiID dockId);
    void renderPanels();
    void renderStatusBar();
    void renderWelcome();
    void renderQuitConfirm();
    void detectFocusChange();
    void composeAndPresent();

    // --- действия -----------------------------------------------------------
    void openFolder(const std::filesystem::path& p);
    void openPath(const std::filesystem::path& p);
    void startDebugging();
    LaunchConfig defaultLaunchConfig() const;
    void saveState();
    void loadSettingsIntoServices();
    void applyLanguageServers();
    void onExtensionInstalled(const ExtensionManifest& m, const std::filesystem::path& dir);
    std::vector<std::pair<std::string, std::string>> collectBackupFiles() const;
    void log(const std::string& line);
    void requestQuit();
    const char* shortcut(const char* cmd);   // для пунктов меню

    // --- состояние ----------------------------------------------------------
    AppOptions            opts_;
    GLFWwindow*           window_ = nullptr;
    std::filesystem::path resourceDir_;
    std::string           iniPath_;
    bool                  fontsDirty_ = false, layoutBuilt_ = false, quitRequested_ = false, quitConfirm_ = false;
    bool                  running_ = true, terminalStarted_ = false, editorFocused_ = false;
    float                 dpiScale_ = 1.f;
    ImGuiID               mainDockId_ = 0, editorDockId_ = 0;
    std::string           lastFocusedWindow_;
    std::string           shortcutBuf_;
    double                lastTime_ = 0.0;
    float                 time_ = 0.f;
    std::vector<FishVertex> fishVerts_;

    // --- сервисы (порядок объявления = порядок конструирования) ------------
    UserSettings      settings_;
    Workspace         workspace_;
    AudioEngine       audio_;
    ShaderEngine      shaders_;
    WaveSimulation    wave_{256, 160};
    FishSimulation    fish_;
    ThemeManager      themes_;
    EditorCore        editor_;
    TerminalEngine    terminal_;
    GitManager        git_;
    BuildSystem       build_;
    FileExplorer      explorer_;
    LspManager        lsp_;
    DAPClient         dap_;
    GitHubManager     github_;
    ToolchainManager  toolchains_{github_};
    ProjectTemplates  templates_;
    KeybindingManager keys_;
    std::unique_ptr<TelemetryPanel> telemetry_;
    SettingsPanel     settingsPanel_;
    PanelContext      panelCtx_;
    FileDialog        fileDialog_;

    // --- видимость панелей --------------------------------------------------
    struct Visible {
        bool explorer = true, git = true, terminal = true, output = true, problems = true;
        bool callStack = true, variables = true, watch = true, breakpoints = false, memory = false, console = true;
        bool telemetry = false, settings = false, keybindings = false, wizard = false, about = false;
        bool properties = false, gauges = true, imguiDemo = false, implotDemo = false;
    } show_;
};

} // namespace ide
