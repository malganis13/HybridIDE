// =============================================================================
//  LSPClient.hpp — асинхронный клиент Language Server Protocol 3.17
//  (JSON-RPC поверх stdio). Один клиент на язык; LspManager запускает
//  серверы лениво (clangd, rust-analyzer, pyright, gopls, tsserver...).
//  Все колбэки результатов доставляются в UI-поток через MainThreadDispatcher.
// =============================================================================
#pragma once
#include "core/EditorCore.hpp"
#include "lsp/JsonRpc.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ide {

class LSPClient {
public:
    using ResponseHandler = std::function<void(const json& result, const json* error)>;

    LSPClient(std::string name, std::string languageId) : name_(std::move(name)), languageId_(std::move(languageId)) {}
    ~LSPClient();

    bool start(const std::vector<std::string>& cmd, const std::filesystem::path& root, std::string* err = nullptr);
    void shutdown();
    [[nodiscard]] bool ready() const { return initialized_.load(); }
    [[nodiscard]] const std::string& name() const { return name_; }

    // --- Синхронизация документов ---
    void didOpen(const Document& d);
    void didChange(const Document& d, const std::vector<TextEdit>& edits);
    void didSave(const Document& d);
    void didClose(const Document& d);

    // --- Языковые функции ---
    void completion(const Document& d, TextPos pos, std::function<void(std::vector<CompletionCandidate>)> cb);
    void hover(const Document& d, TextPos pos, std::function<void(std::string)> cb);
    void definition(const Document& d, TextPos pos, std::function<void(std::filesystem::path, int line, int col)> cb);

    // Публикация диагностики: (путь, список)
    std::function<void(const std::filesystem::path&, std::vector<Diagnostic>)> onDiagnostics;
    std::function<void(const std::string&)> onLog;

    // Низкоуровневые методы
    int  request(const std::string& method, json params, ResponseHandler h);
    void notify(const std::string& method, json params);

private:
    void handleMessage(const json& msg);
    static json position(const Document& d, TextPos p);

    std::string                 name_, languageId_;
    JsonChannel                 chan_;
    std::atomic<int>            nextId_{1};
    std::mutex                  pendingMtx_;
    std::unordered_map<int, ResponseHandler> pending_;
    std::atomic<bool>           initialized_{false};
    std::vector<json>           queuedBeforeInit_;   // сообщения до завершения initialize
    std::mutex                  queueMtx_;
    bool                        incrementalSync_ = true;
    std::string                 triggerChars_;
};

class LspManager {
public:
    void setRoot(const std::filesystem::path& root) { root_ = root; }
    void setOverride(const std::string& langId, const std::string& command) { overrides_[langId] = command; }
    LSPClient* clientFor(const Document& d);   // запускает сервер при первом обращении
    void shutdownAll();
    [[nodiscard]] std::vector<std::pair<std::string, bool>> status() const;

    std::function<void(const std::filesystem::path&, std::vector<Diagnostic>)> onDiagnostics;
    std::function<void(const std::string&)> onLog;

private:
    std::filesystem::path root_;
    std::map<std::string, std::unique_ptr<LSPClient>> clients_;
    std::map<std::string, std::string> overrides_;
    std::map<std::string, bool> failed_;
};

} // namespace ide
