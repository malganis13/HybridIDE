// =============================================================================
//  LSPClient.cpp
// =============================================================================
#include "lsp/LSPClient.hpp"

#include "core/EventQueue.hpp"

#include <algorithm>

#ifdef _WIN32
#  include <windows.h>
#  define IDE_GETPID() (int)GetCurrentProcessId()
#else
#  include <unistd.h>
#  define IDE_GETPID() (int)getpid()
#endif

namespace ide {

namespace fs = std::filesystem;

static const char* kCompletionKinds[] = {"", "Text", "Method", "Function", "Constructor", "Field", "Variable", "Class",
    "Interface", "Module", "Property", "Unit", "Value", "Enum", "Keyword", "Snippet", "Color", "File", "Reference",
    "Folder", "EnumMember", "Constant", "Struct", "Event", "Operator", "TypeParameter"};

LSPClient::~LSPClient() { shutdown(); }

bool LSPClient::start(const std::vector<std::string>& cmd, const fs::path& root, std::string* err) {
    ProcessOptions o; o.argv = cmd; o.cwd = root;
    bool ok = chan_.start(o,
        [this](const json& m) { handleMessage(m); },
        [this](std::string_view e) { if (onLog) { std::string s(e); postToMain([this, s] { if (onLog) onLog("[" + name_ + "] " + s); }); } },
        [this](int code) { initialized_ = false; postToMain([this, code] { if (onLog) onLog("[" + name_ + "] exited: " + std::to_string(code)); }); },
        err);
    if (!ok) return false;

    // --- initialize: объявляем возможности клиента ---
    json caps = {
        {"general", {{"positionEncodings", {"utf-16"}}}},
        {"textDocument", {
            {"synchronization", {{"didSave", true}, {"dynamicRegistration", false}}},
            {"completion", {{"completionItem", {{"snippetSupport", true}, {"documentationFormat", {"plaintext", "markdown"}}}},
                            {"contextSupport", true}}},
            {"hover", {{"contentFormat", {"plaintext", "markdown"}}}},
            {"definition", {{"linkSupport", true}}},
            {"publishDiagnostics", {{"relatedInformation", false}}}}},
        {"window", {{"workDoneProgress", false}}}};
    json params = {
        {"processId", IDE_GETPID()},
        {"clientInfo", {{"name", "HybridIDE"}, {"version", IDE_VERSION}}},
        {"rootUri", pathToUri(root)},
        {"capabilities", caps},
        {"workspaceFolders", json::array({{{"uri", pathToUri(root)}, {"name", root.filename().u8string()}}})}};

    // initialize отправляется напрямую, минуя очередь
    int id = nextId_++;
    {
        std::lock_guard lk(pendingMtx_);
        pending_[id] = [this](const json& result, const json* error) {
            if (error) { if (onLog) onLog("[" + name_ + "] initialize failed: " + error->dump()); return; }
            auto sync = result.value("/capabilities/textDocumentSync"_json_pointer, json());
            if (sync.is_number()) incrementalSync_ = sync.get<int>() == 2;
            else if (sync.is_object()) incrementalSync_ = sync.value("change", 2) == 2;
            auto trig = result.value("/capabilities/completionProvider/triggerCharacters"_json_pointer, json::array());
            for (auto& t : trig) if (t.is_string()) triggerChars_ += t.get<std::string>();
            chan_.send({{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", json::object()}});
            initialized_ = true;
            std::vector<json> q;
            { std::lock_guard lk2(queueMtx_); q.swap(queuedBeforeInit_); }
            for (auto& m : q) chan_.send(m);
            if (onLog) onLog("[" + name_ + "] ready");
        };
    }
    chan_.send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"}, {"params", params}});
    return true;
}

void LSPClient::shutdown() {
    if (!chan_.running()) return;
    if (initialized_) {
        chan_.send({{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "shutdown"}, {"params", nullptr}});
        chan_.send({{"jsonrpc", "2.0"}, {"method", "exit"}});
    }
    initialized_ = false;
    chan_.stop();
}

int LSPClient::request(const std::string& method, json params, ResponseHandler h) {
    int id = nextId_++;
    { std::lock_guard lk(pendingMtx_); pending_[id] = std::move(h); }
    json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    if (!initialized_) { std::lock_guard lk(queueMtx_); queuedBeforeInit_.push_back(std::move(msg)); }
    else chan_.send(msg);
    return id;
}

void LSPClient::notify(const std::string& method, json params) {
    json msg = {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
    if (!initialized_) { std::lock_guard lk(queueMtx_); queuedBeforeInit_.push_back(std::move(msg)); }
    else chan_.send(msg);
}

void LSPClient::handleMessage(const json& msg) {
    // Вызывается из потока чтения stdout: только диспетчеризация, обработка — в UI-потоке
    if (msg.contains("id") && (msg.contains("result") || msg.contains("error")) && !msg.contains("method")) {
        int id = msg["id"].is_number() ? msg["id"].get<int>() : std::atoi(msg["id"].get<std::string>().c_str());
        ResponseHandler h;
        {
            std::lock_guard lk(pendingMtx_);
            auto it = pending_.find(id);
            if (it == pending_.end()) return;
            h = std::move(it->second);
            pending_.erase(it);
        }
        json result = msg.value("result", json());
        bool hasErr = msg.contains("error");
        json error = hasErr ? msg["error"] : json();
        postToMain([h = std::move(h), result = std::move(result), error = std::move(error), hasErr] { h(result, hasErr ? &error : nullptr); });
        return;
    }
    std::string method = msg.value("method", "");
    if (method == "textDocument/publishDiagnostics") {
        const auto& p = msg["params"];
        fs::path path = uriToPath(p.value("uri", ""));
        std::vector<Diagnostic> diags;
        for (auto& d : p.value("diagnostics", json::array())) {
            Diagnostic dg;
            dg.range.start = {d["range"]["start"].value("line", 0), d["range"]["start"].value("character", 0)};
            dg.range.end   = {d["range"]["end"].value("line", 0), d["range"]["end"].value("character", 0)};
            dg.severity = d.value("severity", 1);
            dg.message  = d.value("message", "");
            dg.source   = d.value("source", name_);
            diags.push_back(std::move(dg));
        }
        postToMain([this, path, diags = std::move(diags)]() mutable { if (onDiagnostics) onDiagnostics(path, std::move(diags)); });
    } else if (method == "window/logMessage" || method == "window/showMessage") {
        std::string text = msg["params"].value("message", "");
        postToMain([this, text] { if (onLog) onLog("[" + name_ + "] " + text); });
    } else if (msg.contains("id") && !method.empty()) {
        // Запрос от сервера (workspace/configuration, client/registerCapability...) — отвечаем пустым результатом
        json result = method == "workspace/configuration" ? json::array({json::object()}) : json();
        chan_.send({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", result}});
    }
}

json LSPClient::position(const Document& d, TextPos p) {
    return {{"line", p.line}, {"character", d.buffer.byteToUtf16(p.line, p.col)}};
}

void LSPClient::didOpen(const Document& d) {
    notify("textDocument/didOpen", {{"textDocument", {{"uri", d.uri()}, {"languageId", d.languageId()},
                                                      {"version", (int)d.buffer.version()}, {"text", d.buffer.text()}}}});
}

void LSPClient::didChange(const Document& d, const std::vector<TextEdit>& edits) {
    json changes = json::array();
    if (incrementalSync_ && edits.size() < 64) {
        // Позиции в edits уже относятся к промежуточным состояниям — порядок важен
        for (auto& e : edits) {
            changes.push_back({{"range", {{"start", {{"line", e.range.start.line}, {"character", e.range.start.col}}},
                                          {"end", {{"line", e.range.end.line}, {"character", e.range.end.col}}}}},
                               {"text", e.inserted}});
        }
        // Примечание: столбцы здесь в байтах; для не-ASCII строк надёжнее полная синхронизация
        bool ascii = true;
        for (auto& e : edits) {
            std::string probe = e.inserted + e.removed + d.buffer.line(std::min(e.range.start.line, d.buffer.lineCount() - 1));
            for (unsigned char c : probe) if (c >= 0x80) { ascii = false; break; }
        }
        if (!ascii) changes = json::array({{{"text", d.buffer.text()}}});
    } else {
        changes.push_back({{"text", d.buffer.text()}});
    }
    notify("textDocument/didChange", {{"textDocument", {{"uri", d.uri()}, {"version", (int)d.buffer.version()}}},
                                      {"contentChanges", changes}});
}

void LSPClient::didSave(const Document& d) { notify("textDocument/didSave", {{"textDocument", {{"uri", d.uri()}}}}); }
void LSPClient::didClose(const Document& d) { notify("textDocument/didClose", {{"textDocument", {{"uri", d.uri()}}}}); }

void LSPClient::completion(const Document& d, TextPos pos, std::function<void(std::vector<CompletionCandidate>)> cb) {
    std::string src = name_;
    // Копия строки курсора: документ может быть закрыт до прихода ответа,
    // поэтому ссылку на Document в асинхронный обработчик не передаём.
    const std::string lineText = d.buffer.line(pos.line);
    const int cursorLine = pos.line;
    auto u16ToByte = [lineText](int u16) {
        int u = 0, i = 0;
        while (i < (int)lineText.size() && u < u16) {
            unsigned char c = (unsigned char)lineText[(std::size_t)i];
            int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
            u += len == 4 ? 2 : 1; i += len;
        }
        return i;
    };
    request("textDocument/completion",
            {{"textDocument", {{"uri", d.uri()}}}, {"position", position(d, pos)}, {"context", {{"triggerKind", 1}}}},
            [cb, src, cursorLine, u16ToByte](const json& result, const json* error) {
                std::vector<CompletionCandidate> out;
                if (error || result.is_null()) { cb(out); return; }
                const json& items = result.is_array() ? result : result.value("items", json::array());
                for (auto& it : items) {
                    CompletionCandidate c;
                    c.label      = it.value("label", "");
                    c.insertText = it.value("insertText", c.label);
                    c.filterText = it.value("filterText", "");
                    c.sortText   = it.value("sortText", "");
                    c.detail     = it.value("detail", "");
                    c.source     = src;
                    int k = it.value("kind", 0);
                    if (k > 0 && k < (int)(sizeof(kCompletionKinds) / sizeof(*kCompletionKinds))) c.kind = kCompletionKinds[k];
                    if (it.contains("textEdit") && it["textEdit"].is_object()) {
                        const auto& te = it["textEdit"];
                        c.insertText = te.value("newText", c.insertText);
                        const json& r = te.contains("range") ? te["range"] : te.value("insert", json());
                        if (r.is_object()) {
                            int sl = r["start"].value("line", 0), el = r["end"].value("line", 0);
                            if (sl == cursorLine && el == cursorLine)   // однострочная правка в строке курсора
                                c.editRange = TextRange{{sl, u16ToByte(r["start"].value("character", 0))},
                                                        {el, u16ToByte(r["end"].value("character", 0))}};
                        }
                    }
                    // clangd добавляет ведущий пробел/маркер в label — нормализуем
                    while (!c.label.empty() && (c.label[0] == ' ' || (unsigned char)c.label[0] == 0xE2)) c.label.erase(0, 1);
                    out.push_back(std::move(c));
                    if (out.size() >= 200) break;
                }
                cb(std::move(out));
            });
}

void LSPClient::hover(const Document& d, TextPos pos, std::function<void(std::string)> cb) {
    request("textDocument/hover", {{"textDocument", {{"uri", d.uri()}}}, {"position", position(d, pos)}},
            [cb](const json& result, const json* error) {
                if (error || !result.is_object()) return;
                const auto& c = result["contents"];
                std::string text;
                if (c.is_string()) text = c.get<std::string>();
                else if (c.is_object()) text = c.value("value", "");
                else if (c.is_array()) for (auto& p : c) text += (p.is_string() ? p.get<std::string>() : p.value("value", "")) + "\n";
                // Убираем markdown-ограждения ``` для компактного tooltip
                for (std::size_t p; (p = text.find("```")) != std::string::npos;) {
                    auto eol = text.find('\n', p);
                    text.erase(p, (eol == std::string::npos ? text.size() : eol + 1) - p);
                }
                if (!text.empty()) cb(text);
            });
}

void LSPClient::definition(const Document& d, TextPos pos, std::function<void(fs::path, int, int)> cb) {
    request("textDocument/definition", {{"textDocument", {{"uri", d.uri()}}}, {"position", position(d, pos)}},
            [cb](const json& result, const json* error) {
                if (error || result.is_null()) return;
                json loc = result.is_array() ? (result.empty() ? json() : result[0]) : result;
                if (!loc.is_object()) return;
                std::string uri = loc.value("uri", loc.value("targetUri", ""));
                json range = loc.contains("range") ? loc["range"] : loc.value("targetSelectionRange", json());
                if (uri.empty() || !range.is_object()) return;
                cb(uriToPath(uri), range["start"].value("line", 0), range["start"].value("character", 0));
            });
}

// ------------------------------- LspManager ----------------------------------
LSPClient* LspManager::clientFor(const Document& d) {
    auto* lang = d.highlighter.language();
    if (!lang || root_.empty()) return nullptr;
    const std::string id = lang->id;
    if (auto it = clients_.find(id); it != clients_.end()) return it->second.get();
    if (failed_[id]) return nullptr;
    std::string cmd = overrides_.count(id) ? overrides_[id] : lang->lspCommand;
    if (cmd.empty()) return nullptr;
    auto argv = splitCommandLine(cmd);
    if (argv.empty() || !Process::findExecutable(argv[0])) {
        failed_[id] = true;
        if (onLog) onLog("[lsp] '" + (argv.empty() ? cmd : argv[0]) + "' не найден в PATH — используется локальное автодополнение (Buffer)");
        return nullptr;
    }
    auto c = std::make_unique<LSPClient>(argv[0], lang->lspLanguageId);
    c->onDiagnostics = [this](const fs::path& p, std::vector<Diagnostic> dg) { if (onDiagnostics) onDiagnostics(p, std::move(dg)); };
    c->onLog = [this](const std::string& s) { if (onLog) onLog(s); };
    std::string err;
    if (!c->start(argv, root_, &err)) {
        failed_[id] = true;
        if (onLog) onLog("[lsp] запуск " + cmd + " не удался: " + err);
        return nullptr;
    }
    return (clients_[id] = std::move(c)).get();
}

void LspManager::shutdownAll() {
    for (auto& [_, c] : clients_) c->shutdown();
    clients_.clear();
    failed_.clear();
}

std::vector<std::pair<std::string, bool>> LspManager::status() const {
    std::vector<std::pair<std::string, bool>> s;
    for (auto& [id, c] : clients_) s.emplace_back(c->name(), c->ready());
    return s;
}

} // namespace ide
