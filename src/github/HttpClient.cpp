// =============================================================================
//  HttpClient.cpp
// =============================================================================
#include "github/HttpClient.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>

namespace ide {

namespace {
// Глобальная инициализация libcurl — ровно один раз на процесс
struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};
void ensureCurl() { static CurlGlobal g; }

size_t writeString(char* p, size_t s, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, s * n);
    return s * n;
}
size_t writeFile(char* p, size_t s, size_t n, void* ud) {
    return std::fwrite(p, s, n, static_cast<FILE*>(ud)) * s;
}
size_t writeHeader(char* p, size_t s, size_t n, void* ud) {
    auto* h = static_cast<std::map<std::string, std::string>*>(ud);
    std::string line(p, s * n);
    auto c = line.find(':');
    if (c != std::string::npos) {
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char x) { return (char)std::tolower(x); });
        v.erase(0, v.find_first_not_of(" \t"));
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        (*h)[k] = v;
    }
    return s * n;
}
struct ProgressCtx { HttpClient::Progress cb; };
int progressCb(void* ud, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<ProgressCtx*>(ud);
    if (ctx && ctx->cb && !ctx->cb((std::uint64_t)dlNow, (std::uint64_t)dlTotal)) return 1;   // отмена
    return 0;
}
} // namespace

HttpClient::HttpClient() {
    ensureCurl();
    curl_ = curl_easy_init();
}

HttpClient::~HttpClient() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

std::string HttpClient::urlEncode(std::string_view s) {
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static curl_slist* buildHeaders(const std::vector<std::string>& hs) {
    curl_slist* list = nullptr;
    for (auto& h : hs) list = curl_slist_append(list, h.c_str());
    return list;
}

HttpResponse HttpClient::perform(const HttpRequest& req) {
    HttpResponse r;
    auto* c = static_cast<CURL*>(curl_);
    if (!c) { r.error = "curl_easy_init failed"; return r; }
    curl_easy_reset(c);
    curl_slist* hdrs = buildHeaders(req.headers);
    curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "HybridIDE/" IDE_VERSION);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, req.timeoutSec);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);              // безопасно для многопоточности
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");       // gzip/deflate/br, если поддерживается
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeString);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, writeHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);
    if (req.method != "GET") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(hdrs);
    return r;
}

HttpResponse HttpClient::download(const std::string& url, const std::vector<std::string>& headers,
                                  const std::filesystem::path& dest, Progress progress) {
    HttpResponse r;
    auto* c = static_cast<CURL*>(curl_);
    if (!c) { r.error = "curl_easy_init failed"; return r; }
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    auto part = dest; part += ".part";
#ifdef _WIN32
    FILE* f = _wfopen(part.c_str(), L"wb");
#else
    FILE* f = std::fopen(part.c_str(), "wb");
#endif
    if (!f) { r.error = "cannot open " + part.string(); return r; }
    curl_easy_reset(c);
    curl_slist* hdrs = buildHeaders(headers);
    ProgressCtx ctx{std::move(progress)};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "HybridIDE/" IDE_VERSION);
    // Release-ассеты GitHub отдаются редиректом на CDN; libcurl не пересылает
    // пользовательский заголовок Authorization на другой хост — токен не утекает.
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // обрыв, если < 1 КБ/с
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, writeHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);
    CURLcode rc = curl_easy_perform(c);
    std::fclose(f);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(hdrs);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    if (r.ok()) {
        std::filesystem::rename(part, dest, ec);
        if (ec) r.error = ec.message();
    } else std::filesystem::remove(part, ec);
    return r;
}

} // namespace ide
