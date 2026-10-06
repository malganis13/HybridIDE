// =============================================================================
//  Process.cpp — реализация для Win32 (CreateProcessW + анонимные pipe'ы)
//  и POSIX (pipe + fork + execvp).
// =============================================================================
#include "core/Process.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace ide {

namespace fs = std::filesystem;

std::vector<std::string> splitCommandLine(std::string_view cmd) {
    // Простой shell-подобный разбор: пробелы, одинарные/двойные кавычки, '\'
    std::vector<std::string> out;
    std::string cur;
    bool inSingle = false, inDouble = false, has = false;
    for (std::size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (c == '\\' && !inSingle && i + 1 < cmd.size() && (cmd[i + 1] == '"' || cmd[i + 1] == '\\')) {
            cur += cmd[++i]; has = true;
        } else if (c == '\'' && !inDouble) { inSingle = !inSingle; has = true; }
        else if (c == '"' && !inSingle)    { inDouble = !inDouble; has = true; }
        else if ((c == ' ' || c == '\t') && !inSingle && !inDouble) {
            if (has) { out.push_back(std::move(cur)); cur.clear(); has = false; }
        } else { cur += c; has = true; }
    }
    if (has) out.push_back(std::move(cur));
    return out;
}

std::optional<fs::path> Process::findExecutable(const std::string& name) {
    if (name.empty()) return std::nullopt;
    fs::path p(name);
    std::error_code ec;
    if (p.has_parent_path() && fs::exists(p, ec)) return p;
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return std::nullopt;
#ifdef _WIN32
    const char sep = ';';
    std::vector<std::string> exts{"", ".exe", ".cmd", ".bat", ".com"};
#else
    const char sep = ':';
    std::vector<std::string> exts{""};
#endif
    std::stringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, sep)) {
        if (dir.empty()) continue;
        for (const auto& ext : exts) {
            fs::path cand = fs::path(dir) / (name + ext);
            if (fs::is_regular_file(cand, ec)) return cand;
        }
    }
    return std::nullopt;
}

Process::~Process() {
    terminate();
}

#ifdef _WIN32
// ============================ Windows =======================================
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
// Экранирование аргумента по правилам CommandLineToArgvW
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    std::size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') r.append(bs * 2 + 1, L'\\');
        else           r.append(bs, L'\\');
        bs = 0;
        r += c;
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}
} // namespace

bool Process::start(const ProcessOptions& opts, DataCallback onStdout, DataCallback onStderr, ExitCallback onExit) {
    if (opts.argv.empty()) { lastError_ = "empty argv"; return false; }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR, inW, outR, outW, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) {
        lastError_ = "CreatePipe failed"; return false;
    }
    if (!opts.mergeStderr && !CreatePipe(&errR, &errW, &sa, 0)) { lastError_ = "CreatePipe failed"; return false; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    if (errR) SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdline;
    for (const auto& a : opts.argv) { if (!cmdline.empty()) cmdline += L' '; cmdline += quoteArg(widen(a)); }

    // Блок окружения: текущее + переопределения
    std::wstring envBlock;
    if (!opts.env.empty()) {
        std::map<std::wstring, std::wstring> merged;
        if (LPWCH cur = GetEnvironmentStringsW()) {
            for (LPWCH p = cur; *p; p += wcslen(p) + 1) {
                std::wstring kv(p);
                auto eq = kv.find(L'=', 1);
                if (eq != std::wstring::npos) merged[kv.substr(0, eq)] = kv.substr(eq + 1);
            }
            FreeEnvironmentStringsW(cur);
        }
        for (const auto& [k, v] : opts.env) merged[widen(k)] = widen(v);
        for (const auto& [k, v] : merged) { envBlock += k + L"=" + v; envBlock.push_back(L'\0'); }
        envBlock.push_back(L'\0');
    }

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = inR;
    si.hStdOutput = outW;
    si.hStdError  = opts.mergeStderr ? outW : errW;
    PROCESS_INFORMATION pi{};
    std::wstring cwd = opts.cwd.empty() ? std::wstring() : opts.cwd.wstring();
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             envBlock.empty() ? nullptr : envBlock.data(),
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(inR); CloseHandle(outW); if (errW) CloseHandle(errW);
    if (!ok) {
        lastError_ = "CreateProcess failed, code " + std::to_string(GetLastError());
        CloseHandle(inW); CloseHandle(outR); if (errR) CloseHandle(errR);
        return false;
    }
    CloseHandle(pi.hThread);
    hProcess_ = pi.hProcess; hStdinW_ = inW; hStdoutR_ = outR; hStderrR_ = errR;
    running_  = true;

    auto reader = [](HANDLE h, DataCallback cb) {
        std::array<char, 8192> buf{};
        DWORD n = 0;
        while (ReadFile(h, buf.data(), (DWORD)buf.size(), &n, nullptr) && n > 0)
            if (cb) cb(std::string_view(buf.data(), n));
    };
    outThread_ = std::jthread([=] { reader(outR, onStdout); });
    if (errR) errThread_ = std::jthread([=] { reader(errR, onStderr); });
    waitThread_ = std::jthread([this, onExit] { waitThread(onExit); });
    return true;
}

void Process::waitThread(ExitCallback onExit) {
    WaitForSingleObject(hProcess_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(hProcess_, &code);
    if (outThread_.joinable()) outThread_.join();
    if (errThread_.joinable()) errThread_.join();
    running_ = false;
    if (onExit) onExit((int)code);
}

bool Process::write(std::string_view data) {
    std::lock_guard lk(writeMtx_);
    if (!hStdinW_) return false;
    DWORD written = 0;
    return WriteFile(hStdinW_, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
}

void Process::closeStdin() {
    std::lock_guard lk(writeMtx_);
    if (hStdinW_) { CloseHandle(hStdinW_); hStdinW_ = nullptr; }
}

void Process::terminate() {
    if (hProcess_ && running_) TerminateProcess(hProcess_, 1);
    closeStdin();
    if (waitThread_.joinable()) waitThread_.join();
    if (hStdoutR_) { CloseHandle(hStdoutR_); hStdoutR_ = nullptr; }
    if (hStderrR_) { CloseHandle(hStderrR_); hStderrR_ = nullptr; }
    if (hProcess_) { CloseHandle(hProcess_); hProcess_ = nullptr; }
}

#else
// ============================== POSIX =======================================
bool Process::start(const ProcessOptions& opts, DataCallback onStdout, DataCallback onStderr, ExitCallback onExit) {
    if (opts.argv.empty()) { lastError_ = "empty argv"; return false; }
    int in[2], out[2], err[2] = {-1, -1};
    if (pipe(in) != 0 || pipe(out) != 0 || (!opts.mergeStderr && pipe(err) != 0)) {
        lastError_ = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    // Pipe для сообщения об ошибке execvp (закрывается автоматически при успехе)
    int execErr[2];
    if (pipe(execErr) != 0) { lastError_ = "pipe failed"; return false; }
    fcntl(execErr[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) { lastError_ = std::string("fork: ") + std::strerror(errno); return false; }
    if (pid == 0) {
        // --- дочерний процесс ---
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(opts.mergeStderr ? out[1] : err[1], STDERR_FILENO);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        if (err[0] >= 0) { close(err[0]); close(err[1]); }
        close(execErr[0]);
        if (!opts.cwd.empty() && chdir(opts.cwd.c_str()) != 0) {
            int e = errno; (void)!::write(execErr[1], &e, sizeof(e)); _exit(127);
        }
        for (const auto& [k, v] : opts.env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> argv;
        for (const auto& a : opts.argv) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        int e = errno; (void)!::write(execErr[1], &e, sizeof(e));
        _exit(127);
    }
    // --- родительский процесс ---
    close(in[0]); close(out[1]); if (err[1] >= 0) close(err[1]);
    close(execErr[1]);
    int childErrno = 0;
    if (read(execErr[0], &childErrno, sizeof(childErrno)) == sizeof(childErrno)) {
        close(execErr[0]);
        close(in[1]); close(out[0]); if (err[0] >= 0) close(err[0]);
        waitpid(pid, nullptr, 0);
        lastError_ = "exec '" + opts.argv[0] + "': " + std::strerror(childErrno);
        return false;
    }
    close(execErr[0]);

    pid_ = pid; stdinFd_ = in[1]; stdoutFd_ = out[0]; stderrFd_ = err[0];
    running_ = true;
    signal(SIGPIPE, SIG_IGN);   // запись в закрытый pipe не должна убивать IDE

    auto reader = [](int fd, DataCallback cb) {
        std::array<char, 8192> buf{};
        for (;;) {
            ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n > 0) { if (cb) cb(std::string_view(buf.data(), (std::size_t)n)); }
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
    };
    outThread_ = std::jthread([=, this] { reader(stdoutFd_, onStdout); });
    if (stderrFd_ >= 0) errThread_ = std::jthread([=, this] { reader(stderrFd_, onStderr); });
    waitThread_ = std::jthread([this, onExit] { waitThread(onExit); });
    return true;
}

void Process::waitThread(ExitCallback onExit) {
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    if (outThread_.joinable()) outThread_.join();
    if (errThread_.joinable()) errThread_.join();
    running_ = false;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    if (onExit) onExit(code);
}

bool Process::write(std::string_view data) {
    std::lock_guard lk(writeMtx_);
    if (stdinFd_ < 0) return false;
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(stdinFd_, data.data() + off, data.size() - off);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        off += (std::size_t)n;
    }
    return true;
}

void Process::closeStdin() {
    std::lock_guard lk(writeMtx_);
    if (stdinFd_ >= 0) { close(stdinFd_); stdinFd_ = -1; }
}

void Process::terminate() {
    if (pid_ > 0 && running_) {
        kill(pid_, SIGTERM);
        // Даём процессу 300 мс на корректное завершение, затем SIGKILL
        for (int i = 0; i < 30 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (running_) kill(pid_, SIGKILL);
    }
    closeStdin();
    if (waitThread_.joinable()) waitThread_.join();
    if (stdoutFd_ >= 0) { close(stdoutFd_); stdoutFd_ = -1; }
    if (stderrFd_ >= 0) { close(stderrFd_); stderrFd_ = -1; }
    pid_ = -1;
}
#endif

ProcessResult Process::run(const ProcessOptions& opts) {
    ProcessResult r;
    std::mutex m;
    Process p;
    std::atomic<bool> done{false};
    int code = -1;
    if (!p.start(opts,
            [&](std::string_view d) { std::lock_guard lk(m); r.out.append(d); },
            [&](std::string_view d) { std::lock_guard lk(m); r.err.append(d); },
            [&](int c) { code = c; done = true; })) {
        r.err = p.lastError();
        return r;
    }
    p.closeStdin();
    while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (p.waitThread_.joinable()) p.waitThread_.join();
    r.exitCode = code;
    return r;
}

} // namespace ide
