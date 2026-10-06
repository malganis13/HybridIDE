// =============================================================================
//  FileExplorer.hpp — дерево решения/проекта с наблюдателем файловой системы
//  (фоновый ide::jthread сравнивает снимки std::filesystem), контекстными
//  меню (Create/Rename/Delete/Build/Run/Clean) и инспектором свойств файла.
// =============================================================================
#pragma once
#include "core/JThread.hpp"
#include <imgui.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ide {

struct FsNode {
    std::filesystem::path path;
    std::string           name;
    bool                  dir = false;
    std::uintmax_t        size = 0;
    std::vector<FsNode>   children;
};

class FileExplorer {
public:
    ~FileExplorer() { stopWatcher(); }
    void setRoot(const std::filesystem::path& root);
    void rescan();
    void render(const char* title, bool* open);
    void renderProperties(const char* title, bool* open);

    std::function<void(const std::filesystem::path&)> onOpenFile;
    std::function<void(const std::filesystem::path&)> onFileChangedOnDisk;
    std::function<void()>                             onBuild, onRun, onClean;
    std::function<void(const std::filesystem::path&)> onDeleted;

    std::vector<std::string> ignored{".git", "build", "node_modules", "target", "__pycache__", ".venv", ".cache", "cmake-build-debug", "cmake-build-release"};

private:
    FsNode scan(const std::filesystem::path& p, int depth);
    void   drawNode(const FsNode& n);
    void   startWatcher();
    void   stopWatcher();

    std::filesystem::path root_;
    FsNode                tree_;
    std::mutex            treeMtx_;
    ide::jthread          watcher_;
    std::atomic<bool>     dirty_{false};
    std::map<std::string, std::filesystem::file_time_type> snapshot_;
    std::filesystem::path selected_;
    // Состояние модальных диалогов
    std::filesystem::path pendingParent_, pendingRename_, pendingDelete_;
    std::string           nameBuf_;
    bool                  creatingDir_ = false;
    bool openCreate_ = false, openRename_ = false, openDelete_ = false;
};

} // namespace ide
