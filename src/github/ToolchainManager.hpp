// =============================================================================
//  ToolchainManager.hpp — установка портативных тулчейнов из GitHub Releases
//  (Clang/LLVM, MinGW-w64, Portable MSVC, CMake, Ninja, clangd, rust-analyzer,
//  Python standalone) и выполнение скриптов установки (PowerShell/Bash/Lua).
//  После установки пути bin/ регистрируются в PATH текущего процесса без
//  перезапуска IDE и сохраняются в toolchains.json.
// =============================================================================
#pragma once
#include "core/Process.hpp"
#include "github/GitHubManager.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace ide {

struct ToolchainSpec {
    std::string id, displayName, repo;        // repo: "owner/name"
    std::string assetWindows, assetLinux, assetMac;  // regex по имени ассета
    std::string binSubdir = "bin";             // относительный путь к исполняемым файлам
    std::string setupScriptWindows, setupScriptPosix;   // опциональные скрипты из репозитория
    std::map<std::string, std::string> env;    // доп. переменные: {"CC": "{root}/bin/clang"}
};

struct InstalledToolchain {
    std::string id, version;
    std::filesystem::path root, bin;
    std::map<std::string, std::string> env;
};

enum class InstallStage { Idle, Resolving, Downloading, Extracting, RunningScript, Registering, Done, Failed };

struct InstallJob {
    std::string  id;
    InstallStage stage = InstallStage::Idle;
    float        progress = 0.f;
    std::string  message;
};

class ToolchainManager {
public:
    explicit ToolchainManager(GitHubManager& gh);

    [[nodiscard]] const std::vector<ToolchainSpec>& catalog() const { return catalog_; }
    [[nodiscard]] const std::vector<InstalledToolchain>& installed() const { return installed_; }
    [[nodiscard]] const std::map<std::string, InstallJob>& jobs() const { return jobs_; }

    void addSpec(ToolchainSpec s) { catalog_.push_back(std::move(s)); }
    void install(const std::string& id, const std::string& tag = {});   // пустой tag — последний стабильный релиз
    void uninstall(const std::string& id);
    void loadRegistry();                     // toolchains.json -> PATH
    void saveRegistry() const;

    // Выполнить скрипт установки (вызывается только после подтверждения пользователем в UI)
    static ProcessResult runSetupScript(const std::filesystem::path& script, const std::filesystem::path& cwd,
                                        const std::map<std::string, std::string>& env);
    static bool extractArchive(const std::filesystem::path& archive, const std::filesystem::path& dest, std::string* err);
    static void prependToPath(const std::filesystem::path& dir);
    static void setEnv(const std::string& k, const std::string& v);
    static std::string currentPlatform();    // "windows" | "linux" | "mac"

    std::filesystem::path installRoot() const;
    std::function<void(const std::string&)> log;

private:
    void registerToolchain(const InstalledToolchain& t);
    GitHubManager&                    gh_;
    std::vector<ToolchainSpec>        catalog_;
    std::vector<InstalledToolchain>   installed_;
    std::map<std::string, InstallJob> jobs_;
};

} // namespace ide
