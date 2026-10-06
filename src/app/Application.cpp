// ============================================================================
//  Application.cpp — главный цикл и связывание модулей IDE.
// ============================================================================
#include "app/Application.hpp"

#include "core/EventQueue.hpp"
#include "core/Process.hpp"
#include "graphics/GLLoader.hpp"
#include "ui/Localization.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace ide {

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void glfwErrorCallback(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc);
}

// Диапазоны глифов: латиница, кириллица, пунктуация, стрелки, псевдографика
// терминала (box drawing / блоки), геометрические фигуры, звёздочка ★.
const ImWchar* glyphRanges() {
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesCyrillic());
        static const ImWchar extra[] = {0x2000, 0x206F, 0x2190, 0x21FF, 0x2500, 0x259F, 0x25A0, 0x25FF,
                                        0x2600, 0x26FF, 0x2700, 0x27BF, 0};
        b.AddRanges(extra);
        b.BuildRanges(&ranges);
    }
    return ranges.Data;
}

} // namespace

// ===========================================================================
//  Конструирование / разрушение
// ===========================================================================
Application::Application(AppOptions opts) : opts_(std::move(opts)) {}

Application::~Application() {
    // Порядок важен: сначала внешние процессы (LSP/DAP), затем GL и окно.
    dap_.stop();
    lsp_.shutdownAll();
    telemetry_.reset();
    audio_.shutdown();
    if (window_) {
        shaders_.destroy();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

fs::path Application::findResourceDir() const {
    std::error_code ec;
    std::vector<fs::path> candidates{opts_.exeDir / "resources", opts_.exeDir / ".." / "share" / "HybridIDE" / "resources"};
#ifdef IDE_RESOURCE_DIR
    candidates.emplace_back(IDE_RESOURCE_DIR);
#endif
    for (auto& c : candidates)
        if (fs::is_directory(c, ec)) return fs::weakly_canonical(c, ec);
    return {};
}

void Application::log(const std::string& line) {
    std::string l = line;
    if (l.empty() || l.back() != '\n') l += '\n';
    build_.appendOutput(l);
}

const char* Application::shortcut(const char* cmd) {
    shortcutBuf_ = keys_.shortcutText(cmd);
    return shortcutBuf_.empty() ? nullptr : shortcutBuf_.c_str();
}

// ===========================================================================
//  Инициализация
// ===========================================================================
bool Application::initWindow() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    window_ = glfwCreateWindow(1600, 960, "HybridIDE", nullptr, nullptr);
    if (!window_) { glfwTerminate(); return false; }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(settings_.vsync ? 1 : 0);
    glfwSetWindowUserPointer(window_, this);

    float sx = 1, sy = 1;
    glfwGetWindowContentScale(window_, &sx, &sy);
    dpiScale_ = std::max(1.f, sx);

    // Перетаскивание файлов/папок в окно
    glfwSetDropCallback(window_, [](GLFWwindow* w, int count, const char** paths) {
        auto* self = static_cast<Application*>(glfwGetWindowUserPointer(w));
        for (int i = 0; i < count; ++i) self->openPath(fs::u8path(paths[i]));
    });
    // Закрытие окна — через подтверждение, если есть несохранённые файлы
    glfwSetWindowCloseCallback(window_, [](GLFWwindow* w) {
        auto* self = static_cast<Application*>(glfwGetWindowUserPointer(w));
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        self->requestQuit();
    });
    return true;
}

void Application::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDockingTransparentPayload = true;
    // Раскладка окон хранится в профиле пользователя, а не в текущем каталоге.
    std::error_code ec;
    fs::create_directories(Workspace::userConfigDir(), ec);
    iniPath_ = (Workspace::userConfigDir() / "imgui_layout.ini").string();
    layoutBuilt_ = fs::exists(iniPath_, ec);
    io.IniFilename = iniPath_.c_str();

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
#ifdef __APPLE__
    ImGui_ImplOpenGL3_Init("#version 150");
#else
    ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
}

void Application::rebuildFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const float size = std::round(settings_.fontSize * settings_.uiScale * dpiScale_);

    std::vector<fs::path> candidates;
    if (!resourceDir_.empty()) {
        candidates.push_back(resourceDir_ / "fonts" / "JetBrainsMono-Regular.ttf");
        candidates.push_back(resourceDir_ / "fonts" / "DejaVuSansMono.ttf");
    }
#if defined(_WIN32)
    candidates.insert(candidates.end(), {"C:/Windows/Fonts/CascadiaMono.ttf", "C:/Windows/Fonts/consola.ttf",
                                         "C:/Windows/Fonts/cour.ttf"});
#elif defined(__APPLE__)
    candidates.insert(candidates.end(), {"/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf",
                                         "/Library/Fonts/Arial Unicode.ttf"});
#else
    candidates.insert(candidates.end(), {"/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
                                         "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                         "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
                                         "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
                                         "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                                         "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
                                         "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
                                         "/usr/share/fonts/noto/NotoSansMono-Regular.ttf"});
#endif
    ImFont* font = nullptr;
    std::error_code ec;
    for (auto& c : candidates) {
        if (!fs::exists(c, ec)) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        font = io.Fonts->AddFontFromFileTTF(c.string().c_str(), size, &cfg, glyphRanges());
        if (font) break;
    }
    if (!font) {
        // Встроенный ProggyClean не содержит кириллицы — предупреждаем в логе.
        ImFontConfig cfg;
        cfg.SizePixels = size;
        io.Fonts->AddFontDefault(&cfg);
        std::fprintf(stderr, "[fonts] TTF font not found, Cyrillic glyphs will be missing. Put a font into resources/fonts/\n");
    }
    io.Fonts->Build();

    // Стиль: сброс размеров, повторное применение темы и масштабирование под DPI.
    ImGui::GetStyle() = ImGuiStyle();
    themes_.apply(themes_.currentName());
    ImGui::GetStyle().ScaleAllSizes(settings_.uiScale * dpiScale_);
    editor_.palette = themes_.editorPalette();
}

void Application::initServices() {
    resourceDir_ = findResourceDir();
    Workspace::loadSettings(settings_);

    auto& L = Localization::instance();
    if (!resourceDir_.empty()) L.loadOverrides(resourceDir_ / "locales");
    L.setLanguage(settings_.language);

    // Звук и шейдеры (безопасный режим отключает оба — для диагностики драйверов)
    std::string err;
    if (!opts_.safeMode) {
        if (!audio_.init(&err)) std::fprintf(stderr, "[audio] %s\n", err.c_str());
        if (auto it = settings_.audio.find("master"); it != settings_.audio.end() && it->is_number())
            audio_.masterVolume = it->get<float>();
        auto getProc = [](const char* n) -> void* { return reinterpret_cast<void*>(glfwGetProcAddress(n)); };
        if (!shaders_.init(getProc, &err)) std::fprintf(stderr, "[shaders] %s\n", err.c_str());
        else if (!resourceDir_.empty()) shaders_.reloadFromDirectory(resourceDir_ / "shaders", nullptr);
    } else {
        gl::load([](const char* n) -> void* { return reinterpret_cast<void*>(glfwGetProcAddress(n)); });
    }

    // Темы
    themes_.attach(&audio_, &fish_, &wave_);
    themes_.fromJson(settings_.themes);
    themes_.applyBackgroundMedia = [this](const std::string& path, std::string* e) {
        if (path.empty()) { shaders_.clearBackgroundMedia(); return true; }
        return shaders_.setBackgroundMedia(fs::u8path(path), e);
    };
    themes_.onSettingsChanged = [this] {
        editor_.palette = themes_.editorPalette();
        settings_.theme = themes_.currentName();
        settings_.themes = themes_.toJson();
        Workspace::saveSettings(settings_);
    };
    themes_.apply(settings_.theme);

    // Клавиатура
    keys_.loadPreset(settings_.keymap, resourceDir_);
    keys_.loadUserOverrides(Workspace::userConfigDir() / "keybindings.json");
    keys_.contextActive = [this](const std::string& when) { return when == "editor" ? editorFocused_ : true; };

    // GitHub: токен из защищённого хранилища, проверка в фоне
    if (auto tok = Workspace::loadSecret("github_token")) {
        github_.setToken(*tok);
        github_.validateToken([this](bool ok, std::string e) {
            if (!ok) log("[github] " + e);
        });
    }
    toolchains_.log = [this](const std::string& s) { log("[toolchains] " + s); };
    toolchains_.loadRegistry();
    templates_.loadUserTemplates(Workspace::userConfigDir() / "templates");

    telemetry_ = std::make_unique<TelemetryPanel>();

    // Контекст панелей настроек
    panelCtx_.settings = &settings_;
    panelCtx_.themes = &themes_;
    panelCtx_.keys = &keys_;
    panelCtx_.github = &github_;
    panelCtx_.toolchains = &toolchains_;
    panelCtx_.audio = &audio_;
    panelCtx_.resourceDir = resourceDir_;
    panelCtx_.saveSettings = [this] {
        settings_.themes = themes_.toJson();
        Workspace::saveSettings(settings_);
        glfwSwapInterval(settings_.vsync ? 1 : 0);
    };
    panelCtx_.reloadSettings = [this] { loadSettingsIntoServices(); };
    panelCtx_.rebuildFonts = [this] { fontsDirty_ = true; };
    panelCtx_.openKeybindings = [this] { show_.keybindings = true; };
    panelCtx_.log = [this](const std::string& s) { log(s); };
    panelCtx_.collectBackupFiles = [this] { return collectBackupFiles(); };
    panelCtx_.onExtensionInstalled = [this](const ExtensionManifest& m, const fs::path& d) { onExtensionInstalled(m, d); };
}

void Application::loadSettingsIntoServices() {
    Workspace::loadSettings(settings_);
    Localization::instance().setLanguage(settings_.language);
    themes_.fromJson(settings_.themes);
    themes_.apply(settings_.theme);
    editor_.palette = themes_.editorPalette();
    keys_.loadPreset(settings_.keymap, resourceDir_);
    keys_.loadUserOverrides(Workspace::userConfigDir() / "keybindings.json");
    fontsDirty_ = true;
}

// ---------------------------------------------------------------------------
//  Связывание модулей
// ---------------------------------------------------------------------------
void Application::wireCallbacks() {
    // --- Редактор <-> LSP / DAP / темы --------------------------------------
    auto& cb = editor_.callbacks;
    cb.onOpened = [this](Document& d) { if (auto* c = lsp_.clientFor(d)) c->didOpen(d); };
    cb.onChanged = [this](Document& d, const std::vector<TextEdit>& e) { if (auto* c = lsp_.clientFor(d)) c->didChange(d, e); };
    cb.onSaved = [this](Document& d) { if (auto* c = lsp_.clientFor(d)) c->didSave(d); };
    cb.onClosed = [this](Document& d) { if (auto* c = lsp_.clientFor(d)) c->didClose(d); };
    cb.requestCompletion = [this](Document& d, TextPos pos) {
        const int id = d.id;
        const auto ver = d.completionVersion;
        auto* c = lsp_.clientFor(d);
        if (!c || !c->ready()) { editor_.setCompletionResult(id, ver, pos, {}); return; }   // только слова буфера
        c->completion(d, pos, [this, id, ver, pos](std::vector<CompletionCandidate> items) {
            editor_.setCompletionResult(id, ver, pos, std::move(items));
        });
    };
    cb.requestHover = [this](Document& d, TextPos pos) {
        auto* c = lsp_.clientFor(d);
        if (!c || !c->ready()) return;
        const int id = d.id;
        c->hover(d, pos, [this, id, pos](std::string text) { editor_.setHoverResult(id, pos, std::move(text)); });
    };
    cb.requestDefinition = [this](Document& d, TextPos pos) {
        auto* c = lsp_.clientFor(d);
        if (!c || !c->ready()) return;
        c->definition(d, pos, [this](fs::path p, int line, int col) { if (!p.empty()) editor_.goTo(p, line, col); });
    };
    cb.onBreakpoint = [this](Document& d, int line, bool on) { if (!d.path.empty()) dap_.setBreakpoint(d.path, line, on); };
    cb.onKeystroke = [this](unsigned int ch) { themes_.onKeystroke(ch); };
    cb.onEditorClick = [this](ImVec2 p) { themes_.onEditorClick(p); };

    lsp_.onDiagnostics = [this](const fs::path& p, std::vector<Diagnostic> d) { editor_.setDiagnostics(p, std::move(d)); };
    lsp_.onLog = [this](const std::string& s) { log("[lsp] " + s); };

    // --- Отладчик -----------------------------------------------------------
    dap_.onStoppedAt = [this](const fs::path& p, int line0) {
        editor_.setExecutionLine(p, line0);
        show_.variables = show_.callStack = true;
        glfwFocusWindow(window_);
    };
    dap_.onTerminated = [this] { editor_.clearExecutionLine(); log("[debug] session terminated"); };

    // --- Сборка -------------------------------------------------------------
    build_.onNavigate = [this](const fs::path& p, int line, int col) { editor_.goTo(p, std::max(0, line - 1), std::max(0, col - 1)); };
    build_.onFinished = [this](bool ok, double secs) {
        themes_.onBuildFinished(ok);
        char buf[96];
        std::snprintf(buf, sizeof buf, "[build] %s (%.1f s)", ok ? "OK" : "FAILED", secs);
        log(buf);
        if (!ok) show_.problems = true;
    };

    // --- Git / проводник ----------------------------------------------------
    git_.log = [this](const std::string& s) { log("[git] " + s); };
    git_.openFile = [this](const fs::path& p) { editor_.openFile(p); };
    explorer_.onOpenFile = [this](const fs::path& p) {
        std::string err;
        if (!editor_.openFile(p, &err)) log("[editor] " + err);
    };
    explorer_.onFileChangedOnDisk = [this](const fs::path& p) {
        // Перезагружаем, только если в редакторе нет несохранённых правок.
        Document* d = editor_.findByPath(p);
        if (!d || d->buffer.dirty()) return;
        TextPos pos = d->cursors.front().pos;
        editor_.close(d->id);
        editor_.goTo(p, pos.line, pos.col);
        git_.refresh();
    };
    explorer_.onBuild = [this] { keys_.execute("build.build"); };
    explorer_.onRun = [this] { keys_.execute("build.run"); };
    explorer_.onClean = [this] { keys_.execute("build.clean"); };
}

// ---------------------------------------------------------------------------
//  Команды (id совпадают с ключами раскладок и cmd.* в локализации)
// ---------------------------------------------------------------------------
void Application::registerCommands() {
    auto& k = keys_;
    auto root = [this] { return workspace_.hasFolder() ? workspace_.root() : fs::current_path(); };

    k.registerCommand("file.new", [this] { editor_.newUntitled(); });
    k.registerCommand("file.open", [this, root] {
        fileDialog_.open(FileDialog::Mode::OpenFile, root(), [this](const fs::path& p) { openPath(p); });
    });
    k.registerCommand("file.openFolder", [this, root] {
        fileDialog_.open(FileDialog::Mode::OpenFolder, root(), [this](const fs::path& p) { openFolder(p); });
    });
    k.registerCommand("file.save", [this, root] {
        Document* d = editor_.active();
        if (!d) return;
        if (d->path.empty()) {
            int id = d->id;
            fileDialog_.open(FileDialog::Mode::SaveFile, root(), [this, id](const fs::path& p) {
                if (Document* doc = editor_.find(id)) {
                    doc->path = p;
                    doc->title = p.filename().string();
                    std::string err;
                    if (!editor_.save(*doc, &err)) log("[editor] " + err);
                }
            });
        } else {
            editor_.executeCommand("file.save");
        }
    }, "editor");
    k.registerCommand("file.saveAll", [this] { editor_.saveAll(); });
    k.registerCommand("file.close", [this] { editor_.executeCommand("file.close"); }, "editor");
    k.registerCommand("project.new", [this] { show_.wizard = true; });
    k.registerCommand("app.quit", [this] { requestQuit(); });

    k.registerCommand("view.commandPalette", [this] { keys_.openPalette(); });
    k.registerCommand("view.explorer", [this] { show_.explorer = !show_.explorer; });
    k.registerCommand("view.git", [this] { show_.git = !show_.git; });
    k.registerCommand("view.terminal", [this] {
        show_.terminal = !show_.terminal;
        if (show_.terminal && !terminalStarted_) { terminal_.newSession(workspace_.hasFolder() ? workspace_.root() : fs::current_path()); terminalStarted_ = true; }
    });
    k.registerCommand("view.problems", [this] { show_.problems = !show_.problems; });
    k.registerCommand("view.output", [this] { show_.output = !show_.output; });
    k.registerCommand("view.telemetry", [this] { show_.telemetry = !show_.telemetry; });
    k.registerCommand("view.settings", [this] { show_.settings = true; settingsPanel_.focusTab(SettingsTab::General); });
    k.registerCommand("view.keybindings", [this] { show_.keybindings = true; });
    k.registerCommand("view.extensions", [this] { show_.settings = true; settingsPanel_.focusTab(SettingsTab::Network); });
    k.registerCommand("view.debug", [this] {
        show_.callStack = show_.variables = show_.watch = show_.console = true;
    });
    k.registerCommand("view.resetLayout", [this] { layoutBuilt_ = false; });
    k.registerCommand("theme.next", [this] { themes_.next(); });
    k.registerCommand("language.toggle", [this] {
        auto& L = Localization::instance();
        settings_.language = L.language() == "ru-RU" ? "en-US" : "ru-RU";
        L.setLanguage(settings_.language);
        Workspace::saveSettings(settings_);
    });

    k.registerCommand("build.build", [this] { themes_.onBuildStarted(); show_.output = true; build_.build(); });
    k.registerCommand("build.run", [this] { show_.output = true; build_.run(workspace_.state.runArgs); });
    k.registerCommand("build.clean", [this] { build_.clean(); });
    k.registerCommand("build.test", [this] { themes_.onBuildStarted(); show_.output = true; build_.test(); });
    k.registerCommand("build.cancel", [this] { build_.cancel(); });

    k.registerCommand("debug.start", [this] { startDebugging(); });
    k.registerCommand("debug.stop", [this] { dap_.stop(); editor_.clearExecutionLine(); });
    k.registerCommand("debug.restart", [this] { dap_.restart(); });
    k.registerCommand("debug.pause", [this] { dap_.pause(); });
    k.registerCommand("debug.stepOver", [this] { dap_.next(); });
    k.registerCommand("debug.stepInto", [this] { dap_.stepIn(); });
    k.registerCommand("debug.stepOut", [this] { dap_.stepOut(); });

    for (const char* id : {"editor.undo", "editor.redo", "editor.copy", "editor.cut", "editor.paste", "editor.selectAll",
                           "editor.find", "editor.addNextOccurrence", "editor.addCursorAbove", "editor.addCursorBelow",
                           "editor.toggleComment", "editor.duplicateLine", "editor.toggleFold", "editor.foldAll",
                           "editor.unfoldAll", "editor.triggerCompletion", "editor.goToDefinition", "editor.toggleBreakpoint"}) {
        std::string cmd = id;
        k.registerCommand(cmd, [this, cmd] { editor_.executeCommand(cmd); }, "editor");
    }
    k.registerCommand("editor.zoomIn", [this] { editor_.executeCommand("editor.zoomIn"); });
    k.registerCommand("editor.zoomOut", [this] { editor_.executeCommand("editor.zoomOut"); });

    k.registerCommand("git.refresh", [this] { git_.refresh(); });
    k.registerCommand("git.pull", [this] { git_.pull(); });
    k.registerCommand("git.push", [this] { git_.push(); });
    k.registerCommand("help.about", [this] { show_.about = true; });
}

// ===========================================================================
//  Действия
// ===========================================================================
void Application::openPath(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) { openFolder(p); return; }
    std::string err;
    if (!editor_.openFile(p, &err)) log("[editor] " + err);
}

void Application::openFolder(const fs::path& p) {
    std::error_code ec;
    fs::path root = fs::weakly_canonical(p, ec);
    if (workspace_.hasFolder()) saveState();
    editor_.closeAll();
    lsp_.shutdownAll();

    std::string err;
    if (!workspace_.openFolder(root, &err)) { log("[workspace] " + err); return; }
    explorer_.setRoot(root);
    git_.setRepository(root);
    build_.setRoot(root);
    lsp_.setRoot(root);
    applyLanguageServers();
    fs::current_path(root, ec);
    glfwSetWindowTitle(window_, ("HybridIDE — " + root.filename().string()).c_str());

    // Восстановление открытых файлов, точек останова и свёрток
    for (auto& f : workspace_.state.openFiles) {
        if (Document* d = editor_.openFile(fs::u8path(f.path))) {
            for (int b : f.breakpoints) { d->breakpoints.insert(b); dap_.setBreakpoint(d->path, b, true); }
            for (int fl : f.folded) d->folded.insert(fl);
            editor_.goTo(d->path, f.line, f.col);
        }
    }
    if (!workspace_.state.activeFile.empty()) {
        if (Document* d = editor_.findByPath(fs::u8path(workspace_.state.activeFile))) d->focusRequested = true;
    }

    // «Недавние» — максимум 12 без дублей
    auto& r = settings_.recentFolders;
    r.erase(std::remove(r.begin(), r.end(), root.string()), r.end());
    r.insert(r.begin(), root.string());
    if (r.size() > 12) r.resize(12);
    Workspace::saveSettings(settings_);

    if (!terminalStarted_) { terminal_.newSession(root); terminalStarted_ = true; }
    log("[workspace] " + root.string() + " (" + BuildSystem::kindName(build_.kind()) + ")");
}

void Application::applyLanguageServers() {
    for (auto& [lang, cmd] : workspace_.state.lspOverrides.items())
        if (cmd.is_string()) lsp_.setOverride(lang, cmd.get<std::string>());
}

LaunchConfig Application::defaultLaunchConfig() const {
    LaunchConfig cfg;
    const fs::path root = workspace_.root();
    const std::string name = root.filename().string();
    cfg.cwd = root.string();
    cfg.args = splitCommandLine(workspace_.state.runArgs);
#ifdef _WIN32
    const std::string exe = ".exe";
#else
    const std::string exe;
#endif
    auto nativeAdapter = [] {
        if (Process::findExecutable("lldb-dap")) return std::string("lldb-dap");
        if (Process::findExecutable("lldb-vscode")) return std::string("lldb-vscode");
        return std::string("gdb -i dap");          // GDB >= 14
    };
    switch (build_.kind()) {
    case ProjectKind::CMake:
    case ProjectKind::Make: {
        cfg.adapter = nativeAdapter();
        // Ищем исполняемый файл с именем проекта в типичных каталогах сборки.
        for (const char* sub : {"build", "build/Debug", "build/debug", "out/build", "."}) {
            fs::path cand = root / sub / (name + exe);
            std::error_code ec;
            if (fs::exists(cand, ec)) { cfg.program = cand.string(); break; }
        }
        if (cfg.program.empty()) cfg.program = (root / "build" / (name + exe)).string();
        break;
    }
    case ProjectKind::Cargo:
        cfg.adapter = nativeAdapter();
        cfg.program = (root / "target" / "debug" / (name + exe)).string();
        break;
    case ProjectKind::Python: {
#ifdef _WIN32
        cfg.adapter = "python -m debugpy.adapter";
#else
        cfg.adapter = "python3 -m debugpy.adapter";
#endif
        Document* d = editor_.active();
        cfg.program = d && d->languageId() == "python" ? d->path.string() : (root / "main.py").string();
        cfg.extra = {{"type", "python"}, {"console", "internalConsole"}, {"justMyCode", true}};
        break;
    }
    case ProjectKind::Go:
        cfg.adapter = "dlv dap";
        cfg.program = root.string();
        cfg.extra = {{"mode", "debug"}};
        break;
    case ProjectKind::Node:
        cfg.adapter = "js-debug-adapter";
        cfg.program = (root / "index.js").string();
        cfg.extra = {{"type", "pwa-node"}};
        break;
    default:
        cfg.adapter = nativeAdapter();
        break;
    }
    return cfg;
}

void Application::startDebugging() {
    if (dap_.state() == DebugState::Stopped) { dap_.continue_(); return; }
    if (dap_.state() == DebugState::Running || dap_.state() == DebugState::Initializing) return;
    if (!workspace_.hasFolder()) { log("[debug] open a folder first"); return; }

    LaunchConfig cfg = defaultLaunchConfig();
    // Пользовательская конфигурация (аналог launch.json) перекрывает автоопределение.
    const auto& dc = workspace_.state.debugConfig;
    if (dc.is_object() && !dc.empty()) {
        cfg.adapter = dc.value("adapter", cfg.adapter);
        cfg.request = dc.value("request", cfg.request);
        cfg.program = dc.value("program", cfg.program);
        cfg.cwd = dc.value("cwd", cfg.cwd);
        cfg.stopOnEntry = dc.value("stopOnEntry", cfg.stopOnEntry);
        if (dc.contains("args") && dc["args"].is_array()) cfg.args = dc["args"].get<std::vector<std::string>>();
        if (dc.contains("extra") && dc["extra"].is_object()) cfg.extra.update(dc["extra"]);
    }
    // Точки останова из всех открытых документов
    for (auto& d : editor_.documents())
        if (!d->path.empty()) for (int b : d->breakpoints) dap_.setBreakpoint(d->path, b, true);

    std::string err;
    show_.console = show_.callStack = show_.variables = true;
    if (!dap_.start(cfg, workspace_.root(), &err)) log("[debug] " + cfg.adapter + ": " + err);
}

void Application::onExtensionInstalled(const ExtensionManifest& m, const fs::path& dir) {
    // Обрабатываем только декларативные типы — код расширений не исполняется.
    try {
        if (m.type == "theme" && m.raw.contains("theme")) {
            auto j = nlohmann::json::parse(readFile(dir / m.raw["theme"].get<std::string>()));
            themes_.fromJson(j);
            themes_.apply(themes_.currentName());
            if (themes_.onSettingsChanged) themes_.onSettingsChanged();
        } else if (m.type == "keymap" && m.raw.contains("keymap")) {
            keys_.loadUserOverrides(dir / m.raw["keymap"].get<std::string>());
            keys_.saveUserOverrides(Workspace::userConfigDir() / "keybindings.json");
        } else if (m.type == "toolchain" && m.raw.contains("toolchain")) {
            const auto& t = m.raw["toolchain"];
            ToolchainSpec s;
            s.id = t.value("id", m.name);
            s.displayName = t.value("displayName", m.name);
            s.repo = t.value("repo", m.repo);
            s.assetWindows = t.value("assetWindows", "");
            s.assetLinux = t.value("assetLinux", "");
            s.assetMac = t.value("assetMac", "");
            s.binSubdir = t.value("binSubdir", "bin");
            s.setupScriptWindows = t.value("setupScriptWindows", "");
            s.setupScriptPosix = t.value("setupScriptPosix", "");
            if (t.contains("env") && t["env"].is_object()) s.env = t["env"].get<std::map<std::string, std::string>>();
            toolchains_.addSpec(s);
            toolchains_.install(s.id);
        } else if (m.type == "template") {
            templates_.loadUserTemplates(dir);
        }
        log("[extensions] " + m.name + " " + m.version + " (" + m.type + ") -> " + dir.string());
    } catch (const std::exception& e) {
        log("[extensions] " + m.name + ": " + e.what());
    }
}

std::vector<std::pair<std::string, std::string>> Application::collectBackupFiles() const {
    std::vector<std::pair<std::string, std::string>> files;
    std::error_code ec;
    const fs::path cfg = Workspace::userConfigDir();
    // Секреты (токен) намеренно НЕ попадают в бэкап.
    for (const char* f : {"settings.json", "keybindings.json"})
        if (fs::exists(cfg / f, ec)) files.emplace_back(f, readFile(cfg / f));
    if (fs::exists(toolchains_.installRoot() / "toolchains.json", ec))
        files.emplace_back("toolchains.json", readFile(toolchains_.installRoot() / "toolchains.json"));
    if (workspace_.hasFolder()) {
        auto wf = Workspace::workspaceFile(workspace_.root());
        if (fs::exists(wf, ec))
            files.emplace_back("workspaces/" + workspace_.root().filename().string() + ".ide_workspace.json", readFile(wf));
    }
    return files;
}

void Application::saveState() {
    settings_.theme = themes_.currentName();
    settings_.themes = themes_.toJson();
    settings_.audio["master"] = audio_.masterVolume.load();
    settings_.language = Localization::instance().language();
    settings_.keymap = keys_.preset();
    Workspace::saveSettings(settings_);
    keys_.saveUserOverrides(Workspace::userConfigDir() / "keybindings.json");

    if (workspace_.hasFolder()) {
        auto& st = workspace_.state;
        st.openFiles.clear();
        for (auto& d : editor_.documents()) {
            if (d->path.empty()) continue;
            OpenFileState f;
            f.path = d->path.u8string();
            f.line = d->cursors.front().pos.line;
            f.col = d->cursors.front().pos.col;
            f.breakpoints.assign(d->breakpoints.begin(), d->breakpoints.end());
            f.folded.assign(d->folded.begin(), d->folded.end());
            st.openFiles.push_back(std::move(f));
        }
        if (Document* a = editor_.active(); a && !a->path.empty()) st.activeFile = a->path.u8string();
        std::string err;
        if (!workspace_.save(&err)) std::fprintf(stderr, "[workspace] %s\n", err.c_str());
    }
}

void Application::requestQuit() {
    bool dirty = false;
    for (auto& d : editor_.documents()) dirty |= d->buffer.dirty();
    if (dirty) quitConfirm_ = true;
    else running_ = false;
}

// ===========================================================================
//  Главный цикл
// ===========================================================================
int Application::run() {
    Workspace::loadSettings(settings_);
    if (!initWindow()) { std::fprintf(stderr, "Failed to create GLFW window / OpenGL 3.3 context\n"); return 1; }
    initImGui();        // контекст ImGui нужен темам (стили) ещё до шрифтов
    initServices();
    rebuildFonts();
    wireCallbacks();
    registerCommands();

    if (!opts_.openPath.empty()) openPath(opts_.openPath);
    else if (!settings_.recentFolders.empty()) {
        std::error_code ec;
        if (fs::is_directory(fs::u8path(settings_.recentFolders.front()), ec)) openFolder(fs::u8path(settings_.recentFolders.front()));
    }

    lastTime_ = glfwGetTime();
    while (running_) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) {     // свёрнуто — не тратим GPU
            MainThreadDispatcher::instance().drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        frame();
    }
    saveState();
    return 0;
}

void Application::frame() {
    MainThreadDispatcher::instance().drain();   // результаты фоновых задач (LSP, git, http ...)

    if (fontsDirty_) {
        rebuildFonts();
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
        fontsDirty_ = false;
    }

    const double now = glfwGetTime();
    const float dt = std::clamp(float(now - lastTime_), 1e-4f, 0.1f);
    lastTime_ = now;
    time_ += dt;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    keys_.process();                 // горячие клавиши — до отрисовки панелей
    renderMenuBar();
    renderStatusBar();
    renderDockspace();
    renderPanels();
    keys_.renderPalette();
    fileDialog_.render();
    renderQuitConfirm();
    detectFocusChange();

    // Анимации темы: телеметрия влияет на шестерни и манометры
    ThemeFrameInput in;
    in.dt = dt;
    in.time = time_;
    auto s = telemetry_->last();
    in.cpuPercent = s.cpuPercent;
    in.ramPercent = s.ramTotalBytes ? 100.f * float(s.ramUsedBytes) / float(s.ramTotalBytes) : 0.f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    in.viewportPos = vp->Pos;
    in.viewportSize = vp->Size;
    themes_.update(in);
    editor_.palette = themes_.editorPalette();
    terminal_.background = editor_.palette.background;

    composeAndPresent();
}

void Application::composeAndPresent() {
    ImGui::Render();
    int fbW = 0, fbH = 0, winW = 0, winH = 0;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    glfwGetWindowSize(window_, &winW, &winH);

    ShaderUniforms u;
    themes_.fillUniforms(u);
    const bool fx = shaders_.ready() && fbW > 0 && fbH > 0;
    if (fx) {
        const std::vector<FishVertex>* fv = nullptr;
        if (themes_.wantsFish()) {
            fishVerts_.clear();
            fish_.buildVertices(fishVerts_, time_);
            fv = &fishVerts_;
        }
        // Фон рисуется в offscreen-буфер; ImGui рисуется поверх в тот же буфер,
        // затем пост-эффект (CRT/неон/сепия) выводит результат на экран.
        shaders_.beginScene(fbW, fbH, u, themes_.current() == ThemeId::Aquarium ? &wave_ : nullptr, fv, (float)winW, (float)winH);
    } else {
        gl::Viewport(0, 0, fbW, fbH);
        gl::ClearColor(u.clear[0], u.clear[1], u.clear[2], 1.f);
        gl::Clear(gl::COLOR_BUFFER_BIT);
    }
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (fx) shaders_.endSceneToScreen(u);

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }
    glfwSwapBuffers(window_);
}

void Application::detectFocusChange() {
    ImGuiContext* g = ImGui::GetCurrentContext();
    std::string name = (g->NavWindow && g->NavWindow->RootWindow) ? g->NavWindow->RootWindow->Name : "";
    editorFocused_ = name.find("###doc") != std::string::npos;
    if (name != lastFocusedWindow_) {
        if (!lastFocusedWindow_.empty() && !name.empty()) themes_.onFocusChanged();   // глитч Cyberpunk
        lastFocusedWindow_ = name;
    }
}

// ---------------------------------------------------------------------------
//  Меню
// ---------------------------------------------------------------------------
void Application::renderMenuBar() {
    auto& L = Localization::instance();
    if (!ImGui::BeginMainMenuBar()) return;
    auto item = [&](const char* cmd, bool enabled = true) {
        std::string label = L.tr(std::string("cmd.") + cmd);
        if (ImGui::MenuItem(label.c_str(), shortcut(cmd), false, enabled)) keys_.execute(cmd);
    };
    auto toggle = [&](const char* key, bool* v) { ImGui::MenuItem(L.tr(key), nullptr, v); };

    if (ImGui::BeginMenu(L.tr("menu.file"))) {
        item("file.new"); item("file.open"); item("file.openFolder"); item("project.new");
        if (ImGui::BeginMenu(L.tr("menu.recent"), !settings_.recentFolders.empty())) {
            std::string pick;
            for (auto& r : settings_.recentFolders) if (ImGui::MenuItem(r.c_str())) pick = r;
            ImGui::EndMenu();
            if (!pick.empty()) openFolder(fs::u8path(pick));
        }
        ImGui::Separator();
        item("file.save", editor_.active() != nullptr); item("file.saveAll"); item("file.close", editor_.active() != nullptr);
        ImGui::Separator();
        item("app.quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L.tr("menu.edit"))) {
        const bool e = editor_.active() != nullptr;
        item("editor.undo", e); item("editor.redo", e); ImGui::Separator();
        item("editor.cut", e); item("editor.copy", e); item("editor.paste", e); item("editor.selectAll", e);
        ImGui::Separator();
        item("editor.find", e); item("editor.addNextOccurrence", e); item("editor.toggleComment", e);
        item("editor.duplicateLine", e); item("editor.goToDefinition", e);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L.tr("menu.view"))) {
        item("view.commandPalette");
        ImGui::Separator();
        toggle("window.explorer", &show_.explorer); toggle("window.git", &show_.git);
        toggle("window.terminal", &show_.terminal); toggle("window.output", &show_.output);
        toggle("window.problems", &show_.problems); toggle("window.telemetry", &show_.telemetry);
        toggle("window.properties", &show_.properties); toggle("theme.gauges_window", &show_.gauges);
        if (ImGui::BeginMenu(L.tr("menu.debug_windows"))) {
            toggle("window.callstack", &show_.callStack); toggle("window.variables", &show_.variables);
            toggle("window.watch", &show_.watch); toggle("window.breakpoints", &show_.breakpoints);
            toggle("window.memory", &show_.memory); toggle("window.console", &show_.console);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(L.tr("menu.theme"))) {
            for (auto& n : ThemeManager::names()) {
                if (ImGui::MenuItem(L.tr("theme.name." + n), nullptr, n == themes_.currentName())) {
                    themes_.apply(n);
                    if (themes_.onSettingsChanged) themes_.onSettingsChanged();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(L.tr("menu.language"))) {
            for (auto& lang : L.availableLanguages()) {
                if (ImGui::MenuItem(Localization::languageDisplayName(lang), nullptr, lang == L.language())) {
                    L.setLanguage(lang);
                    settings_.language = lang;
                    Workspace::saveSettings(settings_);
                }
            }
            ImGui::EndMenu();
        }
        item("view.resetLayout");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L.tr("menu.build_menu"))) {
        const bool has = workspace_.hasFolder();
        item("build.build", has && !build_.busy()); item("build.run", has && !build_.busy());
        item("build.test", has && !build_.busy()); item("build.clean", has && !build_.busy());
        item("build.cancel", build_.busy());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L.tr("menu.debug"))) {
        const auto st = dap_.state();
        const bool active = st == DebugState::Running || st == DebugState::Stopped || st == DebugState::Initializing;
        item("debug.start"); item("debug.stop", active); item("debug.restart", active);
        item("debug.pause", st == DebugState::Running);
        ImGui::Separator();
        item("debug.stepOver", st == DebugState::Stopped); item("debug.stepInto", st == DebugState::Stopped);
        item("debug.stepOut", st == DebugState::Stopped);
        ImGui::Separator();
        item("editor.toggleBreakpoint", editor_.active() != nullptr);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Git")) {
        item("git.refresh", git_.isRepo()); item("git.pull", git_.isRepo()); item("git.push", git_.isRepo());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L.tr("menu.tools"))) {
        item("view.settings"); item("view.keybindings"); item("view.extensions"); item("view.telemetry");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L.tr("menu.help"))) {
        item("help.about");
        toggle("menu.imgui_demo", &show_.imguiDemo);
        toggle("menu.implot_demo", &show_.implotDemo);
        ImGui::EndMenu();
    }
    // Справа: текущая тема (клик — следующая)
    std::string themeLabel = std::string(L.tr("theme.name." + std::string(themes_.currentName())));
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(themeLabel.c_str()).x - 24);
    if (ImGui::SmallButton(themeLabel.c_str())) keys_.execute("theme.next");
    ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------------------
//  Док-пространство и раскладка по умолчанию
// ---------------------------------------------------------------------------
void Application::buildDefaultLayout(ImGuiID dock) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, (ImGuiDockNodeFlags)ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dock;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.35f, nullptr, &left);

    ImGui::DockBuilderDockWindow("###Explorer", left);
    ImGui::DockBuilderDockWindow("###Git", left);
    ImGui::DockBuilderDockWindow("###Properties", leftBottom);
    ImGui::DockBuilderDockWindow("###Gauges", leftBottom);
    ImGui::DockBuilderDockWindow("###Terminal", bottom);
    ImGui::DockBuilderDockWindow("###Output", bottom);
    ImGui::DockBuilderDockWindow("###Problems", bottom);
    ImGui::DockBuilderDockWindow("###DebugConsole", bottom);
    ImGui::DockBuilderDockWindow("###Variables", right);
    ImGui::DockBuilderDockWindow("###Watch", right);
    ImGui::DockBuilderDockWindow("###Memory", right);
    ImGui::DockBuilderDockWindow("###CallStack", rightBottom);
    ImGui::DockBuilderDockWindow("###Breakpoints", rightBottom);
    ImGui::DockBuilderDockWindow("###Telemetry", rightBottom);
    ImGui::DockBuilderDockWindow("###Welcome", center);
    ImGui::DockBuilderFinish(dock);
}

void Application::renderDockspace() {
    mainDockId_ = ImGui::DockSpaceOverViewport(ImGui::GetID("MainDock"), ImGui::GetMainViewport(),
                                               ImGuiDockNodeFlags_PassthruCentralNode);
    if (!layoutBuilt_) { buildDefaultLayout(mainDockId_); layoutBuilt_ = true; }
    if (ImGuiDockNode* c = ImGui::DockBuilderGetCentralNode(mainDockId_)) editorDockId_ = c->ID;
}

// ---------------------------------------------------------------------------
//  Панели
// ---------------------------------------------------------------------------
void Application::renderPanels() {
    auto& L = Localization::instance();

    if (editor_.documents().empty()) renderWelcome();
    editor_.render(editorDockId_);

    if (show_.explorer) explorer_.render(L.tr("window.explorer"), &show_.explorer);
    if (show_.properties) explorer_.renderProperties(L.tr("window.properties"), &show_.properties);
    if (show_.git) git_.render(L.tr("window.git"), &show_.git);
    git_.renderDiffWindow(L.tr("window.diff"));
    if (show_.terminal) terminal_.render(L.tr("window.terminal"), &show_.terminal);
    if (show_.output) build_.renderOutput(L.tr("window.output"), &show_.output);
    if (show_.problems) build_.renderProblems(L.tr("window.problems"), &show_.problems);

    if (show_.callStack) dap_.renderCallStack(L.tr("window.callstack"), &show_.callStack);
    if (show_.variables) dap_.renderVariables(L.tr("window.variables"), &show_.variables);
    if (show_.watch) dap_.renderWatch(L.tr("window.watch"), &show_.watch);
    if (show_.breakpoints) dap_.renderBreakpoints(L.tr("window.breakpoints"), &show_.breakpoints);
    if (show_.memory) dap_.renderMemory(L.tr("window.memory"), &show_.memory);
    if (show_.console) dap_.renderConsole(L.tr("window.console"), &show_.console);

    if (show_.telemetry) telemetry_->render(&show_.telemetry);
    if (show_.gauges) {
        auto s = telemetry_->last();
        themes_.renderGaugesWindow(&show_.gauges, s.cpuPercent,
                                   s.ramTotalBytes ? 100.f * float(s.ramUsedBytes) / float(s.ramTotalBytes) : 0.f);
    }
    if (show_.settings) settingsPanel_.render(&show_.settings, panelCtx_);
    if (show_.keybindings)
        keys_.renderEditor(&show_.keybindings, resourceDir_, Workspace::userConfigDir() / "keybindings.json");
    if (show_.wizard) templates_.renderWizard(&show_.wizard, [this](const fs::path& p) { openFolder(p); });
    renderAbout(&show_.about);
    if (show_.imguiDemo) ImGui::ShowDemoWindow(&show_.imguiDemo);
    if (show_.implotDemo) ImPlot::ShowDemoWindow(&show_.implotDemo);
}

void Application::renderWelcome() {
    auto& L = Localization::instance();
    ImGui::SetNextWindowDockID(editorDockId_, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);   // фон темы виден целиком
    if (!ImGui::Begin(L.tr("window.welcome"), nullptr, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    ImGui::Dummy(ImVec2(0, 30));
    ImGui::SetWindowFontScale(1.8f);
    ImGui::TextUnformatted("HybridIDE");
    ImGui::SetWindowFontScale(1.f);
    ImGui::TextDisabled("%s", L.tr("welcome.subtitle"));
    ImGui::Dummy(ImVec2(0, 16));
    auto btn = [&](const char* cmd) {
        std::string label = L.tr(std::string("cmd.") + cmd);
        if (const char* sc = shortcut(cmd)) label += "   (" + std::string(sc) + ")";
        if (ImGui::Button(label.c_str(), ImVec2(360, 0))) keys_.execute(cmd);
    };
    btn("file.openFolder"); btn("project.new"); btn("file.new"); btn("view.commandPalette"); btn("view.settings");
    if (!settings_.recentFolders.empty()) {
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::SeparatorText(L.tr("menu.recent"));
        std::string pick;
        for (auto& r : settings_.recentFolders) if (ImGui::Selectable(r.c_str())) pick = r;
        if (!pick.empty()) openFolder(fs::u8path(pick));
    }
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::SeparatorText(L.tr("theme.select"));
    for (auto& n : ThemeManager::names()) {
        if (ImGui::Button(L.tr("theme.name." + n))) { themes_.apply(n); if (themes_.onSettingsChanged) themes_.onSettingsChanged(); }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::End();
}

void Application::renderStatusBar() {
    auto& L = Localization::instance();
    const float h = ImGui::GetFrameHeight();
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (!ImGui::BeginViewportSideBar("##statusBar", ImGui::GetMainViewport(), ImGuiDir_Down, h, fl)) { ImGui::End(); return; }
    if (ImGui::BeginMenuBar()) {
        themes_.renderStatusBarDecor(h * 0.8f);
        ImGui::SameLine();
        if (git_.isRepo()) { ImGui::Text("  %s", git_.branch().c_str()); ImGui::SameLine(); }
        if (build_.busy()) {
            ImGui::ProgressBar(build_.progress(), ImVec2(120, h * 0.6f), L.tr("status.building"));
            ImGui::SameLine();
        } else if (workspace_.hasFolder()) {
            ImGui::TextDisabled("%s", BuildSystem::kindName(build_.kind()));
            ImGui::SameLine();
        }
        for (auto& [name, ok] : lsp_.status()) {
            ImGui::TextColored(ok ? ImVec4(0.4f, 0.9f, 0.5f, 1) : ImVec4(0.9f, 0.6f, 0.3f, 1), "● %s", name.c_str());
            ImGui::SameLine();
        }
        switch (dap_.state()) {
        case DebugState::Running: ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "%s", L.tr("status.debug_running")); ImGui::SameLine(); break;
        case DebugState::Stopped: ImGui::TextColored(ImVec4(1, 0.9f, 0.2f, 1), "%s", L.tr("status.debug_paused")); ImGui::SameLine(); break;
        default: break;
        }
        if (auto hint = keys_.pendingHint(); !hint.empty()) { ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "%s", hint.c_str()); ImGui::SameLine(); }

        // Правая часть
        char right[256];
        std::string pos;
        if (Document* d = editor_.active()) {
            auto c = d->cursors.front().pos;
            char b[96];
            std::snprintf(b, sizeof b, "%s %d, %s %d  |  %s  |  ", L.tr("status.line"), c.line + 1, L.tr("status.col"), c.col + 1,
                          d->languageId().c_str());
            pos = b;
        }
        auto s = telemetry_->last();
        std::snprintf(right, sizeof right, "%sCPU %2.0f%%  RAM %2.0f%%  |  %s", pos.c_str(), s.cpuPercent,
                      s.ramTotalBytes ? 100.0 * double(s.ramUsedBytes) / double(s.ramTotalBytes) : 0.0, L.language().c_str());
        float w = ImGui::CalcTextSize(right).x + 16;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8, ImGui::GetWindowWidth() - w));
        ImGui::TextUnformatted(right);
        if (ImGui::IsItemClicked()) show_.telemetry = !show_.telemetry;
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

void Application::renderQuitConfirm() {
    auto& L = Localization::instance();
    if (quitConfirm_) { ImGui::OpenPopup("###quitConfirm"); quitConfirm_ = false; }
    std::string title = std::string(L.tr("quit.title")) + "###quitConfirm";
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(L.tr("quit.message"));
        for (auto& d : editor_.documents()) if (d->buffer.dirty()) ImGui::BulletText("%s", d->title.c_str());
        if (ImGui::Button(L.tr("quit.save_all"))) { editor_.saveAll(); running_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(L.tr("quit.discard"))) { running_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(L.tr("common.cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace ide
