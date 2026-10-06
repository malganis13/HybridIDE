// =============================================================================
//  Workspace.hpp — сохранение сессии проекта (.ide_workspace.json) и
//  глобальных пользовательских настроек (settings.json в каталоге профиля).
// =============================================================================
#pragma once
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ide {

struct OpenFileState {
    std::string path;
    int         line = 0, col = 0;
    std::vector<int> breakpoints;
    std::vector<int> folded;
};

// Состояние конкретного проекта — хранится в корне папки проекта
struct WorkspaceState {
    std::vector<OpenFileState> openFiles;
    std::string                activeFile;
    std::string                buildConfig = "Debug";
    std::string                runArgs;
    std::string                customBuildCommand, customRunCommand;
    nlohmann::json             lspOverrides = nlohmann::json::object();   // {"cpp": "clangd --header-insertion=never"}
    nlohmann::json             debugConfig  = nlohmann::json::object();   // аналог launch.json
};

// Глобальные настройки пользователя
struct UserSettings {
    std::string                language = "ru-RU";
    std::string                theme    = "Aquarium";
    std::string                keymap   = "vscode";
    float                      fontSize = 16.f;
    float                      uiScale  = 1.f;
    bool                       vsync    = true;
    std::vector<std::string>   recentFolders;
    nlohmann::json             themes = nlohmann::json::object();          // параметры каждой темы
    nlohmann::json             audio  = nlohmann::json::object();
    std::string                githubBackupRepo = "hybrid-ide-backup";
    std::vector<std::string>   extensionSources;   // "owner/repo" для поиска расширений
};

class Workspace {
public:
    static std::filesystem::path userConfigDir();          // %APPDATA%/HybridIDE или ~/.config/HybridIDE
    static std::filesystem::path workspaceFile(const std::filesystem::path& root) { return root / ".ide_workspace.json"; }

    bool openFolder(const std::filesystem::path& root, std::string* err = nullptr);
    bool save(std::string* err = nullptr) const;
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] bool hasFolder() const { return !root_.empty(); }

    WorkspaceState state;

    static bool loadSettings(UserSettings& s);
    static bool saveSettings(const UserSettings& s);

    // Секрет (GitHub PAT) хранится отдельно от настроек, с правами 0600,
    // и никогда не попадает в резервные копии рабочего пространства.
    static std::optional<std::string> loadSecret(const std::string& name);
    static bool saveSecret(const std::string& name, const std::string& value);
    static bool deleteSecret(const std::string& name);

private:
    std::filesystem::path root_;
};

void to_json(nlohmann::json& j, const OpenFileState& s);
void from_json(const nlohmann::json& j, OpenFileState& s);

} // namespace ide
