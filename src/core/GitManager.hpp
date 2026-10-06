// =============================================================================
//  GitManager.hpp — графический Git-клиент поверх git CLI:
//  статус, staging (stage/unstage), коммит, ветки, push/pull, граф истории,
//  Diff-просмотрщик (side-by-side и inline). Все операции асинхронные.
//  Выбор git CLI вместо libgit2 — осознанный: полная совместимость с
//  credential helpers, SSH-агентом, hooks и LFS пользователя.
// =============================================================================
#pragma once
#include <imgui.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ide {

struct GitFileStatus {
    std::string path;
    char        index    = ' ';   // статус в индексе (staged)
    char        worktree = ' ';   // статус в рабочем дереве
    [[nodiscard]] bool staged() const { return index != ' ' && index != '?'; }
    [[nodiscard]] bool unstaged() const { return worktree != ' ' || index == '?'; }
};

struct GitCommit {
    std::string graph;     // ASCII-граф из git log --graph
    std::string hash, author, date, refs, subject;
};

struct DiffLine {
    enum Kind { Context, Added, Removed, Hunk } kind = Context;
    int oldNo = -1, newNo = -1;
    std::string text;
};

struct FileDiff {
    std::string           path;
    bool                  staged = false;
    std::vector<DiffLine> lines;
};

class GitManager {
public:
    void setRepository(const std::filesystem::path& root);
    [[nodiscard]] bool isRepo() const { return isRepo_; }
    [[nodiscard]] const std::string& branch() const { return branch_; }

    // Асинхронные операции (результат применяется в UI-потоке)
    void refresh();
    void stage(const std::string& path);
    void unstage(const std::string& path);
    void stageAll();
    void commit(const std::string& message, bool amend = false);
    void checkout(const std::string& branch, bool create = false);
    void push();
    void pull();
    void fetch();
    void initRepo();
    void openDiff(const std::string& path, bool staged);

    void render(const char* title, bool* open);
    void renderDiffWindow(const char* title);

    static std::vector<DiffLine> parseUnifiedDiff(const std::string& diff);
    static std::vector<GitFileStatus> parsePorcelain(const std::string& out);

    std::function<void(const std::string&)> log;    // вывод в панель Output
    std::function<void(const std::filesystem::path&)> openFile;

private:
    void runGit(std::vector<std::string> args, std::function<void(int, std::string)> done, bool refreshAfter = true);

    std::filesystem::path       root_;
    bool                        isRepo_ = false;
    std::string                 branch_;
    std::vector<std::string>    branches_;
    std::vector<GitFileStatus>  files_;
    std::vector<GitCommit>      history_;
    FileDiff                    diff_;
    bool                        diffOpen_ = false, sideBySide_ = true, amend_ = false;
    std::string                 commitMsg_;
    std::string                 newBranch_;
    std::atomic<int>            busy_{0};
    int                         ahead_ = 0, behind_ = 0;
};

} // namespace ide
