// =============================================================================
//  Workspace.cpp
// =============================================================================
#include "core/Workspace.hpp"

#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#  include <windows.h>
#  include <wincrypt.h>
#  pragma comment(lib, "crypt32.lib")
#else
#  include <sys/stat.h>
#endif

namespace ide {

namespace fs = std::filesystem;
using nlohmann::json;

void to_json(json& j, const OpenFileState& s) {
    j = json{{"path", s.path}, {"line", s.line}, {"col", s.col}, {"breakpoints", s.breakpoints}, {"folded", s.folded}};
}
void from_json(const json& j, OpenFileState& s) {
    s.path = j.value("path", "");
    s.line = j.value("line", 0);
    s.col  = j.value("col", 0);
    s.breakpoints = j.value("breakpoints", std::vector<int>{});
    s.folded      = j.value("folded", std::vector<int>{});
}

fs::path Workspace::userConfigDir() {
    fs::path dir;
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA")) dir = fs::path(a) / "HybridIDE";
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME")) dir = fs::path(h) / "Library/Application Support/HybridIDE";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) dir = fs::path(x) / "HybridIDE";
    else if (const char* h = std::getenv("HOME")) dir = fs::path(h) / ".config/HybridIDE";
#endif
    if (dir.empty()) dir = fs::current_path() / ".hybridide";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

static bool readJson(const fs::path& p, json& out, std::string* err) {
    std::ifstream f(p);
    if (!f) { if (err) *err = "file not found"; return false; }
    try { out = json::parse(f, nullptr, true, /*ignore_comments=*/true); return true; }
    catch (const std::exception& e) { if (err) *err = std::string("JSON: ") + e.what(); return false; }
}

static bool writeJson(const fs::path& p, const json& j, std::string* err) {
    auto tmp = p; tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + tmp.string(); return false; }
        f << j.dump(2);
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec); fs::remove(tmp, ec); }
    return !ec;
}

bool Workspace::openFolder(const fs::path& root, std::string* err) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) { if (err) *err = "Не каталог: " + root.string(); return false; }
    root_ = fs::canonical(root, ec);
    state = WorkspaceState{};
    json j;
    if (readJson(workspaceFile(root_), j, nullptr)) {
        try {
            state.openFiles   = j.value("openFiles", std::vector<OpenFileState>{});
            state.activeFile  = j.value("activeFile", "");
            state.buildConfig = j.value("buildConfig", "Debug");
            state.runArgs     = j.value("runArgs", "");
            state.customBuildCommand = j.value("customBuildCommand", "");
            state.customRunCommand   = j.value("customRunCommand", "");
            state.lspOverrides = j.value("lspOverrides", json::object());
            state.debugConfig  = j.value("debugConfig", json::object());
        } catch (const std::exception& e) {
            if (err) *err = std::string("Повреждён .ide_workspace.json: ") + e.what();
        }
    }
    return true;
}

bool Workspace::save(std::string* err) const {
    if (root_.empty()) return false;
    json j{{"version", 1},
           {"openFiles", state.openFiles},
           {"activeFile", state.activeFile},
           {"buildConfig", state.buildConfig},
           {"runArgs", state.runArgs},
           {"customBuildCommand", state.customBuildCommand},
           {"customRunCommand", state.customRunCommand},
           {"lspOverrides", state.lspOverrides},
           {"debugConfig", state.debugConfig}};
    return writeJson(workspaceFile(root_), j, err);
}

bool Workspace::loadSettings(UserSettings& s) {
    json j;
    if (!readJson(userConfigDir() / "settings.json", j, nullptr)) return false;
    try {
        s.language = j.value("language", s.language);
        s.theme    = j.value("theme", s.theme);
        s.keymap   = j.value("keymap", s.keymap);
        s.fontSize = j.value("fontSize", s.fontSize);
        s.uiScale  = j.value("uiScale", s.uiScale);
        s.vsync    = j.value("vsync", s.vsync);
        s.recentFolders    = j.value("recentFolders", s.recentFolders);
        s.themes           = j.value("themes", json::object());
        s.audio            = j.value("audio", json::object());
        s.githubBackupRepo = j.value("githubBackupRepo", s.githubBackupRepo);
        s.extensionSources = j.value("extensionSources", s.extensionSources);
    } catch (...) { return false; }
    return true;
}

bool Workspace::saveSettings(const UserSettings& s) {
    json j{{"language", s.language}, {"theme", s.theme}, {"keymap", s.keymap}, {"fontSize", s.fontSize},
           {"uiScale", s.uiScale}, {"vsync", s.vsync}, {"recentFolders", s.recentFolders}, {"themes", s.themes},
           {"audio", s.audio}, {"githubBackupRepo", s.githubBackupRepo}, {"extensionSources", s.extensionSources}};
    return writeJson(userConfigDir() / "settings.json", j, nullptr);
}

// ---------------------------------------------------------------------------
//  Хранилище секретов. Windows: DPAPI (CryptProtectData) — шифрование ключом
//  учётной записи пользователя. POSIX: файл с правами 0600.
// ---------------------------------------------------------------------------
static fs::path secretPath(const std::string& name) { return Workspace::userConfigDir() / ("secret_" + name + ".bin"); }

std::optional<std::string> Workspace::loadSecret(const std::string& name) {
    if (name == "github_token") {
        if (const char* env = std::getenv("HYBRIDIDE_GITHUB_TOKEN"); env && *env) return std::string(env);
    }
    std::ifstream f(secretPath(name), std::ios::binary);
    if (!f) return std::nullopt;
    std::string data((std::istreambuf_iterator<char>(f)), {});
#ifdef _WIN32
    DATA_BLOB in{(DWORD)data.size(), (BYTE*)data.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return std::nullopt;
    std::string plain((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
#else
    return data;
#endif
}

bool Workspace::saveSecret(const std::string& name, const std::string& value) {
    auto p = secretPath(name);
    std::string data = value;
#ifdef _WIN32
    DATA_BLOB in{(DWORD)value.size(), (BYTE*)value.data()}, out{};
    if (!CryptProtectData(&in, L"HybridIDE", nullptr, nullptr, nullptr, 0, &out)) return false;
    data.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
#endif
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
    }
#ifndef _WIN32
    chmod(p.c_str(), S_IRUSR | S_IWUSR);   // 0600 — только владелец
#endif
    return true;
}

bool Workspace::deleteSecret(const std::string& name) {
    std::error_code ec;
    return fs::remove(secretPath(name), ec);
}

} // namespace ide
