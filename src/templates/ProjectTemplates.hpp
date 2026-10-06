// =============================================================================
//  ProjectTemplates.hpp — параметрический мастер создания проектов:
//  C++20/23 (CMake), Rust (Cargo), Python (pyproject), Go (Modules),
//  Node.js / TypeScript. Шаблоны — подстановочные строки {{name}} и т.п.,
//  поэтому пользователь может добавлять собственные шаблоны (JSON).
// =============================================================================
#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ide {

struct TemplateFile {
    std::string path;      // относительный путь, допускает {{name}}
    std::string content;
    bool        executable = false;
};

struct ProjectTemplate {
    std::string id, displayName, language, description;
    std::vector<std::pair<std::string, std::vector<std::string>>> options;  // имя опции -> варианты
    std::vector<TemplateFile> files;
};

struct ProjectParams {
    std::string                        name = "MyProject";
    std::filesystem::path              location;
    std::string                        templateId = "cpp-cmake";
    std::map<std::string, std::string> options;   // "standard" -> "23", "kind" -> "executable"
    bool                               initGit = true;
    std::string                        author;
    std::string                        license = "MIT";
};

class ProjectTemplates {
public:
    ProjectTemplates();
    [[nodiscard]] const std::vector<ProjectTemplate>& all() const { return templates_; }
    [[nodiscard]] const ProjectTemplate* find(const std::string& id) const;

    // Генерация проекта; возвращает корень или пустой путь при ошибке
    std::filesystem::path generate(const ProjectParams& p, std::string* err = nullptr) const;
    bool loadUserTemplates(const std::filesystem::path& dir);   // *.json в каталоге профиля

    static std::string substitute(std::string text, const std::map<std::string, std::string>& vars);
    static bool isValidName(const std::string& name);

    void renderWizard(bool* open, std::function<void(const std::filesystem::path&)> onCreated);

private:
    std::vector<ProjectTemplate> templates_;
    ProjectParams                wiz_;
    std::string                  wizError_;
    std::string                  locationBuf_;
};

} // namespace ide
