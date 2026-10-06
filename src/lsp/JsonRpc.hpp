// =============================================================================
//  JsonRpc.hpp — транспорт JSON-RPC 2.0 с заголовками Content-Length
//  (общий для LSP и DAP). Потоковый разборщик собирает сообщения из
//  произвольно нарезанных фрагментов stdout дочернего процесса.
// =============================================================================
#pragma once
#include "core/Process.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ide {

using json = nlohmann::json;

// Инкрементальный разбор потока "Content-Length: N\r\n\r\n{...}"
class MessageFramer {
public:
    // Возвращает все полностью полученные сообщения
    std::vector<std::string> feed(std::string_view chunk);
    static std::string frame(const std::string& body);
private:
    std::string buf_;
};

// Двунаправленный канал JSON поверх процесса (stdio)
class JsonChannel {
public:
    using MessageHandler = std::function<void(const json&)>;
    ~JsonChannel() { stop(); }

    bool start(const ProcessOptions& opts, MessageHandler onMessage, std::function<void(std::string_view)> onStderr,
               std::function<void(int)> onExit, std::string* err);
    bool send(const json& msg);
    void stop();
    [[nodiscard]] bool running() const { return proc_ && proc_->running(); }

private:
    std::unique_ptr<Process> proc_;
    MessageFramer            framer_;
    std::mutex               framerMtx_;
};

} // namespace ide
