// =============================================================================
//  TerminalEngine.cpp — PTY и VT-эмулятор
// =============================================================================
#include <imgui_internal.h>   // ImTextCharFromUtf8 / ImTextCharToUtf8
#include "core/TerminalEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/ioctl.h>
#  include <sys/wait.h>
#  include <termios.h>
#  include <unistd.h>
#endif

namespace ide {

// ------------------------------- VtScreen ----------------------------------
static const ImU32 kAnsi[16] = {
    IM_COL32(12, 12, 12, 255),   IM_COL32(197, 15, 31, 255),  IM_COL32(19, 161, 14, 255),  IM_COL32(193, 156, 0, 255),
    IM_COL32(0, 55, 218, 255),   IM_COL32(136, 23, 152, 255), IM_COL32(58, 150, 221, 255), IM_COL32(204, 204, 204, 255),
    IM_COL32(118, 118, 118, 255),IM_COL32(231, 72, 86, 255),  IM_COL32(22, 198, 12, 255),  IM_COL32(249, 241, 165, 255),
    IM_COL32(59, 120, 255, 255), IM_COL32(180, 0, 158, 255),  IM_COL32(97, 214, 214, 255), IM_COL32(242, 242, 242, 255)};

static ImU32 xterm256(int n) {
    if (n < 16) return kAnsi[n];
    if (n < 232) { n -= 16; int r = n / 36, g = (n / 6) % 6, b = n % 6; auto v = [](int x) { return x ? 55 + x * 40 : 0; }; return IM_COL32(v(r), v(g), v(b), 255); }
    int g = 8 + (n - 232) * 10; return IM_COL32(g, g, g, 255);
}

void VtScreen::resize(int cols, int rows) {
    cols = std::max(10, cols); rows = std::max(3, rows);
    grid_.resize((std::size_t)rows);
    for (auto& r : grid_) r.resize((std::size_t)cols);
    cols_ = cols; rows_ = rows;
    cx_ = std::min(cx_, cols_ - 1); cy_ = std::min(cy_, rows_ - 1);
}

void VtScreen::newline() {
    if (++cy_ >= rows_) {
        scrollback_.push_back(std::move(grid_.front()));
        if (scrollback_.size() > 5000) scrollback_.pop_front();   // лимит истории
        grid_.erase(grid_.begin());
        grid_.emplace_back((std::size_t)cols_);
        cy_ = rows_ - 1;
    }
}

void VtScreen::put(char32_t c) {
    if (cx_ >= cols_) { cx_ = 0; newline(); }
    auto& cell = grid_[(std::size_t)cy_][(std::size_t)cx_];
    cell.ch = c; cell.fg = fg_; cell.bg = bg_; cell.bold = bold_;
    ++cx_;
}

void VtScreen::sgr(const std::vector<int>& p) {
    if (p.empty()) { fg_ = kAnsi[7]; bg_ = 0; bold_ = false; return; }
    for (std::size_t i = 0; i < p.size(); ++i) {
        int v = p[i];
        if (v == 0) { fg_ = kAnsi[7]; bg_ = 0; bold_ = false; }
        else if (v == 1) bold_ = true;
        else if (v == 22) bold_ = false;
        else if (v >= 30 && v <= 37) fg_ = kAnsi[v - 30 + (bold_ ? 8 : 0)];
        else if (v >= 90 && v <= 97) fg_ = kAnsi[v - 90 + 8];
        else if (v >= 40 && v <= 47) bg_ = kAnsi[v - 40];
        else if (v >= 100 && v <= 107) bg_ = kAnsi[v - 100 + 8];
        else if (v == 39) fg_ = kAnsi[7];
        else if (v == 49) bg_ = 0;
        else if ((v == 38 || v == 48) && i + 1 < p.size()) {
            ImU32 col = 0;
            if (p[i + 1] == 5 && i + 2 < p.size()) { col = xterm256(p[i + 2]); i += 2; }
            else if (p[i + 1] == 2 && i + 4 < p.size()) { col = IM_COL32(p[i + 2], p[i + 3], p[i + 4], 255); i += 4; }
            (v == 38 ? fg_ : bg_) = col;
        }
    }
}

void VtScreen::csi(char f, const std::string& raw) {
    std::string params = raw;
    bool priv = !params.empty() && (params[0] == '?' || params[0] == '>');
    if (priv) params.erase(0, 1);
    std::vector<int> p;
    std::size_t s = 0;
    while (s <= params.size()) {
        auto e = params.find(';', s);
        std::string part = params.substr(s, e == std::string::npos ? std::string::npos : e - s);
        p.push_back(part.empty() ? -1 : std::atoi(part.c_str()));
        if (e == std::string::npos) break;
        s = e + 1;
    }
    auto arg = [&](std::size_t i, int def) { return i < p.size() && p[i] >= 0 ? p[i] : def; };
    if (priv) return;   // режимы DEC (?25h и пр.) — игнорируем
    switch (f) {
        case 'A': cy_ = std::max(0, cy_ - arg(0, 1)); break;
        case 'B': cy_ = std::min(rows_ - 1, cy_ + arg(0, 1)); break;
        case 'C': cx_ = std::min(cols_ - 1, cx_ + arg(0, 1)); break;
        case 'D': cx_ = std::max(0, cx_ - arg(0, 1)); break;
        case 'G': cx_ = std::clamp(arg(0, 1) - 1, 0, cols_ - 1); break;
        case 'H': case 'f': cy_ = std::clamp(arg(0, 1) - 1, 0, rows_ - 1); cx_ = std::clamp(arg(1, 1) - 1, 0, cols_ - 1); break;
        case 'J': {
            int m = arg(0, 0);
            auto clearRow = [&](int r, int from, int to) { for (int x = from; x < to; ++x) grid_[(std::size_t)r][(std::size_t)x] = TermCell{}; };
            if (m == 0) { clearRow(cy_, cx_, cols_); for (int r = cy_ + 1; r < rows_; ++r) clearRow(r, 0, cols_); }
            else if (m == 1) { for (int r = 0; r < cy_; ++r) clearRow(r, 0, cols_); clearRow(cy_, 0, cx_ + 1); }
            else { for (int r = 0; r < rows_; ++r) clearRow(r, 0, cols_); if (m == 3) scrollback_.clear(); }
            break;
        }
        case 'K': {
            int m = arg(0, 0);
            int from = m == 0 ? cx_ : 0, to = m == 1 ? cx_ + 1 : cols_;
            for (int x = from; x < std::min(to, cols_); ++x) grid_[(std::size_t)cy_][(std::size_t)x] = TermCell{};
            break;
        }
        case 'P': {   // удалить символы
            auto& r = grid_[(std::size_t)cy_];
            int n = std::min(arg(0, 1), cols_ - cx_);
            r.erase(r.begin() + cx_, r.begin() + cx_ + n); r.resize((std::size_t)cols_);
            break;
        }
        case '@': {   // вставить пробелы
            auto& r = grid_[(std::size_t)cy_];
            int n = std::min(arg(0, 1), cols_ - cx_);
            r.insert(r.begin() + cx_, (std::size_t)n, TermCell{}); r.resize((std::size_t)cols_);
            break;
        }
        case 'm': { std::vector<int> q; for (int v : p) q.push_back(v < 0 ? 0 : v); if (raw.empty()) q.clear(); sgr(q); break; }
        case 's': savedX_ = cx_; savedY_ = cy_; break;
        case 'u': cx_ = savedX_; cy_ = savedY_; break;
        default: break;
    }
}

void VtScreen::feed(std::string_view bytes) {
    for (char raw : bytes) {
        unsigned char c = (unsigned char)raw;
        switch (st_) {
            case St::Esc:
                if (c == '[') { st_ = St::Csi; params_.clear(); }
                else if (c == ']') { st_ = St::Osc; params_.clear(); }
                else if (c == '7') { savedX_ = cx_; savedY_ = cy_; st_ = St::Normal; }
                else if (c == '8') { cx_ = savedX_; cy_ = savedY_; st_ = St::Normal; }
                else st_ = St::Normal;
                continue;
            case St::Csi:
                if (c >= 0x40 && c <= 0x7E) { csi((char)c, params_); st_ = St::Normal; }
                else params_ += (char)c;
                continue;
            case St::Osc:    // заголовок окна и пр. — пропускаем до BEL или ESC '\'
                if (c == 0x07) st_ = St::Normal;
                else if (c == 0x1B) st_ = St::OscEsc;
                continue;
            case St::OscEsc: st_ = St::Normal; continue;
            case St::Normal: break;
        }
        // Сборка UTF-8 последовательностей
        if (utf8need_ > 0) {
            utf8acc_ += (char)c;
            if (--utf8need_ == 0) {
                unsigned int cp = 0;
                ImTextCharFromUtf8(&cp, utf8acc_.data(), utf8acc_.data() + utf8acc_.size());
                put((char32_t)cp);
                utf8acc_.clear();
            }
            continue;
        }
        if (c >= 0xC0) { utf8acc_ = (char)c; utf8need_ = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1; continue; }
        switch (c) {
            case 0x1B: st_ = St::Esc; break;
            case '\r': cx_ = 0; break;
            case '\n': newline(); break;
            case '\b': cx_ = std::max(0, cx_ - 1); break;
            case '\t': cx_ = std::min(cols_ - 1, (cx_ / 8 + 1) * 8); break;
            case 0x07: break;   // BEL
            default: if (c >= 32) put((char32_t)c); break;
        }
    }
}

std::string VtScreen::plainText() const {
    std::string out;
    auto dump = [&](const std::vector<TermCell>& r) {
        std::string line;
        for (auto& cell : r) { char b[5] = {}; ImTextCharToUtf8(b, (unsigned int)cell.ch); line += b; }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line; out += '\n';
    };
    for (auto& r : scrollback_) dump(r);
    for (auto& r : grid_) dump(r);
    return out;
}

// ---------------------------- TerminalSession --------------------------------
TerminalSession::~TerminalSession() { stop(); }

#ifdef _WIN32
bool TerminalSession::start(const std::filesystem::path& cwd, int cols, int rows, std::string* err) {
    // ConPTY: два pipe'а + псевдоконсоль, процесс получает её через атрибут
    HANDLE inR, inW, outR, outW;
    if (!CreatePipe(&inR, &inW, nullptr, 0) || !CreatePipe(&outR, &outW, nullptr, 0)) { if (err) *err = "CreatePipe"; return false; }
    using CreatePC = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, void**);
    auto fn = (CreatePC)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreatePseudoConsole");
    if (!fn) { if (err) *err = "ConPTY недоступен (требуется Windows 10 1809+)"; return false; }
    void* hpc = nullptr;
    if (FAILED(fn(COORD{(SHORT)cols, (SHORT)rows}, inR, outW, 0, &hpc))) { if (err) *err = "CreatePseudoConsole failed"; return false; }
    CloseHandle(inR); CloseHandle(outW);

    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si);
    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
    std::vector<char> attr(sz);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr.data();
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &sz);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, 0x00020016 /*PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE*/, hpc, sizeof(hpc), nullptr, nullptr);
    std::wstring cmd = L"powershell.exe -NoLogo";
    if (!std::filesystem::exists("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")) cmd = L"cmd.exe";
    PROCESS_INFORMATION pi{};
    std::wstring wcwd = cwd.wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi)) {
        if (err) *err = "CreateProcess(shell) failed"; return false;
    }
    CloseHandle(pi.hThread);
    hPC_ = hpc; hIn_ = inW; hOut_ = outR; hProc_ = pi.hProcess;
    screen.resize(cols, rows);
    running_ = true;
    reader_ = std::jthread([this](std::stop_token st) {
        std::array<char, 4096> buf{};
        DWORD n = 0;
        while (!st.stop_requested() && ReadFile((HANDLE)hOut_, buf.data(), (DWORD)buf.size(), &n, nullptr) && n > 0) {
            std::lock_guard lk(mtx);
            screen.feed(std::string_view(buf.data(), n));
        }
        running_ = false;
    });
    return true;
}
void TerminalSession::write(std::string_view d) { DWORD w; if (hIn_) WriteFile((HANDLE)hIn_, d.data(), (DWORD)d.size(), &w, nullptr); }
void TerminalSession::resize(int cols, int rows) {
    using ResizePC = HRESULT(WINAPI*)(void*, COORD);
    if (auto fn = (ResizePC)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ResizePseudoConsole"); fn && hPC_) fn(hPC_, COORD{(SHORT)cols, (SHORT)rows});
    std::lock_guard lk(mtx); screen.resize(cols, rows);
}
void TerminalSession::stop() {
    if (hProc_) { TerminateProcess((HANDLE)hProc_, 0); CloseHandle((HANDLE)hProc_); hProc_ = nullptr; }
    using ClosePC = void(WINAPI*)(void*);
    if (auto fn = (ClosePC)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ClosePseudoConsole"); fn && hPC_) fn(hPC_);
    hPC_ = nullptr;
    if (hIn_) { CloseHandle((HANDLE)hIn_); hIn_ = nullptr; }
    if (hOut_) { CloseHandle((HANDLE)hOut_); hOut_ = nullptr; }
    if (reader_.joinable()) { reader_.request_stop(); reader_.join(); }
}
#else
bool TerminalSession::start(const std::filesystem::path& cwd, int cols, int rows, std::string* err) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        if (err) *err = std::string("posix_openpt: ") + std::strerror(errno);
        if (master >= 0) close(master);
        return false;
    }
    const char* slaveName = ptsname(master);
    std::string slave = slaveName ? slaveName : "";
    pid_t pid = fork();
    if (pid < 0) { if (err) *err = "fork failed"; close(master); return false; }
    if (pid == 0) {
        // Дочерний: новая сессия, slave PTY становится управляющим терминалом
        setsid();
        int s = open(slave.c_str(), O_RDWR);
        if (s < 0) _exit(1);
#ifdef TIOCSCTTY
        ioctl(s, TIOCSCTTY, 0);
#endif
        winsize ws{(unsigned short)rows, (unsigned short)cols, 0, 0};
        ioctl(s, TIOCSWINSZ, &ws);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(master);
        if (!cwd.empty()) (void)!chdir(cwd.c_str());
        setenv("TERM", "xterm-256color", 1);
        const char* sh = std::getenv("SHELL");
        if (!sh || !*sh) sh = "/bin/bash";
        execl(sh, sh, "-i", (char*)nullptr);
        execl("/bin/sh", "sh", "-i", (char*)nullptr);
        _exit(127);
    }
    masterFd_ = master; pid_ = pid;
    screen.resize(cols, rows);
    running_ = true;
    reader_ = std::jthread([this](std::stop_token st) {
        std::array<char, 4096> buf{};
        while (!st.stop_requested()) {
            ssize_t n = ::read(masterFd_, buf.data(), buf.size());
            if (n > 0) { std::lock_guard lk(mtx); screen.feed(std::string_view(buf.data(), (std::size_t)n)); }
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        running_ = false;
    });
    return true;
}
void TerminalSession::write(std::string_view d) { if (masterFd_ >= 0) (void)!::write(masterFd_, d.data(), d.size()); }
void TerminalSession::resize(int cols, int rows) {
    if (masterFd_ >= 0) { winsize ws{(unsigned short)rows, (unsigned short)cols, 0, 0}; ioctl(masterFd_, TIOCSWINSZ, &ws); }
    std::lock_guard lk(mtx); screen.resize(cols, rows);
}
void TerminalSession::stop() {
    if (pid_ > 0) { kill(pid_, SIGHUP); waitpid(pid_, nullptr, WNOHANG); }
    if (masterFd_ >= 0) { close(masterFd_); masterFd_ = -1; }   // read() вернёт ошибку -> поток завершится
    if (reader_.joinable()) { reader_.request_stop(); reader_.join(); }
    if (pid_ > 0) { waitpid(pid_, nullptr, 0); pid_ = -1; }
}
#endif

// ---------------------------- TerminalEngine ---------------------------------
void TerminalEngine::newSession(const std::filesystem::path& cwd) {
    lastCwd_ = cwd;
    auto s = std::make_unique<TerminalSession>();
    std::string err;
    if (!s->start(cwd, 120, 30, &err)) {
        std::lock_guard lk(s->mtx);
        s->screen.feed("\x1b[31mНе удалось запустить терминал: " + err + "\x1b[0m\r\n");
    }
    sessions_.push_back(std::move(s));
    active_ = (int)sessions_.size() - 1;
}

void TerminalEngine::runCommand(const std::string& cmd) {
    if (sessions_.empty()) newSession(lastCwd_.empty() ? std::filesystem::current_path() : lastCwd_);
    sessions_[(std::size_t)active_]->write(cmd + "\r");
}

void TerminalEngine::render(const char* windowTitle, bool* open) {
    if (!ImGui::Begin(windowTitle, open)) { ImGui::End(); return; }
    if (sessions_.empty() && ImGui::Button("+ Terminal")) newSession(std::filesystem::current_path());
    if (ImGui::BeginTabBar("##terms", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_Reorderable)) {
        for (int i = 0; i < (int)sessions_.size(); ++i) {
            bool keep = true;
            std::string label = "shell " + std::to_string(i + 1) + (sessions_[(std::size_t)i]->running() ? "" : " (exited)") + "###t" + std::to_string(i);
            if (ImGui::BeginTabItem(label.c_str(), &keep)) { active_ = i; ImGui::EndTabItem(); }
            if (!keep) { sessions_.erase(sessions_.begin() + i); active_ = std::max(0, active_ - 1); --i; }
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) newSession(lastCwd_.empty() ? std::filesystem::current_path() : lastCwd_);
        ImGui::EndTabBar();
    }
    if (sessions_.empty()) { ImGui::End(); return; }
    auto& s = *sessions_[(std::size_t)std::clamp(active_, 0, (int)sessions_.size() - 1)];

    ImGui::SetWindowFontScale(fontScale);
    float cw = ImGui::CalcTextSize("M").x, lh = ImGui::GetTextLineHeight();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int cols = std::max(20, (int)(avail.x / cw) - 1), rows = std::max(5, (int)(avail.y / lh) - 1);

    ImGui::BeginChild("##termview", avail, ImGuiChildFlags_None, ImGuiWindowFlags_NoMove);
    {
        std::lock_guard lk(s.mtx);
        bool needResize = s.screen.cols() != cols || s.screen.rows() != rows;
        if (needResize) { s.mtx.unlock(); s.resize(cols, rows); s.mtx.lock(); }
        auto& scr = s.screen;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        int sb = (int)scr.scrollback().size();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(cw * (float)scr.cols(), lh * (float)(sb + scr.rows())));
        dl->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), ImGui::GetWindowPos().y + ImGui::GetWindowHeight()), background);
        auto drawRow = [&](const std::vector<TermCell>& r, float y) {
            for (int x = 0; x < (int)r.size(); ++x) {
                const auto& c = r[(std::size_t)x];
                ImVec2 p(origin.x + (float)x * cw, y);
                if (c.bg) dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + lh), c.bg);
                if (c.ch != U' ') { char b[5] = {}; ImTextCharToUtf8(b, (unsigned int)c.ch); dl->AddText(p, c.fg, b); }
            }
        };
        float scrollY = ImGui::GetScrollY(), winH = ImGui::GetWindowHeight();
        int first = std::max(0, (int)(scrollY / lh)), last = std::min(sb + scr.rows() - 1, (int)((scrollY + winH) / lh) + 1);
        for (int i = first; i <= last; ++i)
            drawRow(i < sb ? scr.scrollback()[(std::size_t)i] : scr.row(i - sb), origin.y + (float)i * lh);
        // Курсор
        ImVec2 cp(origin.x + (float)scr.cursorX() * cw, origin.y + (float)(sb + scr.cursorY()) * lh);
        if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + lh), IM_COL32(200, 200, 200, 160));
        // Автопрокрутка к низу при новом выводе
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - lh * 2) ImGui::SetScrollHereY(1.0f);
    }
    // Клавиатурный ввод -> PTY
    if (ImGui::IsWindowFocused()) {
        ImGuiIO& io = ImGui::GetIO();
        io.WantCaptureKeyboard = true; io.WantTextInput = true;
        std::string out;
        for (ImWchar ch : io.InputQueueCharacters) { char b[5] = {}; ImTextCharToUtf8(b, ch); out += b; }
        io.InputQueueCharacters.resize(0);
        auto key = [&](ImGuiKey k, const char* seq) { if (ImGui::IsKeyPressed(k)) out += seq; };
        key(ImGuiKey_Enter, "\r"); key(ImGuiKey_KeypadEnter, "\r"); key(ImGuiKey_Backspace, "\x7f"); key(ImGuiKey_Tab, "\t");
        key(ImGuiKey_Escape, "\x1b"); key(ImGuiKey_UpArrow, "\x1b[A"); key(ImGuiKey_DownArrow, "\x1b[B");
        key(ImGuiKey_RightArrow, "\x1b[C"); key(ImGuiKey_LeftArrow, "\x1b[D"); key(ImGuiKey_Home, "\x1b[H");
        key(ImGuiKey_End, "\x1b[F"); key(ImGuiKey_Delete, "\x1b[3~");
        if (io.KeyCtrl) {   // Ctrl+A..Z -> управляющие коды 0x01..0x1A
            for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k)
                if (ImGui::IsKeyPressed((ImGuiKey)k) && !(io.KeyShift && k == ImGuiKey_V)) out += (char)(1 + k - ImGuiKey_A);
            if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_V)) if (const char* clip = ImGui::GetClipboardText()) out += clip;
        }
        if (!out.empty()) s.write(out);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ide
