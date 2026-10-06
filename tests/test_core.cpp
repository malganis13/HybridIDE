// ============================================================================
//  test_core.cpp — юнит-тесты ядра без GUI (запуск: ctest или ./ide_tests).
//
//  Минималистичный фреймворк без внешних зависимостей: TEST(name) регистрирует
//  функцию, CHECK/CHECK_EQ считают провалы и печатают место ошибки.
// ============================================================================
#include "core/BuildSystem.hpp"
#include "core/GitManager.hpp"
#include "core/SyntaxHighlighter.hpp"
#include "core/TerminalEngine.hpp"
#include "core/TextBuffer.hpp"
#include "github/GitHubManager.hpp"
#include "graphics/FishSimulation.hpp"
#include "graphics/WaveSimulation.hpp"
#include "lsp/JsonRpc.hpp"
#include "templates/ProjectTemplates.hpp"
#include "themes/ThemeManager.hpp"
#include "ui/KeybindingManager.hpp"
#include "ui/Localization.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {
struct TestCase { const char* name; std::function<void()> fn; };
std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
int g_failures = 0;
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
} // namespace

#define TEST(name) \
    static void test_##name(); \
    static Registrar reg_##name(#name, test_##name); \
    static void test_##name()
#define CHECK(cond) do { if (!(cond)) { ++g_failures; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++g_failures; \
    std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

using namespace ide;

// --- JSON-RPC framing (LSP/DAP) ---------------------------------------------
TEST(framer_split_and_join) {
    MessageFramer f;
    std::string a = MessageFramer::frame(R"({"id":1})"), b = MessageFramer::frame(R"({"id":2,"text":"привет"})");
    std::string all = a + b;
    std::vector<std::string> got;
    for (std::size_t i = 0; i < all.size(); i += 7) {          // кормим кусками по 7 байт
        auto part = f.feed(std::string_view(all).substr(i, 7));
        got.insert(got.end(), part.begin(), part.end());
    }
    CHECK_EQ(got.size(), std::size_t(2));
    if (got.size() == 2) {
        CHECK_EQ(got[0], std::string(R"({"id":1})"));
        CHECK_EQ(got[1], std::string(R"({"id":2,"text":"привет"})"));   // Content-Length в байтах UTF-8
    }
}

// --- TextBuffer -----------------------------------------------------------------
TEST(textbuffer_edit_undo_redo) {
    TextBuffer b;
    b.setText("hello\nworld");
    CHECK_EQ(b.lineCount(), 2);
    b.insert({0, 5}, ", dear");
    CHECK_EQ(b.line(0), std::string("hello, dear"));
    b.replace({{0, 0}, {1, 5}}, "X");
    CHECK_EQ(b.text(), std::string("X"));
    CHECK(b.undo());
    CHECK_EQ(b.text(), std::string("hello, dear\nworld"));
    CHECK(b.undo());
    CHECK_EQ(b.text(), std::string("hello\nworld"));
    CHECK(b.redo());
    CHECK_EQ(b.line(0), std::string("hello, dear"));
    CHECK(b.dirty());
}

TEST(textbuffer_word_navigation) {
    TextBuffer b;
    b.setText("int value = 42;");
    auto w = b.wordAt({0, 6});
    CHECK_EQ(b.getText(w), std::string("value"));
    CHECK_EQ(b.nextWord({0, 0}).col > 0, true);
}

// --- Подсветка ------------------------------------------------------------------
TEST(highlighter_cpp_tokens) {
    const LanguageDef* cpp = LanguageRegistry::instance().byExtension(".cpp");
    CHECK(cpp != nullptr);
    if (!cpp) return;
    SyntaxHighlighter::State st = SyntaxHighlighter::Normal;
    auto toks = SyntaxHighlighter::tokenizeLine(cpp, "int x = 42; // note", st);
    bool kw = false, num = false, com = false;
    for (auto& t : toks) {
        kw |= t.kind == TokenKind::Keyword || t.kind == TokenKind::Type;
        num |= t.kind == TokenKind::Number;
        com |= t.kind == TokenKind::Comment;
    }
    CHECK(kw); CHECK(num); CHECK(com);
    // Многострочный комментарий переносит состояние на следующую строку
    st = SyntaxHighlighter::Normal;
    SyntaxHighlighter::tokenizeLine(cpp, "/* start", st);
    CHECK_EQ((int)st, (int)SyntaxHighlighter::InBlockComment);
}

TEST(language_registry_ids) {
    auto& r = LanguageRegistry::instance();
    for (const char* ext : {".rs", ".py", ".go", ".js", ".json", ".toml"}) CHECK(r.byExtension(ext) != nullptr);
}

// --- Симуляции --------------------------------------------------------------------
TEST(boids_stay_in_bounds_and_flee) {
    FishSimulation sim;
    sim.reset(40, 800, 600, 42);
    for (int i = 0; i < 300; ++i) sim.update(1.f / 60.f);
    for (auto& f : sim.fish()) {
        CHECK(std::isfinite(f.pos.x) && std::isfinite(f.pos.y));
        CHECK(f.pos.x > -100 && f.pos.x < 900 && f.pos.y > -100 && f.pos.y < 700);
    }
    // Испуг: рыбы рядом с угрозой ускоряются
    Vec2 at = sim.fish().front().pos;
    sim.scare(at, 1.f);
    sim.update(1.f / 60.f);
    CHECK(sim.fish().front().fear > 0.f);
    std::vector<FishVertex> v;
    sim.buildVertices(v, 0.f);
    CHECK(!v.empty());
}

TEST(wave_is_stable_and_decays) {
    WaveSimulation w(64, 48);
    w.disturb(0.5f, 0.5f, 1.f, 0.05f);
    float e0 = 0;
    for (int i = 0; i < 30; ++i) w.step(1.f / 60.f);
    e0 = w.energy();
    for (int i = 0; i < 600; ++i) w.step(1.f / 60.f);
    float e1 = w.energy();
    CHECK(std::isfinite(e1));
    CHECK(e1 < e0);                      // затухание, без взрыва (условие Куранта)
}

// --- Локализация -------------------------------------------------------------
TEST(localization_hot_swap) {
    auto& L = Localization::instance();
    CHECK(L.setLanguage("en-US"));
    CHECK_EQ(std::string(L.tr("menu.file")), std::string("File"));
    CHECK(L.setLanguage("ru-RU"));
    CHECK_EQ(std::string(L.tr("menu.file")), std::string("Файл"));
    CHECK_EQ(std::string(L.tr("no.such.key")), std::string("no.such.key"));
    CHECK(!L.setLanguage("xx-XX"));
    // Каждый встроенный ключ переведён на оба языка, а у окон есть стабильный ###ID
    for (auto& k : L.builtinKeys()) {
        if (k.rfind("window.", 0) == 0) CHECK(std::string(L.tr(k)).find("###") != std::string::npos);
    }
}

TEST(all_commands_have_titles) {
    auto& L = Localization::instance();
    for (const char* preset : {"vscode", "visualstudio"}) {
        auto j = KeybindingManager::builtinPresetJson(preset);
        for (auto& b : j["bindings"]) {
            std::string key = "cmd." + b["command"].get<std::string>();
            if (!L.has(key)) { ++g_failures; std::printf("  FAIL missing translation %s\n", key.c_str()); }
        }
    }
}

// --- Клавиши ----------------------------------------------------------------
TEST(keybinding_parse_roundtrip) {
    auto s = KeybindingManager::parseSequence("Ctrl+K Ctrl+C");
    CHECK(s.has_value());
    if (s) {
        CHECK_EQ(s->size(), std::size_t(2));
        CHECK_EQ(KeybindingManager::sequenceToString(*s), std::string("Ctrl+K Ctrl+C"));
    }
    auto f5 = KeybindingManager::parseSequence("Shift+F5");
    CHECK(f5 && KeybindingManager::sequenceToString(*f5) == "Shift+F5");
    CHECK(!KeybindingManager::parseSequence("Ctrl+Nope").has_value());
    CHECK(!KeybindingManager::parseSequence("A B C").has_value());
    // Все сочетания встроенных пресетов разбираются
    for (const char* preset : {"vscode", "visualstudio"})
        for (auto& b : KeybindingManager::builtinPresetJson(preset)["bindings"])
            CHECK(KeybindingManager::parseSequence(b["key"].get<std::string>()).has_value());
}

// --- Шаблоны ------------------------------------------------------------------
TEST(template_substitution) {
    std::map<std::string, std::string> vars{{"name", "demo"}, {"git", "1"}, {"license", ""}};
    auto out = ProjectTemplates::substitute("project({{name}}){{#git}} +git{{/git}}{{#license}} +lic{{/license}}", vars);
    CHECK_EQ(out, std::string("project(demo) +git"));
    CHECK(ProjectTemplates::isValidName("my_app-2"));
    CHECK(!ProjectTemplates::isValidName("../evil"));
    CHECK(!ProjectTemplates::isValidName(""));
}

// --- Разбор вывода компиляторов -----------------------------------------------
TEST(build_output_parsing) {
    const std::string text =
        "/src/main.cpp:12:5: error: use of undeclared identifier 'foo'\n"
        "C:\\proj\\a.cpp(7,3): warning C4996: 'strcpy': unsafe\n"
        "error[E0308]: mismatched types\n"
        "  --> src/main.rs:4:9\n";
    auto p = BuildSystem::parseOutput(text, "/src");
    CHECK(p.size() >= 3);
    if (p.size() >= 2) {
        CHECK_EQ(p[0].line, 12); CHECK_EQ(p[0].col, 5); CHECK_EQ(p[0].severity, 1);
        CHECK_EQ(p[1].line, 7); CHECK_EQ(p[1].severity, 2);
    }
}

// --- Git porcelain -----------------------------------------------------------------
TEST(git_porcelain) {
    using namespace std::string_literals;   // формат `git status --porcelain=v1 -z`
    auto st = GitManager::parsePorcelain(" M src/a.cpp\0A  new.txt\0R  b.txt\0a.txt\0?? untracked.md\0"s);
    CHECK_EQ(st.size(), std::size_t(4));
    if (st.size() == 4) {
        CHECK(!st[0].staged() && st[0].unstaged());
        CHECK(st[1].staged());
        CHECK_EQ(st[2].path, std::string("b.txt"));   // rename: старое имя пропускается
        CHECK(st[3].unstaged());
    }
    auto d = GitManager::parseUnifiedDiff("@@ -1,2 +1,2 @@\n-old\n+new\n ctx\n");
    CHECK(d.size() >= 3);
}

// --- Терминал (VT100) -------------------------------------------------------------
TEST(vt_screen_basic) {
    VtScreen s(20, 5);
    s.feed("abc\r\n\x1b[31mred\x1b[0m\x1b[2;2Hx");
    std::string t = s.plainText();
    CHECK(t.find("abc") != std::string::npos);
    CHECK(t.find("rxd") != std::string::npos || t.find("red") != std::string::npos);
    CHECK_EQ(s.row(0)[0].ch, U'a');
    s.feed("\x1b[2J\x1b[H");
    CHECK_EQ(s.row(0)[0].ch, U' ');
}

// --- base64 (GitHub contents API) ---------------------------------------------
TEST(base64_roundtrip) {
    for (std::string v : {"", "f", "fo", "foo", "foob", "fooba", "foobar", "Привет, мир!"}) {
        auto enc = GitHubManager::base64Encode(v);
        CHECK_EQ(GitHubManager::base64Decode(enc), v);
    }
    CHECK_EQ(GitHubManager::base64Encode("foobar"), std::string("Zm9vYmFy"));
    CHECK_EQ(GitHubManager::base64Decode("Zm9v\nYmFy"), std::string("foobar"));   // GitHub режет строки по 60 символов
}

// --- Темы: JSON-персистентность ------------------------------------------------
TEST(theme_settings_roundtrip) {
    ThemeManager a;
    a.aquarium.fishCount = 77;
    a.hacker.curvature = 0.2f;
    a.cyberpunk.glitchOnFocus = false;
    auto j = a.toJson();
    ThemeManager b;
    b.fromJson(j);
    CHECK_EQ(b.aquarium.fishCount, 77);
    CHECK(std::fabs(b.hacker.curvature - 0.2f) < 1e-6f);
    CHECK(!b.cyberpunk.glitchOnFocus);
    CHECK_EQ(ThemeManager::idFromName("Hacker"), ThemeId::Hacker);
    CHECK_EQ(ThemeManager::idFromName("bogus"), ThemeId::Aquarium);
}

int main() {
    int failedTests = 0;
    for (auto& t : registry()) {
        int before = g_failures;
        t.fn();
        bool ok = g_failures == before;
        failedTests += ok ? 0 : 1;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failedTests);
    return failedTests == 0 ? 0 : 1;
}
