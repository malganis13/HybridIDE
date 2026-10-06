// =============================================================================
//  BuildSystem.cpp
// =============================================================================
#include "core/BuildSystem.hpp"

#include "core/Async.hpp"
#include "ui/Localization.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>
#include <regex>
#include <sstream>

namespace ide {

namespace fs = std::filesystem;

ProjectKind BuildSystem::detect(const fs::path& root) {
    std::error_code ec;
    auto has = [&](const char* f) { return fs::exists(root / f, ec); };
    if (has("CMakeLists.txt")) return ProjectKind::CMake;
    if (has("Cargo.toml"))     return ProjectKind::Cargo;
    if (has("go.mod"))         return ProjectKind::Go;
    if (has("package.json"))   return ProjectKind::Node;
    if (has("pyproject.toml") || has("setup.py") || has("requirements.txt")) return ProjectKind::Python;
    if (has("Makefile"))       return ProjectKind::Make;
    return ProjectKind::Unknown;
}

const char* BuildSystem::kindName(ProjectKind k) {
    switch (k) {
        case ProjectKind::CMake: return "CMake";   case ProjectKind::Cargo: return "Cargo";
        case ProjectKind::Python: return "Python"; case ProjectKind::Go: return "Go";
        case ProjectKind::Node: return "Node.js";  case ProjectKind::Make: return "Make";
        default: return "—";
    }
}

BuildProfile BuildSystem::profileFor(ProjectKind k, const std::string& cfg) {
    BuildProfile p;
    const bool rel = cfg == "Release";
    switch (k) {
        case ProjectKind::CMake:
            p.configure = "cmake -S . -B build -DCMAKE_BUILD_TYPE=" + cfg + " -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";
            p.build     = "cmake --build build --config " + cfg + " --parallel";
            p.clean     = "cmake --build build --target clean";
            p.test      = "ctest --test-dir build -C " + cfg + " --output-on-failure";
            p.run       = "";   // определяется по найденному исполняемому файлу
            break;
        case ProjectKind::Cargo:
            p.build = rel ? "cargo build --release" : "cargo build";
            p.run   = rel ? "cargo run --release"   : "cargo run";
            p.clean = "cargo clean"; p.test = "cargo test";
            break;
        case ProjectKind::Go:
            p.build = "go build ./..."; p.run = "go run ."; p.clean = "go clean"; p.test = "go test ./...";
            break;
        case ProjectKind::Node:
            p.configure = "npm install"; p.build = "npm run build"; p.run = "npm start"; p.test = "npm test";
            break;
        case ProjectKind::Python:
#ifdef _WIN32
            p.run = "python -m main";
#else
            p.run = "python3 -m main";
#endif
            p.test = "python -m pytest"; p.build = "python -m compileall -q .";
            break;
        case ProjectKind::Make:
            p.build = "make -j"; p.clean = "make clean"; p.run = "make run"; p.test = "make test";
            break;
        default: break;
    }
    return p;
}

std::vector<Problem> BuildSystem::parseOutput(const std::string& text, const fs::path& root) {
    std::vector<Problem> out;
    // GCC/Clang/Go:  path:line:col: error: message
    static const std::regex gcc(R"(^\s*([^\s:][^:]*|[A-Za-z]:[^:]+):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note)?:?\s*(.*)$)");
    // MSVC / tsc:    path(line,col): error C2065: message
    static const std::regex msvc(R"(^\s*(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(fatal error|error|warning|note)\s*([A-Z]+\d+)?\s*:?\s*(.*)$)");
    // rustc:         error[E0308]: message  \n  --> src/main.rs:4:5
    static const std::regex rustHead(R"(^(error|warning)(?:\[(E\d+)\])?:\s*(.*)$)");
    static const std::regex rustLoc(R"(^\s*-->\s*(.+?):(\d+):(\d+))");
    // Python:        File "x.py", line 12
    static const std::regex py(R"re(^\s*File "(.+?)", line (\d+))re");

    std::istringstream ss(text);
    std::string line;
    std::optional<Problem> pendingRust;
    auto sevOf = [](const std::string& s) { return s.find("error") != std::string::npos ? 1 : s == "warning" ? 2 : 3; };
    auto resolve = [&](const std::string& f) {
        fs::path p = fs::u8path(f);
        if (p.is_relative()) p = root / p;
        return p.lexically_normal();
    };
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (pendingRust && std::regex_search(line, m, rustLoc)) {
            pendingRust->file = resolve(m[1]); pendingRust->line = std::stoi(m[2]); pendingRust->col = std::stoi(m[3]);
            out.push_back(*pendingRust); pendingRust.reset(); continue;
        }
        if (std::regex_match(line, m, rustHead)) {
            if (line.find("aborting due to") != std::string::npos || line.find("could not compile") != std::string::npos) continue;
            Problem p; p.severity = sevOf(m[1]); p.code = m[2]; p.message = m[3];
            pendingRust = p; continue;
        }
        if (std::regex_match(line, m, msvc)) {
            Problem p; p.file = resolve(m[1]); p.line = std::stoi(m[2]); p.col = m[3].matched ? std::stoi(m[3]) : 1;
            p.severity = sevOf(m[4]); p.code = m[5]; p.message = m[6];
            out.push_back(p); continue;
        }
        if (std::regex_match(line, m, gcc) && m[4].matched) {
            Problem p; p.file = resolve(m[1]); p.line = std::stoi(m[2]); p.col = m[3].matched ? std::stoi(m[3]) : 1;
            p.severity = sevOf(m[4]); p.message = m[5];
            out.push_back(p); continue;
        }
        if (std::regex_search(line, m, py)) {
            Problem p; p.file = resolve(m[1]); p.line = std::stoi(m[2]); p.col = 1; p.severity = 1; p.message = "Traceback";
            out.push_back(p);
        }
    }
    return out;
}

void BuildSystem::setRoot(const fs::path& root) { root_ = root; kind_ = detect(root); }

void BuildSystem::appendOutput(std::string_view text) {
    std::lock_guard lk(outMtx_);
    output_.append(text);
    if (output_.size() > 4 * 1024 * 1024) output_.erase(0, output_.size() - 3 * 1024 * 1024);   // кольцевой лимит
}

void BuildSystem::clearOutput() { std::lock_guard lk(outMtx_); output_.clear(); }

void BuildSystem::startSequence(std::vector<std::string> commands, const std::string& label) {
    if (running_) { appendOutput("[!] Задача уже выполняется\n"); return; }
    commands.erase(std::remove(commands.begin(), commands.end(), std::string{}), commands.end());
    if (commands.empty()) { appendOutput("[!] Нет команды для: " + label + "\n"); return; }
    running_ = true; progress_ = 0.f;
    problems_.clear();
    clearOutput();
    appendOutput("> " + label + " [" + kindName(kind_) + ", " + config + "]\n");
    auto root = root_;
    auto t0 = std::chrono::steady_clock::now();
    // Цепочка команд выполняется в пуле; вывод стримится в Output по мере поступления
    ThreadPool::global().submit([this, commands, root, t0] {
        bool ok = true;
        for (const auto& cmd : commands) {
            appendOutput("$ " + cmd + "\n");
            ProcessOptions o; o.argv = splitCommandLine(cmd); o.cwd = root; o.mergeStderr = true;
            o.env["CLICOLOR_FORCE"] = "0";
            auto p = std::make_unique<Process>();
            std::atomic<bool> done{false};
            int code = -1;
            std::string all;
            bool started = p->start(o,
                [&](std::string_view d) {
                    appendOutput(d);
                    all.append(d);
                    // Прогресс по шаблону "[12/40]" (ninja/make/cmake)
                    auto lb = all.rfind('['), sl = all.rfind('/'), rb = all.rfind(']');
                    if (lb != std::string::npos && sl > lb && rb > sl) {
                        try { float a = std::stof(all.substr(lb + 1, sl - lb - 1)), b = std::stof(all.substr(sl + 1, rb - sl - 1)); if (b > 0) progress_ = a / b; } catch (...) {}
                    }
                },
                {}, [&](int c) { code = c; done = true; });
            if (!started) { appendOutput("[x] " + p->lastError() + "\n"); ok = false; break; }
            {   // Публикуем процесс, чтобы cancel() мог его прервать
                std::lock_guard lk(procMtx_);
                current_ = p.get();
            }
            while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(15));
            {
                std::lock_guard lk(procMtx_);
                current_ = nullptr;
            }
            if (cancelled_.exchange(false)) { appendOutput("[x] cancelled\n"); ok = false; break; }
            auto probs = parseOutput(all, root);
            postToMain([this, probs] {
                problems_.insert(problems_.end(), probs.begin(), probs.end());
                if (onProblems) onProblems(problems_);
            });
            if (code != 0) { appendOutput("[x] exit code " + std::to_string(code) + "\n"); ok = false; break; }
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        appendOutput(ok ? "[ok] " : "[FAILED] ");
        appendOutput(std::to_string(secs).substr(0, 5) + " s\n");
        progress_ = 1.f;
        running_ = false;
        postToMain([this, ok, secs] { if (onFinished) onFinished(ok, secs); });
    });
}

void BuildSystem::build() {
    auto p = profileFor(kind_, config);
    if (!customBuild.empty()) { startSequence({customBuild}, "Build"); return; }
    std::error_code ec;
    std::vector<std::string> cmds;
    if (kind_ == ProjectKind::CMake && !fs::exists(root_ / "build" / "CMakeCache.txt", ec)) cmds.push_back(p.configure);
    if (kind_ == ProjectKind::Node && !fs::exists(root_ / "node_modules", ec)) cmds.push_back(p.configure);
    cmds.push_back(p.build);
    startSequence(cmds, "Build");
}

void BuildSystem::run(const std::string& args) {
    if (!customRun.empty()) { startSequence({customRun + (args.empty() ? "" : " " + args)}, "Run"); return; }
    auto p = profileFor(kind_, config);
    if (kind_ == ProjectKind::CMake) {
        // Ищем самый свежий исполняемый файл в build/
        std::error_code ec;
        fs::path best; fs::file_time_type bestT{};
        if (fs::exists(root_ / "build", ec))
            for (auto& e : fs::recursive_directory_iterator(root_ / "build", fs::directory_options::skip_permission_denied, ec)) {
                if (!e.is_regular_file(ec) || e.path().string().find("CMakeFiles") != std::string::npos) continue;
#ifdef _WIN32
                bool exe = e.path().extension() == ".exe";
#else
                auto perms = e.status(ec).permissions();
                bool exe = (perms & fs::perms::owner_exec) != fs::perms::none && e.path().extension() != ".so" && e.path().extension() != ".sh";
#endif
                if (exe && e.last_write_time(ec) > bestT) { best = e.path(); bestT = e.last_write_time(ec); }
            }
        if (best.empty()) { appendOutput("[!] Исполняемый файл не найден — сначала выполните сборку\n"); return; }
        p.run = "\"" + best.string() + "\"";
    }
    startSequence({p.run + (args.empty() ? "" : " " + args)}, "Run");
}

void BuildSystem::clean() { startSequence({profileFor(kind_, config).clean}, "Clean"); }
void BuildSystem::test()  { startSequence({profileFor(kind_, config).test}, "Test"); }
void BuildSystem::runCustom(const std::string& label, const std::string& command) { startSequence({command}, label); }
void BuildSystem::cancel() {
    std::lock_guard lk(procMtx_);
    if (current_) { cancelled_ = true; current_->terminate(); }
}

void BuildSystem::renderOutput(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    if (ImGui::SmallButton(L.tr("output.clear"))) clearOutput();
    ImGui::SameLine(); ImGui::Checkbox(L.tr("output.autoscroll"), &autoScroll_);
    if (running_) { ImGui::SameLine(); ImGui::ProgressBar(progress_, ImVec2(160, 0)); }
    ImGui::Separator();
    ImGui::BeginChild("##out", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard lk(outMtx_);
        // Подсветка строк с ошибками/предупреждениями
        std::size_t start = 0;
        while (start < output_.size()) {
            std::size_t end = output_.find('\n', start);
            if (end == std::string::npos) end = output_.size();
            std::string_view l(output_.data() + start, end - start);
            ImVec4 col(0.85f, 0.85f, 0.85f, 1);
            if (l.find("error") != std::string_view::npos || l.rfind("[x]", 0) == 0 || l.rfind("[FAILED]", 0) == 0) col = ImVec4(1, .45f, .45f, 1);
            else if (l.find("warning") != std::string_view::npos) col = ImVec4(1, .8f, .3f, 1);
            else if (l.rfind("[ok]", 0) == 0) col = ImVec4(.4f, 1, .5f, 1);
            else if (l.rfind("$ ", 0) == 0 || l.rfind("> ", 0) == 0) col = ImVec4(.5f, .75f, 1, 1);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(l.data(), l.data() + l.size());
            ImGui::PopStyleColor();
            start = end + 1;
        }
    }
    if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

void BuildSystem::renderProblems(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    int errs = 0, warns = 0;
    for (auto& p : problems_) (p.severity == 1 ? errs : warns)++;
    ImGui::Text("%s: %d   %s: %d", L.tr("problems.errors"), errs, L.tr("problems.warnings"), warns);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    const char* items[] = {L.tr("problems.only_errors"), L.tr("problems.errors_warnings"), L.tr("problems.all")};
    int sel = filterSeverity_ - 1;
    if (ImGui::Combo("##sev", &sel, items, 3)) filterSeverity_ = sel + 1;
    if (ImGui::BeginTable("##problems", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 20);
        ImGui::TableSetupColumn(L.tr("problems.message"));
        ImGui::TableSetupColumn(L.tr("problems.file"), ImGuiTableColumnFlags_WidthFixed, 220);
        ImGui::TableSetupColumn(L.tr("problems.line"), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < problems_.size(); ++i) {
            auto& p = problems_[i];
            if (p.severity > filterSeverity_) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(p.severity == 1 ? ImVec4(1, .35f, .35f, 1) : p.severity == 2 ? ImVec4(1, .8f, .3f, 1) : ImVec4(.5f, .7f, 1, 1),
                               "%s", p.severity == 1 ? "E" : p.severity == 2 ? "W" : "i");
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            std::string msg = (p.code.empty() ? "" : p.code + ": ") + p.message;
            // Кликабельная строка -> переход к файлу/строке в редакторе
            if (ImGui::Selectable(msg.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && onNavigate)
                onNavigate(p.file, std::max(0, p.line - 1), std::max(0, p.col - 1));
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.file.filename().u8string().c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.file.u8string().c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d:%d", p.line, p.col);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace ide
