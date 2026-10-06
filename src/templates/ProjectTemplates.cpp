// =============================================================================
//  ProjectTemplates.cpp
// =============================================================================
#include "templates/ProjectTemplates.hpp"

#include "core/Process.hpp"
#include "ui/Localization.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>

namespace ide {

namespace fs = std::filesystem;

static const char* kMitLicense = R"(MIT License

Copyright (c) {{year}} {{author}}

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
)";

ProjectTemplates::ProjectTemplates() {
    // ------------------------------ C++ / CMake ------------------------------
    templates_.push_back({"cpp-cmake", "C++20/23 (CMake)", "cpp", "Console app or library with CMake, CTest and clang-format",
        {{"standard", {"20", "23"}}, {"kind", {"executable", "library"}}},
        {{"CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.20)
project({{name}} VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD {{standard}})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # для clangd

{{#library}}add_library({{name}} src/{{name}}.cpp)
target_include_directories({{name}} PUBLIC include)
{{/library}}{{#executable}}add_executable({{name}} src/main.cpp)
{{/executable}}
if(MSVC)
    target_compile_options({{name}} PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options({{name}} PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable({{name}}_tests tests/test_main.cpp)
add_test(NAME {{name}}_tests COMMAND {{name}}_tests)
)"},
         {"src/main.cpp", R"(#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    constexpr std::string_view greeting = "Hello from {{name}}!";
    std::cout << greeting << '\n';
    return 0;
}
)"},
         {"tests/test_main.cpp", R"(#include <cassert>

int main() {
    assert(1 + 1 == 2);
    return 0;
}
)"},
         {".clang-format", "BasedOnStyle: LLVM\nIndentWidth: 4\nColumnLimit: 120\n"},
         {".gitignore", "build/\n.cache/\ncompile_commands.json\n"}}});

    // ------------------------------ Rust / Cargo -----------------------------
    templates_.push_back({"rust-cargo", "Rust (Cargo)", "rust", "Binary or library crate, edition 2021",
        {{"kind", {"bin", "lib"}}},
        {{"Cargo.toml", "[package]\nname = \"{{name_snake}}\"\nversion = \"0.1.0\"\nedition = \"2021\"\nauthors = [\"{{author}}\"]\n\n[dependencies]\n"},
         {"src/main.rs", "fn main() {\n    println!(\"Hello from {{name}}!\");\n}\n\n#[cfg(test)]\nmod tests {\n    #[test]\n    fn it_works() {\n        assert_eq!(2 + 2, 4);\n    }\n}\n"},
         {".gitignore", "/target\n"}}});

    // ------------------------------ Python -----------------------------------
    templates_.push_back({"python-pyproject", "Python (pyproject.toml)", "python", "PEP 621 package with src layout and pytest",
        {{"python", {"3.10", "3.11", "3.12", "3.13"}}},
        {{"pyproject.toml", "[build-system]\nrequires = [\"setuptools>=68\", \"wheel\"]\nbuild-backend = \"setuptools.build_meta\"\n\n[project]\nname = \"{{name_snake}}\"\nversion = \"0.1.0\"\nrequires-python = \">={{python}}\"\nauthors = [{name = \"{{author}}\"}]\ndependencies = []\n\n[project.scripts]\n{{name_snake}} = \"{{name_snake}}.__main__:main\"\n\n[tool.pytest.ini_options]\ntestpaths = [\"tests\"]\n"},
         {"src/{{name_snake}}/__init__.py", "__version__ = \"0.1.0\"\n"},
         {"src/{{name_snake}}/__main__.py", "def main() -> None:\n    print(\"Hello from {{name}}!\")\n\n\nif __name__ == \"__main__\":\n    main()\n"},
         {"main.py", "from src.{{name_snake}}.__main__ import main\n\nif __name__ == \"__main__\":\n    main()\n"},
         {"tests/test_basic.py", "def test_basic():\n    assert 1 + 1 == 2\n"},
         {".gitignore", "__pycache__/\n.venv/\n*.egg-info/\ndist/\n"}}});

    // ------------------------------ Go ---------------------------------------
    templates_.push_back({"go-module", "Go (Modules)", "go", "Go module with main package and test",
        {{"go", {"1.21", "1.22", "1.23"}}},
        {{"go.mod", "module {{go_module}}\n\ngo {{go}}\n"},
         {"main.go", "package main\n\nimport \"fmt\"\n\nfunc Greeting() string { return \"Hello from {{name}}!\" }\n\nfunc main() {\n\tfmt.Println(Greeting())\n}\n"},
         {"main_test.go", "package main\n\nimport \"testing\"\n\nfunc TestGreeting(t *testing.T) {\n\tif Greeting() == \"\" {\n\t\tt.Fatal(\"empty greeting\")\n\t}\n}\n"},
         {".gitignore", "/bin\n"}}});

    // ------------------------------ Node.js / TypeScript ---------------------
    templates_.push_back({"node-ts", "Node.js / TypeScript", "javascript", "TypeScript project with tsc build and npm scripts",
        {{"moduleSystem", {"esm", "commonjs"}}},
        {{"package.json", "{\n  \"name\": \"{{name_kebab}}\",\n  \"version\": \"0.1.0\",\n  \"type\": \"{{module_type}}\",\n  \"main\": \"dist/index.js\",\n  \"scripts\": {\n    \"build\": \"tsc\",\n    \"start\": \"node dist/index.js\",\n    \"test\": \"node --test dist/\"\n  },\n  \"devDependencies\": {\n    \"typescript\": \"^5.6.0\",\n    \"@types/node\": \"^22.0.0\"\n  }\n}\n"},
         {"tsconfig.json", "{\n  \"compilerOptions\": {\n    \"target\": \"ES2022\",\n    \"module\": \"{{ts_module}}\",\n    \"moduleResolution\": \"{{ts_resolution}}\",\n    \"outDir\": \"dist\",\n    \"rootDir\": \"src\",\n    \"strict\": true,\n    \"esModuleInterop\": true,\n    \"sourceMap\": true\n  },\n  \"include\": [\"src\"]\n}\n"},
         {"src/index.ts", "export function greeting(): string {\n  return \"Hello from {{name}}!\";\n}\n\nconsole.log(greeting());\n"},
         {".gitignore", "node_modules/\ndist/\n"}}});
}

const ProjectTemplate* ProjectTemplates::find(const std::string& id) const {
    for (auto& t : templates_) if (t.id == id) return &t;
    return nullptr;
}

bool ProjectTemplates::isValidName(const std::string& n) {
    if (n.empty() || n.size() > 100) return false;
    return std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; }) &&
           n != "." && n != "..";
}

std::string ProjectTemplates::substitute(std::string text, const std::map<std::string, std::string>& vars) {
    // Условные секции {{#key}}...{{/key}}: оставляются, если vars[key] == "1"
    for (;;) {
        auto open = text.find("{{#");
        if (open == std::string::npos) break;
        auto close = text.find("}}", open);
        if (close == std::string::npos) break;
        std::string key = text.substr(open + 3, close - open - 3);
        std::string endTag = "{{/" + key + "}}";
        auto end = text.find(endTag, close);
        if (end == std::string::npos) break;
        auto it = vars.find(key);
        bool keep = it != vars.end() && it->second == "1";
        std::string inner = text.substr(close + 2, end - close - 2);
        text.replace(open, end + endTag.size() - open, keep ? inner : "");
    }
    for (auto& [k, v] : vars) {
        std::string ph = "{{" + k + "}}";
        for (auto p = text.find(ph); p != std::string::npos; p = text.find(ph, p + v.size())) text.replace(p, ph.size(), v);
    }
    return text;
}

fs::path ProjectTemplates::generate(const ProjectParams& p, std::string* err) const {
    const ProjectTemplate* t = find(p.templateId);
    if (!t) { if (err) *err = "Unknown template: " + p.templateId; return {}; }
    if (!isValidName(p.name)) { if (err) *err = "Недопустимое имя проекта (буквы, цифры, '-', '_', '.')"; return {}; }
    fs::path root = p.location / p.name;
    std::error_code ec;
    if (fs::exists(root, ec) && !fs::is_empty(root, ec)) { if (err) *err = "Каталог уже существует и не пуст: " + root.u8string(); return {}; }

    // Переменные шаблона
    std::map<std::string, std::string> vars = p.options;
    vars["name"] = p.name;
    std::string snake = p.name, kebab = p.name;
    for (auto& c : snake) c = (c == '-' || c == '.') ? '_' : (char)std::tolower((unsigned char)c);
    for (auto& c : kebab) c = (c == '_' || c == '.') ? '-' : (char)std::tolower((unsigned char)c);
    vars["name_snake"] = snake;
    vars["name_kebab"] = kebab;
    vars["author"] = p.author.empty() ? "Unknown" : p.author;
    vars["go_module"] = "example.com/" + kebab;
    std::time_t now = std::time(nullptr);
    char year[8]; std::strftime(year, sizeof(year), "%Y", std::localtime(&now));
    vars["year"] = year;
    for (auto& [opt, choices] : t->options) {
        if (!vars.count(opt)) vars[opt] = choices.front();
        for (auto& c : choices) vars[c] = vars[opt] == c ? "1" : "0";   // флаги для условных секций
    }
    // Специфика Node.js: ESM/CommonJS
    bool esm = vars["moduleSystem"] != "commonjs";
    vars["module_type"] = esm ? "module" : "commonjs";
    vars["ts_module"] = esm ? "NodeNext" : "CommonJS";
    vars["ts_resolution"] = esm ? "NodeNext" : "Node";

    try {
        fs::create_directories(root);
        auto files = t->files;
        // Rust lib-крейт: src/lib.rs вместо src/main.rs
        if (t->id == "rust-cargo" && vars["kind"] == "lib")
            for (auto& f : files) if (f.path == "src/main.rs") { f.path = "src/lib.rs"; f.content = "pub fn greeting() -> &'static str {\n    \"Hello from {{name}}!\"\n}\n\n#[cfg(test)]\nmod tests {\n    use super::*;\n    #[test]\n    fn works() { assert!(!greeting().is_empty()); }\n}\n"; }
        if (t->id == "cpp-cmake" && vars["kind"] == "library") {
            files.push_back({"include/{{name}}.hpp", "#pragma once\n#include <string>\n\nnamespace {{name_snake}} {\nstd::string greeting();\n}\n"});
            files.push_back({"src/{{name}}.cpp", "#include \"{{name}}.hpp\"\n\nnamespace {{name_snake}} {\nstd::string greeting() { return \"Hello from {{name}}!\"; }\n}\n"});
        }
        files.push_back({"README.md", "# {{name}}\n\nСоздано в HybridIDE ({{template}}).\n"});
        vars["template"] = t->displayName;
        if (p.license == "MIT") files.push_back({"LICENSE", kMitLicense});
        for (auto& f : files) {
            fs::path fp = root / fs::u8path(substitute(f.path, vars));
            // Защита от выхода за пределы корня проекта (../ в пользовательских шаблонах)
            auto rel = fp.lexically_normal().lexically_relative(root);
            if (rel.empty() || rel.native()[0] == '.') { if (err) *err = "Недопустимый путь в шаблоне: " + f.path; return {}; }
            fs::create_directories(fp.parent_path());
            std::ofstream out(fp, std::ios::binary);
            out << substitute(f.content, vars);
            if (f.executable) fs::permissions(fp, fs::perms::owner_exec, fs::perm_options::add, ec);
        }
        if (p.initGit && Process::findExecutable("git")) {
            ProcessOptions o; o.argv = {"git", "init", "-b", "main"}; o.cwd = root;
            Process::run(o);
        }
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return {};
    }
    return root;
}

bool ProjectTemplates::loadUserTemplates(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".json") continue;
        std::ifstream f(e.path());
        auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded()) continue;
        ProjectTemplate t;
        t.id = j.value("id", e.path().stem().string());
        t.displayName = j.value("displayName", t.id);
        t.language = j.value("language", "");
        t.description = j.value("description", "");
        for (auto& [k, v] : j.value("options", nlohmann::json::object()).items()) t.options.push_back({k, v.get<std::vector<std::string>>()});
        for (auto& fl : j.value("files", nlohmann::json::array())) t.files.push_back({fl.value("path", ""), fl.value("content", ""), fl.value("executable", false)});
        if (!t.files.empty()) templates_.push_back(std::move(t));
    }
    return true;
}

void ProjectTemplates::renderWizard(bool* open, std::function<void(const fs::path&)> onCreated) {
    if (!*open) return;
    auto& L = Localization::instance();
    ImGui::SetNextWindowSize(ImVec2(720, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(L.tr("wizard.title"), open, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }
    if (locationBuf_.empty()) {
        std::error_code ec;
        locationBuf_ = (wiz_.location.empty() ? fs::current_path(ec) : wiz_.location).u8string();
    }
    ImGui::BeginChild("##tpls", ImVec2(240, -40), ImGuiChildFlags_Borders);
    for (auto& t : templates_) {
        if (ImGui::Selectable(t.displayName.c_str(), wiz_.templateId == t.id)) { wiz_.templateId = t.id; wiz_.options.clear(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.description.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##params", ImVec2(0, -40));
    const ProjectTemplate* t = find(wiz_.templateId);
    if (t) {
        ImGui::TextColored(ImVec4(.5f, .8f, 1, 1), "%s", t->displayName.c_str());
        ImGui::TextWrapped("%s", t->description.c_str());
        ImGui::Separator();
        ImGui::InputText(L.tr("wizard.name"), &wiz_.name);
        if (!isValidName(wiz_.name)) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", L.tr("wizard.invalid_name"));
        ImGui::InputText(L.tr("wizard.location"), &locationBuf_);
        ImGui::InputText(L.tr("wizard.author"), &wiz_.author);
        for (auto& [opt, choices] : t->options) {
            auto& cur = wiz_.options[opt];
            if (cur.empty()) cur = choices.front();
            if (ImGui::BeginCombo(opt.c_str(), cur.c_str())) {
                for (auto& c : choices) if (ImGui::Selectable(c.c_str(), c == cur)) cur = c;
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox(L.tr("wizard.init_git"), &wiz_.initGit);
        bool mit = wiz_.license == "MIT";
        if (ImGui::Checkbox(L.tr("wizard.mit"), &mit)) wiz_.license = mit ? "MIT" : "";
        ImGui::TextDisabled("%s: %s", L.tr("wizard.result"), (fs::u8path(locationBuf_) / wiz_.name).u8string().c_str());
    }
    ImGui::EndChild();
    if (!wizError_.empty()) { ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", wizError_.c_str()); ImGui::SameLine(); }
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 220);
    if (ImGui::Button(L.tr("wizard.create"), ImVec2(100, 0))) {
        wiz_.location = fs::u8path(locationBuf_);
        wizError_.clear();
        fs::path root = generate(wiz_, &wizError_);
        if (!root.empty()) { *open = false; if (onCreated) onCreated(root); }
    }
    ImGui::SameLine();
    if (ImGui::Button(L.tr("common.cancel"), ImVec2(100, 0))) *open = false;
    ImGui::End();
}

} // namespace ide
