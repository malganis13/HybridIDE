#pragma once
// ============================================================================
//  FileDialog — встроенный (ImGui) диалог выбора файла/папки.
//
//  Нативные диалоги ОС требуют отдельной зависимости на каждую платформу
//  (IFileDialog / GTK / NSOpenPanel). Встроенный диалог работает одинаково
//  везде, в том числе в окнах-вьюпортах ImGui, и уважает тему оформления.
// ============================================================================
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ide {

class FileDialog {
public:
    enum class Mode { OpenFile, OpenFolder, SaveFile };

    void open(Mode mode, const std::filesystem::path& start, std::function<void(const std::filesystem::path&)> onOk);
    void render();                                 // вызывать каждый кадр
    [[nodiscard]] bool visible() const { return visible_; }

private:
    void list();

    Mode                       mode_ = Mode::OpenFile;
    bool                       visible_ = false, justOpened_ = false;
    std::filesystem::path      dir_;
    std::string                pathInput_, nameInput_, error_;
    std::vector<std::filesystem::directory_entry> entries_;
    std::function<void(const std::filesystem::path&)> onOk_;
    bool                       showHidden_ = false;
};

} // namespace ide
