// =============================================================================
//  TerminalEngine.hpp — встроенный терминал: псевдотерминал (PTY) +
//  эмулятор VT100/xterm (подмножество) + докируемая ImGui-панель.
//  Linux/macOS: posix_openpt/grantpt/unlockpt + fork.
//  Windows: ConPTY (CreatePseudoConsole, Win10 1809+) — современная замена winpty.
// =============================================================================
#pragma once
#include "core/JThread.hpp"
#include <imgui.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ide {

struct TermCell {
    char32_t ch = U' ';
    ImU32    fg = IM_COL32(204, 204, 204, 255);
    ImU32    bg = 0;
    bool     bold = false;
};

// Эмулятор экрана: разбирает ANSI escape-последовательности
class VtScreen {
public:
    VtScreen(int cols = 120, int rows = 32) { resize(cols, rows); }
    void resize(int cols, int rows);
    void feed(std::string_view bytes);
    [[nodiscard]] int cols() const { return cols_; }
    [[nodiscard]] int rows() const { return rows_; }
    [[nodiscard]] const std::vector<TermCell>& row(int r) const { return grid_[(std::size_t)r]; }
    [[nodiscard]] const std::deque<std::vector<TermCell>>& scrollback() const { return scrollback_; }
    [[nodiscard]] int cursorX() const { return cx_; }
    [[nodiscard]] int cursorY() const { return cy_; }
    [[nodiscard]] std::string plainText() const;    // для копирования / тестов

private:
    void put(char32_t c);
    void newline();
    void csi(char final, const std::string& params);
    void sgr(const std::vector<int>& p);
    std::vector<std::vector<TermCell>> grid_;
    std::deque<std::vector<TermCell>>  scrollback_;
    int  cols_ = 0, rows_ = 0, cx_ = 0, cy_ = 0, savedX_ = 0, savedY_ = 0;
    ImU32 fg_ = IM_COL32(204, 204, 204, 255), bg_ = 0;
    bool bold_ = false;
    enum class St { Normal, Esc, Csi, Osc, OscEsc } st_ = St::Normal;
    std::string params_;
    std::string utf8acc_;
    int utf8need_ = 0;
};

class TerminalSession {
public:
    TerminalSession() = default;
    ~TerminalSession();
    bool start(const std::filesystem::path& cwd, int cols, int rows, std::string* err = nullptr);
    void stop();
    void write(std::string_view data);
    void resize(int cols, int rows);
    [[nodiscard]] bool running() const { return running_; }

    // Экран защищён мьютексом: читатель PTY пишет, UI-поток рисует
    std::mutex mtx;
    VtScreen   screen;
    std::string title = "shell";

private:
    std::atomic<bool> running_{false};
    ide::jthread      reader_;
#ifdef _WIN32
    void* hPC_ = nullptr; void* hIn_ = nullptr; void* hOut_ = nullptr; void* hProc_ = nullptr;
#else
    int masterFd_ = -1; int pid_ = -1;
#endif
};

// Панель с вкладками нескольких терминалов
class TerminalEngine {
public:
    void render(const char* windowTitle, bool* open);
    void newSession(const std::filesystem::path& cwd);
    void runCommand(const std::string& cmd);   // отправить команду в активный терминал
    float fontScale = 1.0f;
    ImU32 background = IM_COL32(18, 18, 18, 255);
private:
    std::vector<std::unique_ptr<TerminalSession>> sessions_;
    int active_ = 0;
    std::filesystem::path lastCwd_;
};

} // namespace ide
