// =============================================================================
//  GitHubManager.hpp — клиент GitHub REST API (https://api.github.com):
//   • авторизация по Personal Access Token (Authorization: Bearer <PAT>)
//   • поиск расширений/тем/скриптов тулчейнов в публичных и приватных репо
//   • чтение релизов и скачивание Release Assets
//   • создание приватного репозитория (POST /user/repos) и резервное
//     копирование настроек, тем, раскладок клавиш (PUT /contents)
//  Все запросы выполняются в пуле потоков; ответы — в UI-потоке.
// =============================================================================
#pragma once
#include "github/HttpClient.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ide {

struct GitHubUser    { std::string login, name, avatarUrl; std::vector<std::string> scopes; };
struct GitHubAsset   { long long id = 0; std::string name, contentType, browserUrl, apiUrl; long long size = 0; };
struct GitHubRelease { long long id = 0; std::string tag, name, publishedAt; bool prerelease = false; std::vector<GitHubAsset> assets; };
struct GitHubRepo    { std::string fullName, description, defaultBranch, htmlUrl; bool isPrivate = false; int stars = 0; std::vector<std::string> topics; };

// Манифест расширения: файл hybridide-extension.json в корне репозитория
struct ExtensionManifest {
    std::string repo, name, version, type;   // type: theme | keymap | toolchain | syntax | plugin
    std::string description;
    nlohmann::json raw;
};

class GitHubManager {
public:
    static constexpr const char* kApi = "https://api.github.com";

    void setToken(std::string token);
    [[nodiscard]] bool hasToken() const { std::lock_guard lk(mtx_); return !token_.empty(); }
    [[nodiscard]] bool authenticated() const { return authenticated_; }
    [[nodiscard]] const GitHubUser& user() const { return user_; }

    // --- Асинхронные операции ---
    void validateToken(std::function<void(bool, std::string)> done);                        // GET /user
    void listUserRepos(std::function<void(std::vector<GitHubRepo>, std::string)> done);   // GET /user/repos
    void searchExtensions(const std::string& query, std::function<void(std::vector<GitHubRepo>, std::string)> done);
    void fetchManifest(const std::string& fullName, std::function<void(std::optional<ExtensionManifest>, std::string)> done);
    void listReleases(const std::string& fullName, std::function<void(std::vector<GitHubRelease>, std::string)> done);
    void downloadAsset(const GitHubAsset& a, const std::filesystem::path& dest,
                       std::function<void(float)> progress, std::function<void(bool, std::string)> done);
    void createRepo(const std::string& name, bool isPrivate, const std::string& description,
                    std::function<void(bool, std::string)> done);                           // POST /user/repos
    void putFile(const std::string& fullName, const std::string& path, const std::string& content,
                 const std::string& message, std::function<void(bool, std::string)> done);  // PUT /repos/{o}/{r}/contents/{path}
    void getFile(const std::string& fullName, const std::string& path, std::function<void(std::optional<std::string>, std::string)> done);

    // Полный бэкап: создаёт репозиторий при необходимости и загружает файлы
    void backupWorkspace(const std::string& repoName, const std::vector<std::pair<std::string, std::string>>& files,
                         std::function<void(bool, std::string)> done);

    [[nodiscard]] int rateLimitRemaining() const { return rateRemaining_; }

    // Синхронные примитивы (используются из фоновых потоков)
    HttpResponse api(const std::string& method, const std::string& path, const nlohmann::json* body = nullptr);
    static std::string base64Encode(std::string_view in);
    static std::string base64Decode(std::string_view in);

private:
    std::vector<std::string> headers(bool binary = false) const;

    mutable std::mutex mtx_;
    std::string        token_;
    std::atomic<bool>  authenticated_{false};
    GitHubUser         user_;
    std::atomic<int>   rateRemaining_{-1};
};

} // namespace ide
