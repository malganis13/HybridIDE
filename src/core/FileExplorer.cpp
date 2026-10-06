// =============================================================================
//  FileExplorer.cpp
// =============================================================================
#include "core/JThread.hpp"
#include "core/FileExplorer.hpp"

#include "core/EventQueue.hpp"
#include "ui/Localization.hpp"

#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <fstream>

namespace ide {

namespace fs = std::filesystem;

void FileExplorer::setRoot(const fs::path& root) {
    stopWatcher();
    root_ = root;
    rescan();
    startWatcher();
}

FsNode FileExplorer::scan(const fs::path& p, int depth) {
    FsNode n;
    n.path = p; n.name = p.filename().u8string(); n.dir = true;
    if (depth > 24) return n;   // защита от циклических симлинков
    std::error_code ec;
    for (auto it = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto& e = *it;
        std::string name = e.path().filename().u8string();
        if (std::find(ignored.begin(), ignored.end(), name) != ignored.end()) continue;
        if (e.is_directory(ec)) n.children.push_back(scan(e.path(), depth + 1));
        else {
            FsNode f; f.path = e.path(); f.name = name; f.size = e.file_size(ec);
            n.children.push_back(std::move(f));
        }
    }
    // Каталоги сверху, затем по имени без учёта регистра
    std::sort(n.children.begin(), n.children.end(), [](const FsNode& a, const FsNode& b) {
        if (a.dir != b.dir) return a.dir;
        std::string x = a.name, y = b.name;
        std::transform(x.begin(), x.end(), x.begin(), ::tolower);
        std::transform(y.begin(), y.end(), y.begin(), ::tolower);
        return x < y;
    });
    return n;
}

void FileExplorer::rescan() {
    if (root_.empty()) return;
    FsNode t = scan(root_, 0);
    std::lock_guard lk(treeMtx_);
    tree_ = std::move(t);
}

void FileExplorer::startWatcher() {
    // Polling-наблюдатель: переносимый (Windows/Linux/macOS) и не требующий
    // inotify/ReadDirectoryChangesW; интервал 1 с, сравнение mtime снимков.
    watcher_ = ide::jthread([this, root = root_](ide::stop_token st) {
        auto takeSnapshot = [&](std::map<std::string, fs::file_time_type>& snap) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::string name = it->path().filename().u8string();
                if (std::find(ignored.begin(), ignored.end(), name) != ignored.end()) { it.disable_recursion_pending(); continue; }
                snap[it->path().u8string()] = it->last_write_time(ec);
                if (st.stop_requested()) return;
            }
        };
        takeSnapshot(snapshot_);
        while (!st.stop_requested()) {
            for (int i = 0; i < 10 && !st.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::map<std::string, fs::file_time_type> now;
            takeSnapshot(now);
            if (st.stop_requested()) return;
            bool structural = now.size() != snapshot_.size();
            std::vector<fs::path> modified;
            for (auto& [k, t] : now) {
                auto it = snapshot_.find(k);
                if (it == snapshot_.end()) structural = true;
                else if (it->second != t) modified.push_back(fs::u8path(k));
            }
            snapshot_ = std::move(now);
            if (structural) { rescan(); }
            for (auto& m : modified) postToMain([this, m] { if (onFileChangedOnDisk) onFileChangedOnDisk(m); });
        }
    });
}

void FileExplorer::stopWatcher() {
    if (watcher_.joinable()) { watcher_.request_stop(); watcher_.join(); }
}

static const char* iconFor(const FsNode& n) {
    if (n.dir) return "[D]";
    auto e = n.path.extension().string();
    if (e == ".cpp" || e == ".cc" || e == ".c") return "c++";
    if (e == ".hpp" || e == ".h") return " h ";
    if (e == ".rs") return " rs";
    if (e == ".py") return " py";
    if (e == ".go") return " go";
    if (e == ".ts" || e == ".js") return " js";
    if (e == ".json") return "{ }";
    if (e == ".md") return " md";
    return "   ";
}

void FileExplorer::drawNode(const FsNode& n) {
    auto& L = Localization::instance();
    ImGui::PushID(n.path.u8string().c_str());
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (!n.dir) fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected_ == n.path) fl |= ImGuiTreeNodeFlags_Selected;
    if (&n == &tree_) fl |= ImGuiTreeNodeFlags_DefaultOpen;
    std::string label = std::string(iconFor(n)) + " " + n.name;
    bool opened = ImGui::TreeNodeEx("##node", fl, "%s", label.c_str());
    if (ImGui::IsItemClicked()) selected_ = n.path;
    if (!n.dir && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && onOpenFile) onOpenFile(n.path);
    // Drag-source: перетаскивание файла в редактор/терминал
    if (ImGui::BeginDragDropSource()) {
        std::string s = n.path.u8string();
        ImGui::SetDragDropPayload("IDE_FILE_PATH", s.c_str(), s.size() + 1);
        ImGui::TextUnformatted(n.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("##ctx")) {
        fs::path parent = n.dir ? n.path : n.path.parent_path();
        if (!n.dir && ImGui::MenuItem(L.tr("explorer.open")) && onOpenFile) onOpenFile(n.path);
        if (ImGui::MenuItem(L.tr("explorer.new_file")))   { pendingParent_ = parent; creatingDir_ = false; nameBuf_.clear(); openCreate_ = true; }
        if (ImGui::MenuItem(L.tr("explorer.new_folder"))) { pendingParent_ = parent; creatingDir_ = true;  nameBuf_.clear(); openCreate_ = true; }
        ImGui::Separator();
        if (ImGui::MenuItem(L.tr("explorer.rename"))) { pendingRename_ = n.path; nameBuf_ = n.name; openRename_ = true; }
        if (ImGui::MenuItem(L.tr("explorer.delete"))) { pendingDelete_ = n.path; openDelete_ = true; }
        if (ImGui::MenuItem(L.tr("explorer.copy_path"))) ImGui::SetClipboardText(n.path.u8string().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(L.tr("menu.build")) && onBuild) onBuild();
        if (ImGui::MenuItem(L.tr("menu.run")) && onRun) onRun();
        if (ImGui::MenuItem(L.tr("menu.clean")) && onClean) onClean();
        ImGui::EndPopup();
    }
    if (n.dir && opened) {
        for (auto& c : n.children) drawNode(c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void FileExplorer::render(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    if (root_.empty()) { ImGui::TextDisabled("%s", L.tr("explorer.no_folder")); ImGui::End(); return; }
    if (ImGui::SmallButton(L.tr("explorer.refresh"))) rescan();
    ImGui::SameLine(); ImGui::TextDisabled("%s", root_.filename().u8string().c_str());
    ImGui::Separator();
    {
        std::lock_guard lk(treeMtx_);
        drawNode(tree_);
    }

    // --- Модальные диалоги ---
    if (openCreate_) { ImGui::OpenPopup("##create"); openCreate_ = false; }
    if (openRename_) { ImGui::OpenPopup("##rename"); openRename_ = false; }
    if (openDelete_) { ImGui::OpenPopup("##delete"); openDelete_ = false; }
    if (ImGui::BeginPopupModal("##create", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", creatingDir_ ? L.tr("explorer.new_folder") : L.tr("explorer.new_file"));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool go = ImGui::InputText("##name", &nameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("OK") || go) && !nameBuf_.empty()) {
            std::error_code ec;
            fs::path target = pendingParent_ / fs::u8path(nameBuf_);
            if (creatingDir_) fs::create_directories(target, ec);
            else { fs::create_directories(target.parent_path(), ec); std::ofstream(target).close(); if (onOpenFile) onOpenFile(target); }
            rescan();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(L.tr("common.cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("##rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool go = ImGui::InputText("##name", &nameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("OK") || go) && !nameBuf_.empty()) {
            std::error_code ec;
            fs::rename(pendingRename_, pendingRename_.parent_path() / fs::u8path(nameBuf_), ec);
            rescan();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(L.tr("common.cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("##delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s\n%s", L.tr("explorer.confirm_delete"), pendingDelete_.u8string().c_str());
        if (ImGui::Button(L.tr("explorer.delete"))) {
            std::error_code ec;
            fs::remove_all(pendingDelete_, ec);
            if (onDeleted) onDeleted(pendingDelete_);
            rescan();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(L.tr("common.cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

void FileExplorer::renderProperties(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    if (selected_.empty()) { ImGui::TextDisabled("%s", L.tr("props.none")); ImGui::End(); return; }
    std::error_code ec;
    auto st = fs::status(selected_, ec);
    ImGui::Text("%s: %s", L.tr("props.name"), selected_.filename().u8string().c_str());
    ImGui::TextWrapped("%s: %s", L.tr("props.path"), selected_.u8string().c_str());
    ImGui::Text("%s: %s", L.tr("props.type"), fs::is_directory(st) ? L.tr("props.folder") : selected_.extension().u8string().c_str());
    if (fs::is_regular_file(st)) ImGui::Text("%s: %ju B", L.tr("props.size"), (std::uintmax_t)fs::file_size(selected_, ec));
    auto perms = st.permissions();
    ImGui::Text("%s: %c%c%c", L.tr("props.perms"),
                (perms & fs::perms::owner_read) != fs::perms::none ? 'r' : '-',
                (perms & fs::perms::owner_write) != fs::perms::none ? 'w' : '-',
                (perms & fs::perms::owner_exec) != fs::perms::none ? 'x' : '-');
    ImGui::End();
}

} // namespace ide
