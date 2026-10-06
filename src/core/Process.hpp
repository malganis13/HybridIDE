// =============================================================================
//  Process.hpp — кроссплатформенный запуск дочерних процессов с асинхронными
//  каналами stdin/stdout/stderr. Основа для LSP/DAP-серверов, git CLI,
//  систем сборки (cmake/cargo/go/npm) и скриптов установки тулчейнов.
// =============================================================================
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ide {

struct ProcessResult {
    int         exitCode = -1;
    std::string out;
    std::string err;
    [[nodiscard]] bool ok() const { return exitCode == 0; }
};

struct ProcessOptions {
    std::vector<std::string>           argv;        // argv[0] — исполняемый файл
    std::filesystem::path              cwd;         // рабочий каталог
    std::map<std::string, std::string> env;         // доп. переменные окружения
    bool                               mergeStderr = false;
};

class Process {
public:
    using DataCallback = std::function<void(std::string_view)>;
    using ExitCallback = std::function<void(int)>;

    Process() = default;
    ~Process();
    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

    // Запуск процесса. Колбэки вызываются из фоновых потоков-читателей!
    bool start(const ProcessOptions& opts, DataCallback onStdout,
               DataCallback onStderr = {}, ExitCallback onExit = {});

    // Запись в stdin (потокобезопасно)
    bool write(std::string_view data);
    void closeStdin();
    void terminate();
    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] const std::string& lastError() const { return lastError_; }

    // Синхронный запуск с захватом вывода (для коротких команд: git status и т.п.)
    static ProcessResult run(const ProcessOptions& opts);
    // Поиск исполняемого файла в PATH (с учётом PATHEXT на Windows)
    static std::optional<std::filesystem::path> findExecutable(const std::string& name);

private:
    void waitThread(ExitCallback onExit);

#ifdef _WIN32
    void* hProcess_ = nullptr;
    void* hStdinW_  = nullptr;
    void* hStdoutR_ = nullptr;
    void* hStderrR_ = nullptr;
#else
    int pid_      = -1;
    int stdinFd_  = -1;
    int stdoutFd_ = -1;
    int stderrFd_ = -1;
#endif
    std::atomic<bool> running_{false};
    std::mutex        writeMtx_;
    std::string       lastError_;
    std::jthread      outThread_, errThread_, waitThread_;
};

// Разбор командной строки "cmake --build build" -> {"cmake","--build","build"}
std::vector<std::string> splitCommandLine(std::string_view cmd);

} // namespace ide
