// ============================================================================
//  FileDialog.cpp
// ============================================================================
#include "ui/FileDialog.hpp"
#include "ui/Localization.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace ide {

namespace fs = std::filesystem;

void FileDialog::open(Mode mode, const fs::path& start, std::function<void(const fs::path&)> onOk) {
    mode_ = mode;
    std::error_code ec;
    dir_ = start.empty() ? fs::current_path(ec) : start;
    if (!fs::is_directory(dir_, ec)) dir_ = dir_.parent_path();
    pathInput_ = dir_.string();
    nameInput_.clear();
    error_.clear();
    onOk_ = std::move(onOk);
    visible_ = true;
    justOpened_ = true;
    list();
}

void FileDialog::list() {
    entries_.clear();
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        auto name = it->path().filename().string();
        if (!showHidden_ && !name.empty() && name[0] == '.') continue;
        if (mode_ == Mode::OpenFolder && !it->is_directory(ec)) continue;
        entries_.push_back(*it);
    }
    std::sort(entries_.begin(), entries_.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        std::error_code e1, e2;
        bool da = a.is_directory(e1), db = b.is_directory(e2);
        if (da != db) return da;
        return a.path().filename().string() < b.path().filename().string();
    });
    if (ec) error_ = ec.message();
}

void FileDialog::render() {
    if (!visible_) return;
    auto& L = Localization::instance();
    const char* title = mode_ == Mode::OpenFolder ? L.tr("dialog.open_folder")
                      : mode_ == Mode::SaveFile   ? L.tr("dialog.save_file") : L.tr("dialog.open_file");
    if (justOpened_) { ImGui::OpenPopup("###fileDialog"); justOpened_ = false; }
    ImGui::SetNextWindowSize(ImVec2(680, 460), ImGuiCond_Appearing);
    std::string popupTitle = std::string(title) + "###fileDialog";
    if (!ImGui::BeginPopupModal(popupTitle.c_str(), &visible_)) return;

    // Строка пути + «вверх»
    if (ImGui::Button("..")) { if (dir_.has_parent_path() && dir_.parent_path() != dir_) { dir_ = dir_.parent_path(); pathInput_ = dir_.string(); list(); } }
    ImGui::SameLine();
#ifdef _WIN32
    // Выбор диска на Windows
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        char lbl[4] = {char('A' + i), ':', 0, 0};
        if (ImGui::SmallButton(lbl)) { dir_ = std::string(lbl) + "\\"; pathInput_ = dir_.string(); list(); }
        ImGui::SameLine();
    }
#endif
    ImGui::SetNextItemWidth(-90);
    if (ImGui::InputText("##path", &pathInput_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::error_code ec;
        if (fs::is_directory(pathInput_, ec)) { dir_ = pathInput_; list(); error_.clear(); }
        else if (mode_ == Mode::OpenFile && fs::is_regular_file(pathInput_, ec)) {
            auto cb = onOk_; visible_ = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup();
            if (cb) cb(pathInput_);
            return;
        } else error_ = L.tr("dialog.not_found");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(".*", &showHidden_)) list();

    // Список
    fs::path chosen;
    ImGui::BeginChild("##entries", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), ImGuiChildFlags_Borders);
    for (auto& e : entries_) {
        std::error_code ec;
        bool isDir = e.is_directory(ec);
        std::string label = (isDir ? "[+] " : "    ") + e.path().filename().string();
        bool selected = !nameInput_.empty() && e.path().filename().string() == nameInput_;
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (!isDir || mode_ == Mode::OpenFolder) nameInput_ = e.path().filename().string();
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (isDir) { dir_ = e.path(); pathInput_ = dir_.string(); nameInput_.clear(); list(); break; }
                chosen = e.path();
            }
        }
    }
    ImGui::EndChild();

    if (mode_ != Mode::OpenFolder || !nameInput_.empty()) {
        ImGui::SetNextItemWidth(-200);
        ImGui::InputTextWithHint("##name", L.tr("dialog.name"), &nameInput_);
        ImGui::SameLine();
    }
    const char* okLabel = mode_ == Mode::SaveFile ? L.tr("dialog.save") : L.tr("dialog.open");
    if (ImGui::Button(okLabel)) {
        if (mode_ == Mode::OpenFolder) chosen = nameInput_.empty() ? dir_ : dir_ / nameInput_;
        else if (!nameInput_.empty()) chosen = dir_ / nameInput_;
    }
    ImGui::SameLine();
    if (ImGui::Button(L.tr("common.cancel"))) { visible_ = false; ImGui::CloseCurrentPopup(); }
    if (!error_.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", error_.c_str()); }

    if (!chosen.empty()) {
        auto cb = onOk_;
        visible_ = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        if (cb) cb(chosen);
        return;
    }
    ImGui::EndPopup();
}

} // namespace ide
