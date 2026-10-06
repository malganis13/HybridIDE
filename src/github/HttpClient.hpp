// =============================================================================
//  HttpClient.hpp — тонкая RAII-обёртка над libcurl (HTTPS, редиректы,
//  таймауты, потоковое скачивание с прогрессом и отменой).
// =============================================================================
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ide {

struct HttpResponse {
    long                               status = 0;
    std::string                        body;
    std::map<std::string, std::string> headers;    // ключи в нижнем регистре
    std::string                        error;      // ошибка транспорта (curl)
    [[nodiscard]] bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

struct HttpRequest {
    std::string                        method = "GET";
    std::string                        url;
    std::vector<std::string>           headers;    // "Name: value"
    std::string                        body;
    long                               timeoutSec = 60;
};

class HttpClient {
public:
    using Progress = std::function<bool(std::uint64_t now, std::uint64_t total)>;   // false — отменить

    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse perform(const HttpRequest& req);
    // Скачивание в файл (атомарно: .part -> итоговое имя)
    HttpResponse download(const std::string& url, const std::vector<std::string>& headers,
                          const std::filesystem::path& dest, Progress progress = {});

    static std::string urlEncode(std::string_view s);

private:
    void* curl_ = nullptr;   // CURL*
};

} // namespace ide
