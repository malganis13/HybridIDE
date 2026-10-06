// =============================================================================
//  BuildSystem.hpp — оркестрация сборки (аналог MSBuild/tasks.json):
//  автоопределение типа проекта, команды Build/Run/Clean, потоковый вывод
//  в панель Output и разбор ошибок компиляторов в панель Problems.
// =============================================================================
#pragma once
#include "core/Process.hpp"

#include <imgui.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ide {

enum class ProjectKind { Unknown, CMake, Cargo, Python, Go, Node, Make };

struct Problem {
    std::filesystem::path file;
    int         line = 0, col = 0;   // 1-based, как в выводе компиляторов
    int         severity = 1;        // 1 error, 2 warning, 3 note
    std::string message;
    std::string code;                // C2065, E0308, TS2304 ...
};

struct BuildProfile {
    std::string configure, build, run, clean, test;
};

class BuildSystem {
public:
    static ProjectKind detect(const std::filesystem::path& root);
    static BuildProfile profileFor(ProjectKind k, const std::string& config);
    static const char* kindName(ProjectKind k);

    // Разбор строки вывода: GCC/Clang, MSVC, rustc, Go, tsc, Python traceback
    static std::vector<Problem> parseOutput(const std::string& text, const std::filesystem::path& root);

    void setRoot(const std::filesystem::path& root);
    void build();
    void run(const std::string& args = {});
    void clean();
    void test();
    void runCustom(const std::string& label, const std::string& command);
    void cancel();

    [[nodiscard]] bool busy() const { return running_.load(); }
    [[nodiscard]] float progress() const { return progress_.load(); }  // 0..1 по выводу [n/m]
    [[nodiscard]] ProjectKind kind() const { return kind_; }
    std::string config = "Debug";
    std::string customBuild, customRun;

    void renderOutput(const char* title, bool* open);
    void renderProblems(const char* title, bool* open);

    std::function<void(bool success, double seconds)>          onFinished;   // звук пара / уведомление
    std::function<void(const std::filesystem::path&, int, int)> onNavigate;  // клик по ошибке
    std::function<void(const std::vector<Problem>&)>           onProblems;

    void appendOutput(std::string_view text);
    void clearOutput();
    [[nodiscard]] const std::vector<Problem>& problems() const { return problems_; }

private:
    void startSequence(std::vector<std::string> commands, const std::string& label);

    std::filesystem::path     root_;
    ProjectKind               kind_ = ProjectKind::Unknown;
    std::mutex                procMtx_;
    Process*                  current_ = nullptr;
    std::atomic<bool>         cancelled_{false};
    std::atomic<bool>         running_{false};
    std::atomic<float>        progress_{0.f};
    std::mutex                outMtx_;
    std::string               output_;
    std::string               pendingLine_;
    std::vector<Problem>      problems_;
    bool                      autoScroll_ = true;
    int                       filterSeverity_ = 3;
};

} // namespace ide
