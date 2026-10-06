// =============================================================================
//  DAPClient.hpp — асинхронный клиент Debug Adapter Protocol.
//  Типы повторяют структуры google/cppdap (dap::StackFrame, dap::Scope,
//  dap::Variable, dap::Breakpoint), но сериализуются через nlohmann/json,
//  что убирает лишнюю зависимость и даёт единый транспорт с LSP.
//  Поддерживаемые адаптеры: lldb-dap, gdb -i dap (GDB 14+), codelldb,
//  debugpy (Python), dlv dap (Go), cppvsdbg/cpptools (через OpenDebugAD7).
//  Визуализаторы: Call Stack, Locals/Globals/Watch, Breakpoints, Memory, Console.
// =============================================================================
#pragma once
#include "lsp/JsonRpc.hpp"

#include <imgui.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ide {

namespace dap {
struct Source     { std::string name, path; };
struct StackFrame { int id = 0; std::string name; Source source; int line = 0, column = 0; std::string instructionPointerReference; };
struct Thread     { int id = 0; std::string name; };
struct Scope      { std::string name; int variablesReference = 0; bool expensive = false; };
struct Variable   { std::string name, value, type, evaluateName, memoryReference; int variablesReference = 0; };
struct Breakpoint { int id = 0; bool verified = false; int line = 0; std::string message; };
} // namespace dap

enum class DebugState { Idle, Initializing, Running, Stopped, Terminated };

struct LaunchConfig {
    std::string              adapter;          // "lldb-dap", "gdb -i dap", "python -m debugpy.adapter", "dlv dap"
    std::string              request = "launch";
    std::string              program;
    std::vector<std::string> args;
    std::string              cwd;
    bool                     stopOnEntry = false;
    nlohmann::json           extra = nlohmann::json::object();   // доп. поля launch.json
};

class DAPClient {
public:
    using Handler = std::function<void(bool success, const json& body, const std::string& message)>;
    ~DAPClient() { stop(); }

    bool start(const LaunchConfig& cfg, const std::filesystem::path& root, std::string* err = nullptr);
    void stop();
    [[nodiscard]] DebugState state() const { return state_; }

    // Управление выполнением
    void continue_();
    void next();
    void stepIn();
    void stepOut();
    void pause();
    void restart();

    // Точки останова (хранятся и до запуска сессии)
    void setBreakpoint(const std::filesystem::path& file, int line0, bool on);
    [[nodiscard]] const std::map<std::string, std::set<int>>& breakpoints() const { return bps_; }

    void evaluate(const std::string& expr, const std::string& context, std::function<void(std::string)> cb);
    void readMemory(const std::string& memoryReference, int offset, int count);
    void addWatch(const std::string& expr) { watches_.push_back({expr, ""}); refreshWatches(); }

    // Визуализаторы
    void renderCallStack(const char* title, bool* open);
    void renderVariables(const char* title, bool* open);
    void renderWatch(const char* title, bool* open);
    void renderBreakpoints(const char* title, bool* open);
    void renderMemory(const char* title, bool* open);
    void renderConsole(const char* title, bool* open);

    std::function<void(const std::filesystem::path&, int line0)> onStoppedAt;
    std::function<void()>                                        onTerminated;
    std::function<void(const std::string&)>                      onOutput;

    static std::vector<std::uint8_t> base64Decode(std::string_view in);

private:
    int  send(const std::string& command, json args, Handler h = {});
    void handleMessage(const json& m);
    void onEvent(const std::string& ev, const json& body);
    void sendAllBreakpoints();
    void sendBreakpoints(const std::string& path);
    void fetchStack(int threadId);
    void selectFrame(int frameId);
    void fetchVariables(int ref);
    void refreshWatches();
    void drawVariable(const dap::Variable& v, int depth);

    JsonChannel                         chan_;
    LaunchConfig                        cfg_;
    std::filesystem::path               root_;
    std::atomic<int>                    seq_{1};
    std::mutex                          pendMtx_;
    std::unordered_map<int, Handler>    pending_;
    std::atomic<DebugState>             state_{DebugState::Idle};
    bool                                supportsRestart_ = false, supportsReadMemory_ = false;
    std::map<std::string, std::set<int>> bps_;                 // путь -> строки (0-based)
    std::map<std::string, std::vector<dap::Breakpoint>> verified_;
    std::vector<dap::Thread>            threads_;
    int                                 stoppedThread_ = 0;
    std::vector<dap::StackFrame>        frames_;
    int                                 selectedFrame_ = 0;
    std::vector<dap::Scope>             scopes_;
    std::unordered_map<int, std::vector<dap::Variable>> vars_;   // variablesReference -> дети
    std::set<int>                       requested_;
    struct Watch { std::string expr, value; };
    std::vector<Watch>                  watches_;
    std::string                         watchInput_, consoleInput_, memAddr_;
    std::vector<std::string>            console_;
    std::string                         memRef_;
    std::uint64_t                       memBase_ = 0;
    std::vector<std::uint8_t>           memory_;
    std::string                         memError_;
};

} // namespace ide
