// =============================================================================
//  GitHubManager.cpp
// =============================================================================
#include "github/GitHubManager.hpp"

#include "core/Async.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <sstream>
#include <thread>

namespace ide {

using nlohmann::json;

void GitHubManager::setToken(std::string token) {
    // Нормализация: убираем пробелы/переводы строк, случайно вставленные из буфера обмена
    token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); }), token.end());
    std::lock_guard lk(mtx_);
    token_ = std::move(token);
    authenticated_ = false;
}

std::vector<std::string> GitHubManager::headers(bool binary) const {
    std::lock_guard lk(mtx_);
    std::vector<std::string> h{
        binary ? "Accept: application/octet-stream" : "Accept: application/vnd.github+json",
        "X-GitHub-Api-Version: 2022-11-28"};
    if (!token_.empty()) h.push_back("Authorization: Bearer " + token_);
    return h;
}

HttpResponse GitHubManager::api(const std::string& method, const std::string& path, const json* body) {
    HttpClient http;
    HttpRequest req;
    req.method  = method;
    req.url     = path.rfind("https://", 0) == 0 ? path : std::string(kApi) + path;
    req.headers = headers();
    if (body) { req.body = body->dump(); req.headers.push_back("Content-Type: application/json"); }
    auto r = http.perform(req);
    if (auto it = r.headers.find("x-ratelimit-remaining"); it != r.headers.end()) rateRemaining_ = std::atoi(it->second.c_str());
    return r;
}

static std::string apiError(const HttpResponse& r) {
    if (!r.error.empty()) return r.error;
    try {
        auto j = json::parse(r.body);
        return "HTTP " + std::to_string(r.status) + ": " + j.value("message", std::string());
    } catch (...) { return "HTTP " + std::to_string(r.status); }
}

std::string GitHubManager::base64Encode(std::string_view in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    unsigned val = 0; int bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; bits += 8;
        while (bits >= 0) { out.push_back(t[(val >> bits) & 0x3F]); bits -= 6; }
    }
    if (bits > -6) out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string GitHubManager::base64Decode(std::string_view in) {
    static const std::string t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned val = 0; int bits = -8;
    for (char c : in) {
        auto p = t.find(c);
        if (p == std::string::npos) continue;
        val = (val << 6) | (unsigned)p; bits += 6;
        if (bits >= 0) { out.push_back((char)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

void GitHubManager::validateToken(std::function<void(bool, std::string)> done) {
    runAsync([this] { return api("GET", "/user"); },
             [this, done](HttpResponse r) {
                 if (!r.ok()) { authenticated_ = false; done(false, apiError(r)); return; }
                 try {
                     auto j = json::parse(r.body);
                     user_.login = j.value("login", "");
                     user_.name  = j.value("name", json()).is_string() ? j["name"].get<std::string>() : "";
                     user_.avatarUrl = j.value("avatar_url", "");
                     user_.scopes.clear();
                     // Классические PAT возвращают список прав в X-OAuth-Scopes
                     std::stringstream ss(r.headers.count("x-oauth-scopes") ? r.headers["x-oauth-scopes"] : "");
                     for (std::string s; std::getline(ss, s, ',');) { s.erase(0, s.find_first_not_of(' ')); if (!s.empty()) user_.scopes.push_back(s); }
                     authenticated_ = true;
                     done(true, user_.login);
                 } catch (const std::exception& e) { done(false, e.what()); }
             });
}

static GitHubRepo parseRepo(const json& j) {
    GitHubRepo r;
    r.fullName = j.value("full_name", "");
    r.description = j.value("description", json()).is_string() ? j["description"].get<std::string>() : "";
    r.defaultBranch = j.value("default_branch", "main");
    r.htmlUrl = j.value("html_url", "");
    r.isPrivate = j.value("private", false);
    r.stars = j.value("stargazers_count", 0);
    for (auto& t : j.value("topics", json::array())) r.topics.push_back(t.get<std::string>());
    return r;
}

void GitHubManager::listUserRepos(std::function<void(std::vector<GitHubRepo>, std::string)> done) {
    runAsync([this] {
                 // Пагинация: до 5 страниц по 100 репозиториев
                 std::vector<GitHubRepo> all; std::string err;
                 for (int page = 1; page <= 5; ++page) {
                     auto r = api("GET", "/user/repos?per_page=100&sort=updated&page=" + std::to_string(page));
                     if (!r.ok()) { err = apiError(r); break; }
                     auto j = json::parse(r.body, nullptr, false);
                     if (!j.is_array() || j.empty()) break;
                     for (auto& x : j) all.push_back(parseRepo(x));
                     if (j.size() < 100) break;
                 }
                 return std::make_pair(all, err);
             },
             [done](std::pair<std::vector<GitHubRepo>, std::string> p) { done(std::move(p.first), p.second); });
}

void GitHubManager::searchExtensions(const std::string& query, std::function<void(std::vector<GitHubRepo>, std::string)> done) {
    // Расширения помечаются темой (topic) hybridide-extension
    std::string q = "topic:hybridide-extension";
    if (!query.empty()) q += " " + query;
    runAsync([this, q] { return api("GET", "/search/repositories?per_page=50&q=" + HttpClient::urlEncode(q)); },
             [done](HttpResponse r) {
                 std::vector<GitHubRepo> out;
                 if (!r.ok()) { done(out, apiError(r)); return; }
                 auto j = json::parse(r.body, nullptr, false);
                 for (auto& x : j.value("items", json::array())) out.push_back(parseRepo(x));
                 done(out, "");
             });
}

void GitHubManager::getFile(const std::string& fullName, const std::string& path, std::function<void(std::optional<std::string>, std::string)> done) {
    runAsync([this, fullName, path] { return api("GET", "/repos/" + fullName + "/contents/" + path); },
             [done](HttpResponse r) {
                 if (!r.ok()) { done(std::nullopt, apiError(r)); return; }
                 auto j = json::parse(r.body, nullptr, false);
                 done(base64Decode(j.value("content", "")), "");
             });
}

void GitHubManager::fetchManifest(const std::string& fullName, std::function<void(std::optional<ExtensionManifest>, std::string)> done) {
    getFile(fullName, "hybridide-extension.json", [fullName, done](std::optional<std::string> content, std::string err) {
        if (!content) { done(std::nullopt, err); return; }
        auto j = json::parse(*content, nullptr, false);
        if (j.is_discarded()) { done(std::nullopt, "invalid manifest JSON"); return; }
        ExtensionManifest m;
        m.repo = fullName; m.name = j.value("name", fullName); m.version = j.value("version", "0.0.0");
        m.type = j.value("type", "plugin"); m.description = j.value("description", ""); m.raw = j;
        done(m, "");
    });
}

void GitHubManager::listReleases(const std::string& fullName, std::function<void(std::vector<GitHubRelease>, std::string)> done) {
    runAsync([this, fullName] { return api("GET", "/repos/" + fullName + "/releases?per_page=20"); },
             [done](HttpResponse r) {
                 std::vector<GitHubRelease> out;
                 if (!r.ok()) { done(out, apiError(r)); return; }
                 auto j = json::parse(r.body, nullptr, false);
                 for (auto& x : j) {
                     GitHubRelease rel;
                     rel.id = x.value("id", 0LL); rel.tag = x.value("tag_name", ""); rel.prerelease = x.value("prerelease", false);
                     rel.name = x.value("name", json()).is_string() ? x["name"].get<std::string>() : rel.tag;
                     rel.publishedAt = x.value("published_at", json()).is_string() ? x["published_at"].get<std::string>() : "";
                     for (auto& a : x.value("assets", json::array()))
                         rel.assets.push_back({a.value("id", 0LL), a.value("name", ""), a.value("content_type", ""),
                                               a.value("browser_download_url", ""), a.value("url", ""), a.value("size", 0LL)});
                     out.push_back(std::move(rel));
                 }
                 done(out, "");
             });
}

void GitHubManager::downloadAsset(const GitHubAsset& a, const std::filesystem::path& dest,
                                  std::function<void(float)> progress, std::function<void(bool, std::string)> done) {
    // GET /repos/{o}/{r}/releases/assets/{id} c Accept: application/octet-stream
    // работает и для приватных репозиториев (в отличие от browser_download_url)
    auto hdrs = headers(true);
    std::string url = a.apiUrl.empty() ? a.browserUrl : a.apiUrl;
    ThreadPool::global().submit([url, hdrs, dest, progress, done] {
        HttpClient http;
        auto r = http.download(url, hdrs, dest, [progress](std::uint64_t now, std::uint64_t total) {
            if (total > 0 && progress) { float f = (float)now / (float)total; postToMain([progress, f] { progress(f); }); }
            return true;
        });
        bool ok = r.ok();
        std::string err = ok ? "" : apiError(r);
        postToMain([done, ok, err] { done(ok, err); });
    });
}

void GitHubManager::createRepo(const std::string& name, bool isPrivate, const std::string& description, std::function<void(bool, std::string)> done) {
    json body = {{"name", name}, {"private", isPrivate}, {"description", description}, {"auto_init", true}};
    runAsync([this, body] { return api("POST", "/user/repos", &body); },
             [done](HttpResponse r) {
                 if (r.status == 201) { auto j = json::parse(r.body, nullptr, false); done(true, j.value("full_name", "")); }
                 else done(false, apiError(r));
             });
}

void GitHubManager::putFile(const std::string& fullName, const std::string& path, const std::string& content,
                            const std::string& message, std::function<void(bool, std::string)> done) {
    runAsync([this, fullName, path, content, message] {
                 // Для обновления существующего файла GitHub требует его текущий sha
                 json body = {{"message", message}, {"content", base64Encode(content)}};
                 auto cur = api("GET", "/repos/" + fullName + "/contents/" + path);
                 if (cur.ok()) { auto j = json::parse(cur.body, nullptr, false); if (j.contains("sha")) body["sha"] = j["sha"]; }
                 return api("PUT", "/repos/" + fullName + "/contents/" + path, &body);
             },
             [done](HttpResponse r) { done(r.ok(), r.ok() ? "" : apiError(r)); });
}

void GitHubManager::backupWorkspace(const std::string& repoName, const std::vector<std::pair<std::string, std::string>>& files,
                                    std::function<void(bool, std::string)> done) {
    runAsync([this, repoName, files] {
                 if (user_.login.empty()) return std::make_pair(false, std::string("not authenticated"));
                 std::string full = user_.login + "/" + repoName;
                 auto probe = api("GET", "/repos/" + full);
                 if (probe.status == 404) {
                     json body = {{"name", repoName}, {"private", true}, {"auto_init", true},
                                  {"description", "HybridIDE settings backup (auto-generated)"}};
                     auto c = api("POST", "/user/repos", &body);
                     if (c.status != 201) return std::make_pair(false, apiError(c));
                     std::this_thread::sleep_for(std::chrono::seconds(2));   // репозиторию нужно время на инициализацию
                 } else if (!probe.ok()) return std::make_pair(false, apiError(probe));
                 int okCount = 0;
                 std::string lastErr;
                 for (auto& [path, content] : files) {
                     json body = {{"message", "HybridIDE backup: " + path}, {"content", base64Encode(content)}};
                     auto cur = api("GET", "/repos/" + full + "/contents/" + path);
                     if (cur.ok()) { auto j = json::parse(cur.body, nullptr, false); if (j.contains("sha")) body["sha"] = j["sha"]; }
                     auto put = api("PUT", "/repos/" + full + "/contents/" + path, &body);
                     if (put.ok()) ++okCount; else lastErr = apiError(put);
                 }
                 bool ok = okCount == (int)files.size();
                 return std::make_pair(ok, ok ? full : lastErr);
             },
             [done](std::pair<bool, std::string> r) { done(r.first, r.second); });
}

} // namespace ide
