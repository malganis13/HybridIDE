// =============================================================================
//  EditorCore.hpp — многовкладочный редактор кода на Dear ImGui:
//  нумерация строк, сворачивание блоков, миникарта, парные скобки,
//  мультикурсор, поиск, диагностика LSP и «призрачный текст» автодополнения.
//  Каждый документ — отдельное докируемое окно ImGui, поэтому разделение
//  редактора (split view) делается простым перетаскиванием вкладки.
// =============================================================================
#pragma once
#include "core/SyntaxHighlighter.hpp"
#include "core/TextBuffer.hpp"

#include <imgui.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ide {

struct Cursor {
    TextPos pos, anchor;
    float   preferredX = -1.f;          // «желаемый» X при движении вверх/вниз
    [[nodiscard]] bool hasSelection() const { return pos != anchor; }
    [[nodiscard]] TextRange range() const { return TextRange{anchor, pos}.normalized(); }
};

// Кандидат автодополнения (из LSP или локального словаря буфера)
struct CompletionCandidate {
    std::string label;
    std::string insertText;
    std::string filterText;
    std::string detail;
    std::string kind;                    // "Function", "Variable", ...
    std::string source;                  // "clangd", "pyright", "Buffer"
    std::string sortText;
    std::optional<TextRange> editRange;  // textEdit.range (если сервер его прислал)
};

// Состояние ghost text: полупрозрачный предпросмотр дополнения
struct GhostText {
    bool        active = false;
    TextPos     anchor;                  // позиция курсора, к которой привязан текст
    std::string remaining;               // что будет вставлено по Tab
    TextRange   replaceRange;            // что будет заменено (префикс)
    std::string fullInsert;
    std::vector<CompletionCandidate> candidates;
    int         index = 0;
};

struct Diagnostic {
    TextRange   range;
    int         severity = 1;            // 1=Error 2=Warning 3=Info 4=Hint (как в LSP)
    std::string message;
    std::string source;
};

struct FoldRange { int startLine, endLine; };

struct Document {
    int                         id = 0;
    std::filesystem::path       path;    // пусто для «Безымянный-N»
    std::string                 title;
    TextBuffer                  buffer;
    SyntaxHighlighter           highlighter;
    std::vector<Cursor>         cursors{Cursor{}};
    std::set<int>               breakpoints;
    std::set<int>               folded;  // стартовые строки свёрнутых блоков
    std::vector<FoldRange>      folds;
    std::uint64_t               foldsVersion = 0;
    std::vector<Diagnostic>     diagnostics;
    GhostText                   ghost;
    int                         executionLine = -1;   // текущая строка отладчика
    bool                        open = true;
    bool                        focusRequested = false;
    bool                        scrollToCursor = false;
    std::optional<int>          scrollToLine;
    double                      completionDueAt = -1.0;
    std::uint64_t               completionVersion = 0;
    // Hover
    TextPos                     hoverPos{-1, -1};
    double                      hoverSince = 0.0;
    bool                        hoverRequested = false;
    std::string                 hoverText;

    [[nodiscard]] std::string languageId() const {
        auto* l = highlighter.language(); return l ? l->lspLanguageId : "plaintext";
    }
    [[nodiscard]] std::string uri() const;
};

struct EditorCallbacks {
    std::function<void(Document&)>                       onOpened;
    std::function<void(Document&, const std::vector<TextEdit>&)> onChanged;
    std::function<void(Document&)>                       onSaved;
    std::function<void(Document&)>                       onClosed;
    std::function<void(Document&, TextPos)>              requestCompletion;
    std::function<void(Document&, TextPos)>              requestHover;
    std::function<void(Document&, TextPos)>              requestDefinition;
    std::function<void(Document&, int line, bool on)>    onBreakpoint;
    std::function<void(unsigned int ch)>                 onKeystroke;      // звук печати
    std::function<void(ImVec2 screenPos)>                onEditorClick;   // всплеск/волна темы
};

// Палитра редактора задаётся активной темой
struct EditorPalette {
    std::array<ImU32, (std::size_t)TokenKind::Count> tokens{};
    ImU32 background = IM_COL32(30, 30, 30, 255);
    ImU32 gutter = IM_COL32(37, 37, 38, 255), lineNumber = IM_COL32(110, 118, 129, 255);
    ImU32 lineNumberActive = IM_COL32(220, 220, 220, 255), currentLine = IM_COL32(255, 255, 255, 14);
    ImU32 selection = IM_COL32(38, 79, 120, 200), cursor = IM_COL32(220, 220, 220, 255);
    ImU32 bracket = IM_COL32(255, 215, 0, 90), ghost = IM_COL32(180, 180, 180, 110);
    ImU32 ghostChip = IM_COL32(0, 122, 204, 200), errorSquiggle = IM_COL32(244, 71, 71, 255);
    ImU32 warningSquiggle = IM_COL32(205, 173, 0, 255), breakpoint = IM_COL32(229, 20, 0, 255);
    ImU32 execLine = IM_COL32(255, 238, 0, 40), findMatch = IM_COL32(234, 92, 0, 90);
    float backgroundAlpha = 1.f;   // <1 — фон темы (аквариум, матрица) просвечивает
};

class EditorCore {
public:
    EditorCore();

    Document* openFile(const std::filesystem::path& p, std::string* err = nullptr);
    Document* newUntitled(std::string_view langId = "cpp");
    bool      save(Document& d, std::string* err = nullptr);
    void      saveAll();
    void      close(int id);
    void      closeAll();

    void render(ImGuiID dockId);                 // отрисовка всех окон документов
    void executeCommand(std::string_view cmd);   // команды из KeybindingManager

    [[nodiscard]] Document* active() const;
    [[nodiscard]] Document* find(int id) const;
    [[nodiscard]] Document* findByPath(const std::filesystem::path& p) const;
    [[nodiscard]] const std::vector<std::unique_ptr<Document>>& documents() const { return docs_; }

    void goTo(const std::filesystem::path& p, int line, int col = 0);
    void setCompletionResult(int docId, std::uint64_t version, TextPos at, std::vector<CompletionCandidate> items);
    void setHoverResult(int docId, TextPos at, std::string text);
    void setDiagnostics(const std::filesystem::path& p, std::vector<Diagnostic> diags);
    void setExecutionLine(const std::filesystem::path& p, int line);
    void clearExecutionLine();

    EditorCallbacks callbacks;
    EditorPalette   palette;
    float           fontScale     = 1.0f;
    bool            showMinimap   = true;
    bool            enableGhost   = true;
    int             tabSize       = 4;
    double          completionDelay = 0.12;   // дебаунс запросов к LSP, сек

private:
    void renderDocument(Document& d);
    void handleKeyboard(Document& d);
    void handleMouse(Document& d, ImVec2 origin, float lineH, float textX, const std::vector<int>& visible);
    void insertText(Document& d, std::string_view text, bool typed);
    void deleteBackward(Document& d, bool word);
    void deleteForward(Document& d, bool word);
    void newline(Document& d);
    void moveCursors(Document& d, int dx, int dy, bool shift, bool word);
    void mergeCursors(Document& d);
    void notifyChanged(Document& d);
    void scheduleCompletion(Document& d);
    void acceptGhost(Document& d);
    void dismissGhost(Document& d);
    void cycleGhost(Document& d, int dir);
    void rebuildGhost(Document& d);
    void recomputeFolds(Document& d);
    void toggleFoldAt(Document& d, int line);
    void addNextOccurrence(Document& d);
    void toggleLineComment(Document& d);
    void duplicateLines(Document& d);
    std::vector<CompletionCandidate> bufferWordCandidates(Document& d, std::string_view prefix);
    std::optional<std::pair<TextPos, TextPos>> matchBracket(Document& d) const;
    [[nodiscard]] float xOf(const std::string& s, int col) const;
    [[nodiscard]] int   colOf(const std::string& s, float x) const;
    std::vector<int>    visibleLines(Document& d) const;

    std::vector<std::unique_ptr<Document>> docs_;
    int       nextId_ = 1, untitled_ = 1;
    int       activeId_ = 0;
    // Поиск
    bool        findOpen_ = false;
    std::string findText_;
    bool        findFocus_ = false;
    bool        draggingSelection_ = false;
};

// Утилиты LSP URI <-> путь
std::string pathToUri(const std::filesystem::path& p);
std::filesystem::path uriToPath(std::string_view uri);

} // namespace ide
