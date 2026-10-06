// =============================================================================
//  ToolchainManager.cpp
// =============================================================================
#include "github/ToolchainManager.hpp"

#include "core/Async.hpp"
#include "core/Process.hpp"
#include "core/Workspace.hpp"

#include <cstdlib>
#include <algorithm>
#include <fstream>

namespace ide {

namespace fs = std::filesystem;
using nlohmann::json;

ToolchainManager::ToolchainManager(GitHubManager& gh) : gh_(gh) {
    // Каталог по умолчанию: реальные репозитории с бинарными релизами.
    // Регулярные выражения подбирают ассет под текущую ОС/архитектуру.
    catalog_ = {
        {"llvm", "LLVM / Clang", "llvm/llvm-project",
         R"(^LLVM-\d+\.\d+\.\d+-Windows-X64\.tar\.xz$)", R"(^LLVM-\d+\.\d+\.\d+-Linux-X64\.tar\.xz$)", R"(^LLVM-\d+\.\d+\.\d+-macOS-ARM64\.tar\.xz$)",
         "bin", "", "", {{"CC", "{bin}/clang"}, {"CXX", "{bin}/clang++"}}},
        {"mingw", "MinGW-w64 (WinLibs GCC)", "brechtsanders/winlibs_mingw",
         R"(^winlibs-x86_64-posix-seh-gcc-[\d.]+-.*-ucrt-r\d+\.zip$)", "", "", "mingw64/bin", "", "", {{"CC", "{bin}/gcc.exe"}, {"CXX", "{bin}/g++.exe"}}},
        {"msvc-portable", "Portable MSVC Build Tools", "Data-Oriented-House/PortableBuildTools",
         R"(^PortableBuildTools\.exe$)", "", "", ".", "", "", {}},
        {"cmake", "CMake", "Kitware/CMake",
         R"(^cmake-[\d.]+-windows-x86_64\.zip$)", R"(^cmake-[\d.]+-linux-x86_64\.tar\.gz$)", R"(^cmake-[\d.]+-macos-universal\.tar\.gz$)",
         "bin", "", "", {}},
        {"ninja", "Ninja", "ninja-build/ninja", R"(^ninja-win\.zip$)", R"(^ninja-linux\.zip$)", R"(^ninja-mac\.zip$)", ".", "", "", {}},
        {"clangd", "clangd (C/C++ LSP)", "clangd/clangd",
         R"(^clangd-windows-[\d.]+\.zip$)", R"(^clangd-linux-[\d.]+\.zip$)", R"(^clangd-mac-[\d.]+\.zip$)", "bin", "", "", {}},
        {"rust-analyzer", "rust-analyzer (Rust LSP)", "rust-lang/rust-analyzer",
         R"(^rust-analyzer-x86_64-pc-windows-msvc\.zip$)", R"(^rust-analyzer-x86_64-unknown-linux-gnu\.gz$)", R"(^rust-analyzer-aarch64-apple-darwin\.gz$)",
         ".", "", "", {}},
        {"python", "Python (standalone build)", "astral-sh/python-build-standalone",
         R"(^cpython-3\.12\.\d+\+\d+-x86_64-pc-windows-msvc-install_only\.tar\.gz$)",
         R"(^cpython-3\.12\.\d+\+\d+-x86_64-unknown-linux-gnu-install_only\.tar\.gz$)",
         R"(^cpython-3\.12\.\d+\+\d+-aarch64-apple-darwin-install_only\.tar\.gz$)", "python/bin", "", "", {}},
    };
#ifdef _WIN32
    for (auto& s : catalog_) if (s.id == "python") s.binSubdir = "python";
#endif
}

std::string ToolchainManager::currentPlatform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#else
    return "linux";
#endif
}

fs::path ToolchainManager::installRoot() const {
    auto p = Workspace::userConfigDir() / "toolchains";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

void ToolchainManager::setEnv(const std::string& k, const std::string& v) {
#ifdef _WIN32
    _putenv_s(k.c_str(), v.c_str());
#else
    setenv(k.c_str(), v.c_str(), 1);
#endif
}

void ToolchainManager::prependToPath(const fs::path& dir) {
    const char* cur = std::getenv("PATH");
    std::string path = cur ? cur : "";
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string d = dir.string();
    if (path.find(d) != std::string::npos) return;   // уже зарегистрирован
    setEnv("PATH", d + sep + path);
}

bool ToolchainManager::extractArchive(const fs::path& archive, const fs::path& dest, std::string* err) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    std::string name = archive.filename().string();
    ProcessOptions o; o.cwd = dest; o.mergeStderr = true;
    auto endsWith = [&](const std::string& suf) { return name.size() >= suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0; };
    if (endsWith(".zip")) {
#ifdef _WIN32
        // tar.exe (bsdtar) встроен в Windows 10 1803+ и умеет zip
        o.argv = {"tar", "-xf", archive.string(), "-C", dest.string()};
#else
        o.argv = Process::findExecutable("unzip") ? std::vector<std::string>{"unzip", "-q", "-o", archive.string(), "-d", dest.string()}
                                                  : std::vector<std::string>{"tar", "-xf", archive.string(), "-C", dest.string()};
#endif
    } else if (endsWith(".tar.gz") || endsWith(".tgz") || endsWith(".tar.xz") || endsWith(".tar.bz2") || endsWith(".tar.zst")) {
        o.argv = {"tar", "-xf", archive.string(), "-C", dest.string()};
    } else if (endsWith(".7z")) {
        o.argv = {"7z", "x", "-y", "-o" + dest.string(), archive.string()};
    } else if (endsWith(".gz")) {   // одиночный бинарник (rust-analyzer)
        fs::path out = dest / archive.stem();
#ifdef _WIN32
        o.argv = {"powershell", "-NoProfile", "-Command",
                  "$i=[IO.File]::OpenRead('" + archive.string() + "');$o=[IO.File]::Create('" + out.string() +
                  "');$g=New-Object IO.Compression.GzipStream($i,[IO.Compression.CompressionMode]::Decompress);$g.CopyTo($o);$o.Close();$g.Close()"};
#else
        o.argv = {"sh", "-c", "gzip -dc '" + archive.string() + "' > '" + out.string() + "' && chmod +x '" + out.string() + "'"};
#endif
    } else {
        // Не архив (например, .exe-установщик) — просто копируем
        fs::copy_file(archive, dest / archive.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec && err) *err = ec.message();
        return !ec;
    }
    auto r = Process::run(o);
    if (!r.ok() && err) *err = r.out + r.err;
    return r.ok();
}

ProcessResult ToolchainManager::runSetupScript(const fs::path& script, const fs::path& cwd, const std::map<std::string, std::string>& env) {
    ProcessOptions o; o.cwd = cwd; o.env = env; o.mergeStderr = true;
    auto ext = script.extension().string();
    if (ext == ".ps1") {
        std::string ps = Process::findExecutable("pwsh") ? "pwsh" : "powershell";
        o.argv = {ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script.string()};
    } else if (ext == ".sh") o.argv = {"bash", script.string()};
    else if (ext == ".lua") o.argv = {Process::findExecutable("luajit") ? "luajit" : "lua", script.string()};
    else if (ext == ".py") o.argv = {Process::findExecutable("python3") ? "python3" : "python", script.string()};
    else o.argv = {script.string()};
    return Process::run(o);
}

void ToolchainManager::install(const std::string& id, const std::string& tag) {
    auto it = std::find_if(catalog_.begin(), catalog_.end(), [&](auto& s) { return s.id == id; });
    if (it == catalog_.end()) return;
    ToolchainSpec spec = *it;
    std::string pattern = currentPlatform() == "windows" ? spec.assetWindows : currentPlatform() == "mac" ? spec.assetMac : spec.assetLinux;
    auto& job = jobs_[id];
    job = InstallJob{id, InstallStage::Resolving, 0.f, "GET /repos/" + spec.repo + "/releases"};
    if (pattern.empty()) { job.stage = InstallStage::Failed; job.message = "Нет сборки для платформы " + currentPlatform(); return; }

    gh_.listReleases(spec.repo, [this, spec, pattern, tag, id](std::vector<GitHubRelease> rels, std::string err) {
        auto& job = jobs_[id];
        if (!err.empty()) { job.stage = InstallStage::Failed; job.message = err; return; }
        std::regex re(pattern);
        const GitHubRelease* chosen = nullptr; const GitHubAsset* asset = nullptr;
        for (auto& r : rels) {
            if ((!tag.empty() && r.tag != tag) || (tag.empty() && r.prerelease)) continue;
            for (auto& a : r.assets) if (std::regex_match(a.name, re)) { chosen = &r; asset = &a; break; }
            if (asset) break;
        }
        if (!asset) { job.stage = InstallStage::Failed; job.message = "Подходящий ассет не найден (" + pattern + ")"; return; }
        fs::path dlDir = installRoot() / "_downloads";
        fs::path file  = dlDir / asset->name;
        fs::path dest  = installRoot() / (id + "-" + chosen->tag);
        std::string version = chosen->tag;
        job.stage = InstallStage::Downloading;
        job.message = asset->name + " (" + std::to_string(asset->size / (1024 * 1024)) + " MB)";
        gh_.downloadAsset(*asset, file, [this, id](float f) { jobs_[id].progress = f; },
            [this, id, spec, file, dest, version](bool ok, std::string e) {
                auto& j = jobs_[id];
                if (!ok) { j.stage = InstallStage::Failed; j.message = e; return; }
                j.stage = InstallStage::Extracting; j.message = file.filename().string();
                runAsync([file, dest] { std::string er; bool r = extractArchive(file, dest, &er); return std::make_pair(r, er); },
                    [this, id, spec, dest, version](std::pair<bool, std::string> res) {
                        auto& jj = jobs_[id];
                        if (!res.first) { jj.stage = InstallStage::Failed; jj.message = res.second; return; }
                        // Архив часто содержит единственный корневой каталог — спускаемся в него
                        fs::path root = dest;
                        std::error_code ec;
                        int dirs = 0, files = 0; fs::path only;
                        for (auto& e : fs::directory_iterator(dest, ec)) { if (e.is_directory()) { ++dirs; only = e.path(); } else ++files; }
                        if (dirs == 1 && files == 0 && !fs::exists(dest / spec.binSubdir, ec)) root = only;
                        InstalledToolchain t;
                        t.id = id; t.version = version; t.root = root;
                        t.bin = (root / spec.binSubdir).lexically_normal();
                        for (auto& [k, v] : spec.env) {
                            std::string val = v;
                            for (auto [ph, rep] : {std::pair<std::string, std::string>{"{bin}", t.bin.string()}, {"{root}", root.string()}})
                                for (auto p = val.find(ph); p != std::string::npos; p = val.find(ph)) val.replace(p, ph.size(), rep);
                            t.env[k] = val;
                        }
                        jj.stage = InstallStage::Registering;
                        registerToolchain(t);
                        installed_.erase(std::remove_if(installed_.begin(), installed_.end(), [&](auto& x) { return x.id == id; }), installed_.end());
                        installed_.push_back(t);
                        saveRegistry();
                        jj.stage = InstallStage::Done; jj.progress = 1.f;
                        jj.message = version + " -> " + t.bin.u8string();
                        if (log) log("[toolchain] " + spec.displayName + " " + version + " установлен: " + t.bin.u8string());
                    });
            });
    });
}

void ToolchainManager::registerToolchain(const InstalledToolchain& t) {
    prependToPath(t.bin);
    for (auto& [k, v] : t.env) setEnv(k, v);
}

void ToolchainManager::uninstall(const std::string& id) {
    auto it = std::find_if(installed_.begin(), installed_.end(), [&](auto& t) { return t.id == id; });
    if (it == installed_.end()) return;
    std::error_code ec;
    fs::path dir = it->root;
    // Удаляем только каталоги внутри installRoot (защита от случайного удаления)
    if (dir.u8string().rfind(installRoot().u8string(), 0) == 0) fs::remove_all(dir, ec);
    installed_.erase(it);
    saveRegistry();
}

void ToolchainManager::saveRegistry() const {
    json j = json::array();
    for (auto& t : installed_) j.push_back({{"id", t.id}, {"version", t.version}, {"root", t.root.u8string()}, {"bin", t.bin.u8string()}, {"env", t.env}});
    std::ofstream(installRoot() / "toolchains.json") << j.dump(2);
}

void ToolchainManager::loadRegistry() {
    std::ifstream f(installRoot() / "toolchains.json");
    if (!f) return;
    auto j = json::parse(f, nullptr, false);
    if (!j.is_array()) return;
    installed_.clear();
    for (auto& x : j) {
        InstalledToolchain t;
        t.id = x.value("id", ""); t.version = x.value("version", "");
        t.root = fs::u8path(x.value("root", "")); t.bin = fs::u8path(x.value("bin", ""));
        t.env = x.value("env", std::map<std::string, std::string>{});
        std::error_code ec;
        if (!fs::exists(t.bin, ec)) continue;   // каталог удалён вручную
        registerToolchain(t);
        installed_.push_back(std::move(t));
    }
}

} // namespace ide
