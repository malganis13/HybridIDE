// =============================================================================
//  EditorCore.cpp — реализация редактора кода
// =============================================================================
#include <imgui_internal.h>   // ImTextCharFromUtf8 / ImTextCharToUtf8
#include "core/EditorCore.hpp"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <unordered_set>

namespace ide {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//  URI-утилиты (RFC 3986, percent-encoding для не-ASCII и пробелов)
// ---------------------------------------------------------------------------
std::string pathToUri(const fs::path& p) {
    std::string s = fs::absolute(p).generic_string();
    std::string out = "file://";
#ifdef _WIN32
    out += '/';                       // file:///C:/...
#endif
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

fs::path uriToPath(std::string_view uri) {
    std::string_view s = uri;
    if (s.rfind("file://", 0) == 0) s.remove_prefix(7);
#ifdef _WIN32
    if (!s.empty() && s[0] == '/') s.remove_prefix(1);
#endif
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) { out += (char)std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16); i += 2; }
        else out += s[i];
    }
    return fs::u8path(out);
}

std::string Document::uri() const { return path.empty() ? "untitled:" + title : pathToUri(path); }

// ---------------------------------------------------------------------------
EditorCore::EditorCore() {
    // Палитра по умолчанию в стиле VS Code Dark+
    auto& t = palette.tokens;
    t[(int)TokenKind::Default]      = IM_COL32(212, 212, 212, 255);
    t[(int)TokenKind::Keyword]      = IM_COL32(86, 156, 214, 255);
    t[(int)TokenKind::Type]         = IM_COL32(78, 201, 176, 255);
    t[(int)TokenKind::Identifier]   = IM_COL32(156, 220, 254, 255);
    t[(int)TokenKind::Function]     = IM_COL32(220, 220, 170, 255);
    t[(int)TokenKind::Number]       = IM_COL32(181, 206, 168, 255);
    t[(int)TokenKind::String]       = IM_COL32(206, 145, 120, 255);
    t[(int)TokenKind::Char]         = IM_COL32(206, 145, 120, 255);
    t[(int)TokenKind::Comment]      = IM_COL32(106, 153, 85, 255);
    t[(int)TokenKind::Preprocessor] = IM_COL32(197, 134, 192, 255);
    t[(int)TokenKind::Operator]     = IM_COL32(212, 212, 212, 255);
    t[(int)TokenKind::Punctuation]  = IM_COL32(200, 200, 200, 255);
    t[(int)TokenKind::Attribute]    = IM_COL32(220, 160, 90, 255);
}

// ---------------------------------------------------------------------------
//  Управление документами
// ---------------------------------------------------------------------------
Document* EditorCore::openFile(const fs::path& p, std::string* err) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec) canon = p;
    if (auto* existing = findByPath(canon)) { existing->focusRequested = true; return existing; }

    auto d = std::make_unique<Document>();
    if (!d->buffer.loadFromFile(canon, err)) return nullptr;
    d->id    = nextId_++;
    d->path  = canon;
    d->title = canon.filename().u8string();
    const LanguageDef* lang = LanguageRegistry::instance().byExtension(canon.extension().string());
    if (!lang) lang = LanguageRegistry::instance().byExtension(canon.filename().string());   // CMakeLists.txt
    d->highlighter.setLanguage(lang);
    d->focusRequested = true;
    recomputeFolds(*d);
    docs_.push_back(std::move(d));
    Document& ref = *docs_.back();
    if (callbacks.onOpened) callbacks.onOpened(ref);
    return &ref;
}

Document* EditorCore::newUntitled(std::string_view langId) {
    auto d   = std::make_unique<Document>();
    d->id    = nextId_++;
    d->title = "Untitled-" + std::to_string(untitled_++);
    d->highlighter.setLanguage(LanguageRegistry::instance().byId(langId));
    d->focusRequested = true;
    docs_.push_back(std::move(d));
    return docs_.back().get();
}

bool EditorCore::save(Document& d, std::string* err) {
    if (d.path.empty()) { if (err) *err = "Документ не имеет пути (используйте «Сохранить как»)"; return false; }
    if (!d.buffer.saveToFile(d.path, err)) return false;
    if (callbacks.onSaved) callbacks.onSaved(d);
    return true;
}

void EditorCore::saveAll() {
    for (auto& d : docs_) if (d->buffer.dirty() && !d->path.empty()) save(*d);
}

void EditorCore::close(int id) {
    auto it = std::find_if(docs_.begin(), docs_.end(), [&](auto& d) { return d->id == id; });
    if (it == docs_.end()) return;
    if (callbacks.onClosed) callbacks.onClosed(**it);
    docs_.erase(it);
    if (activeId_ == id) activeId_ = docs_.empty() ? 0 : docs_.back()->id;
}

void EditorCore::closeAll() {
    while (!docs_.empty()) close(docs_.back()->id);
}

Document* EditorCore::active() const { return find(activeId_); }

Document* EditorCore::find(int id) const {
    for (auto& d : docs_) if (d->id == id) return d.get();
    return nullptr;
}

Document* EditorCore::findByPath(const fs::path& p) const {
    std::error_code ec;
    for (auto& d : docs_) if (!d->path.empty() && fs::equivalent(d->path, p, ec)) return d.get();
    return nullptr;
}

void EditorCore::goTo(const fs::path& p, int line, int col) {
    Document* d = findByPath(p);
    if (!d) d = openFile(p);
    if (!d) return;
    TextPos pos = d->buffer.clamp({line, col});
    d->cursors = {Cursor{pos, pos}};
    // Раскрываем свёрнутый блок, если цель внутри него
    for (auto& f : d->folds) if (pos.line > f.startLine && pos.line <= f.endLine) d->folded.erase(f.startLine);
    d->scrollToLine   = line;
    d->focusRequested = true;
}

void EditorCore::setDiagnostics(const fs::path& p, std::vector<Diagnostic> diags) {
    if (auto* d = findByPath(p)) d->diagnostics = std::move(diags);
}

void EditorCore::setExecutionLine(const fs::path& p, int line) {
    clearExecutionLine();
    goTo(p, line);
    if (auto* d = findByPath(p)) d->executionLine = line;
}

void EditorCore::clearExecutionLine() { for (auto& d : docs_) d->executionLine = -1; }

void EditorCore::setHoverResult(int docId, TextPos at, std::string text) {
    if (auto* d = find(docId); d && d->hoverPos == at) d->hoverText = std::move(text);
}

// ---------------------------------------------------------------------------
//  Измерение текста (поддержка UTF-8 и пропорциональных шрифтов)
// ---------------------------------------------------------------------------
float EditorCore::xOf(const std::string& s, int col) const {
    col = std::clamp(col, 0, (int)s.size());
    return ImGui::CalcTextSize(s.data(), s.data() + col).x;
}

int EditorCore::colOf(const std::string& s, float x) const {
    if (x <= 0) return 0;
    const char* b = s.data();
    const char* e = b + s.size();
    const char* p = b;
    float acc = 0;
    while (p < e) {
        unsigned int cp = 0;
        int len = ImTextCharFromUtf8(&cp, p, e);
        if (len <= 0) len = 1;
        float w = ImGui::CalcTextSize(p, p + len).x;
        if (acc + w * 0.5f > x) break;
        acc += w;
        p += len;
    }
    return (int)(p - b);
}

// ---------------------------------------------------------------------------
//  Сворачивание блоков: по фигурным скобкам или по отступам (Python)
// ---------------------------------------------------------------------------
void EditorCore::recomputeFolds(Document& d) {
    d.folds.clear();
    const auto& b = d.buffer;
    bool indentBased = d.highlighter.language() && d.highlighter.language()->id == "python";
    if (indentBased) {
        auto indentOf = [&](int l) {
            const auto& s = b.line(l);
            int n = 0; for (char c : s) { if (c == ' ') ++n; else if (c == '\t') n += 4; else break; }
            return n == (int)s.size() ? -1 : n;   // пустая строка
        };
        for (int l = 0; l < b.lineCount(); ++l) {
            int base = indentOf(l);
            if (base < 0) continue;
            int end = l;
            for (int k = l + 1; k < b.lineCount(); ++k) {
                int ind = indentOf(k);
                if (ind < 0) continue;
                if (ind <= base) break;
                end = k;
            }
            if (end > l) d.folds.push_back({l, end});
        }
    } else {
        std::vector<int> stack;
        for (int l = 0; l < b.lineCount(); ++l) {
            const auto& s = b.line(l);
            bool inStr = false; char q = 0;
            for (std::size_t i = 0; i < s.size(); ++i) {
                char c = s[i];
                if (inStr) { if (c == '\\') ++i; else if (c == q) inStr = false; continue; }
                if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
                if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') break;
                if (c == '{') stack.push_back(l);
                else if (c == '}' && !stack.empty()) {
                    int st = stack.back(); stack.pop_back();
                    if (l > st) d.folds.push_back({st, l - 1 >= st ? l - 1 : st});
                }
            }
        }
        // Сохраняем только «внешний» блок для каждой стартовой строки
        std::sort(d.folds.begin(), d.folds.end(), [](auto& a, auto& c) { return a.startLine < c.startLine || (a.startLine == c.startLine && a.endLine > c.endLine); });
        d.folds.erase(std::unique(d.folds.begin(), d.folds.end(), [](auto& a, auto& c) { return a.startLine == c.startLine; }), d.folds.end());
    }
    // Удаляем «осиротевшие» свёртки
    for (auto it = d.folded.begin(); it != d.folded.end();) {
        bool found = std::any_of(d.folds.begin(), d.folds.end(), [&](auto& f) { return f.startLine == *it; });
        it = found ? std::next(it) : d.folded.erase(it);
    }
    d.foldsVersion = d.buffer.version();
}

void EditorCore::toggleFoldAt(Document& d, int line) {
    for (auto& f : d.folds) if (f.startLine == line) {
        if (d.folded.count(line)) d.folded.erase(line); else d.folded.insert(line);
        return;
    }
}

std::vector<int> EditorCore::visibleLines(Document& d) const {
    std::vector<int> v;
    v.reserve((std::size_t)d.buffer.lineCount());
    std::map<int, int> foldEnd;
    for (auto& f : d.folds) if (d.folded.count(f.startLine)) foldEnd[f.startLine] = f.endLine;
    for (int l = 0; l < d.buffer.lineCount(); ++l) {
        v.push_back(l);
        auto it = foldEnd.find(l);
        if (it != foldEnd.end()) l = it->second;   // пропускаем тело свёрнутого блока
    }
    return v;
}

// ---------------------------------------------------------------------------
//  Парные скобки
// ---------------------------------------------------------------------------
std::optional<std::pair<TextPos, TextPos>> EditorCore::matchBracket(Document& d) const {
    const auto& b = d.buffer;
    TextPos p = d.cursors.front().pos;
    auto charAt = [&](TextPos q) -> char {
        const auto& s = b.line(q.line);
        return q.col >= 0 && q.col < (int)s.size() ? s[(std::size_t)q.col] : '\0';
    };
    static const std::string open = "([{", close = ")]}";
    TextPos at = p;
    char c = charAt(at);
    if (open.find(c) == std::string::npos && close.find(c) == std::string::npos) {
        if (p.col == 0) return std::nullopt;
        at = {p.line, p.col - 1}; c = charAt(at);
    }
    auto oi = open.find(c), ci = close.find(c);
    if (oi == std::string::npos && ci == std::string::npos) return std::nullopt;
    bool fwd = oi != std::string::npos;
    char me = c, other = fwd ? close[oi] : open[ci];
    int depth = 0, scanned = 0;
    TextPos q = at;
    while (scanned++ < 200000) {
        char x = charAt(q);
        if (x == me) ++depth;
        else if (x == other && --depth == 0) return std::make_pair(at, q);
        if (fwd) {
            if (q.col + 1 < (int)b.line(q.line).size()) ++q.col;
            else { do { if (++q.line >= b.lineCount()) return std::nullopt; } while (b.line(q.line).empty()); q.col = 0; }
        } else {
            if (q.col > 0) --q.col;
            else { do { if (--q.line < 0) return std::nullopt; } while (b.line(q.line).empty()); q.col = (int)b.line(q.line).size() - 1; }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
//  Правки с мультикурсором. Курсоры обрабатываются от последнего к первому,
//  поэтому правка не сдвигает ещё не обработанные позиции; уже обработанные
//  (более поздние) корректируются функцией shiftAfter.
// ---------------------------------------------------------------------------
static TextPos shiftAfter(TextPos p, TextRange removed, TextPos insertedEnd) {
    if (p < removed.end) return p;
    if (p.line == removed.end.line) { p.col += insertedEnd.col - removed.end.col; }
    p.line += insertedEnd.line - removed.end.line;
    return p;
}

void EditorCore::insertText(Document& d, std::string_view text, bool typed) {
    auto& buf = d.buffer;
    buf.beginGroup();
    std::vector<std::size_t> order(d.cursors.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return d.cursors[a].range().start > d.cursors[b].range().start; });

    // Мультикурсорная вставка: если строк в буфере обмена == курсоров — раздаём построчно
    std::vector<std::string> parts;
    if (!typed && d.cursors.size() > 1) {
        std::stringstream ss{std::string(text)};
        std::string line;
        while (std::getline(ss, line)) parts.push_back(line);
        if (parts.size() != d.cursors.size()) parts.clear();
    }
    std::vector<std::size_t> done;
    for (std::size_t k = 0; k < order.size(); ++k) {
        auto& c = d.cursors[order[k]];
        TextRange r = c.range();
        std::string_view piece = text;
        // Автозакрытие скобок и кавычек при наборе
        std::string auto_;
        if (typed && text.size() == 1 && !c.hasSelection()) {
            static const std::string op = "([{\"'", cl = ")]}\"'";
            const auto& s = buf.line(r.start.line);
            char next = r.start.col < (int)s.size() ? s[(std::size_t)r.start.col] : '\0';
            auto ci = cl.find(text[0]);
            if (ci != std::string::npos && next == text[0]) {      // «перешагиваем» закрывающую
                c.pos.col += 1; c.anchor = c.pos; continue;
            }
            auto oi = op.find(text[0]);
            if (oi != std::string::npos && (next == '\0' || next == ' ' || cl.find(next) != std::string::npos)) {
                auto_ = std::string(1, text[0]) + cl[oi];
                piece = auto_;
            }
        }
        if (!parts.empty()) piece = parts[order[k]];
        TextPos end = buf.replace(r, piece);
        if (!auto_.empty()) end.col -= 1;   // курсор между парными символами
        for (auto j : done) {
            d.cursors[j].pos    = shiftAfter(d.cursors[j].pos, r, end);
            d.cursors[j].anchor = shiftAfter(d.cursors[j].anchor, r, end);
        }
        c.pos = c.anchor = end; c.preferredX = -1;
        done.push_back(order[k]);
    }
    buf.endGroup();
    if (typed && callbacks.onKeystroke && !text.empty()) callbacks.onKeystroke((unsigned char)text[0]);
    notifyChanged(d);
    d.scrollToCursor = true;
}

void EditorCore::deleteBackward(Document& d, bool word) {
    auto& buf = d.buffer;
    buf.beginGroup();
    std::sort(d.cursors.begin(), d.cursors.end(), [](auto& a, auto& b) { return a.range().start > b.range().start; });
    for (std::size_t i = 0; i < d.cursors.size(); ++i) {
        auto& c = d.cursors[i];
        TextRange r = c.range();
        if (r.empty()) {
            TextPos from = word ? buf.prevWord(r.start) : buf.prevChar(r.start);
            // Удаление пары "()" целиком
            const auto& s = buf.line(r.start.line);
            if (!word && from.line == r.start.line && r.start.col < (int)s.size()) {
                char a = s[(std::size_t)from.col], b = s[(std::size_t)r.start.col];
                if ((a == '(' && b == ')') || (a == '[' && b == ']') || (a == '{' && b == '}') || (a == '"' && b == '"'))
                    r.end.col += 1;
            }
            r.start = from;
        }
        TextPos end = buf.replace(r, "");
        for (std::size_t j = 0; j < i; ++j) {
            d.cursors[j].pos = shiftAfter(d.cursors[j].pos, r, end);
            d.cursors[j].anchor = d.cursors[j].pos;
        }
        c.pos = c.anchor = end;
    }
    buf.endGroup();
    mergeCursors(d);
    notifyChanged(d);
}

void EditorCore::deleteForward(Document& d, bool word) {
    auto& buf = d.buffer;
    buf.beginGroup();
    std::sort(d.cursors.begin(), d.cursors.end(), [](auto& a, auto& b) { return a.range().start > b.range().start; });
    for (std::size_t i = 0; i < d.cursors.size(); ++i) {
        auto& c = d.cursors[i];
        TextRange r = c.range();
        if (r.empty()) r.end = word ? buf.nextWord(r.start) : buf.nextChar(r.start);
        TextPos end = buf.replace(r, "");
        for (std::size_t j = 0; j < i; ++j) { d.cursors[j].pos = shiftAfter(d.cursors[j].pos, r, end); d.cursors[j].anchor = d.cursors[j].pos; }
        c.pos = c.anchor = end;
    }
    buf.endGroup();
    mergeCursors(d);
    notifyChanged(d);
}

void EditorCore::newline(Document& d) {
    // Автоотступ: копируем отступ текущей строки, +1 уровень после '{' или ':'
    auto& buf = d.buffer;
    buf.beginGroup();
    std::sort(d.cursors.begin(), d.cursors.end(), [](auto& a, auto& b) { return a.range().start > b.range().start; });
    for (std::size_t i = 0; i < d.cursors.size(); ++i) {
        auto& c = d.cursors[i];
        TextRange r = c.range();
        const std::string s = buf.line(r.start.line);
        std::string indent;
        for (char ch : s) { if (ch == ' ' || ch == '\t') indent += ch; else break; }
        std::string before = s.substr(0, (std::size_t)r.start.col);
        while (!before.empty() && before.back() == ' ') before.pop_back();
        char last = before.empty() ? '\0' : before.back();
        char next = r.start.col < (int)s.size() ? s[(std::size_t)r.start.col] : '\0';
        std::string unit(tabSize, ' ');
        std::string ins = "\n" + indent;
        TextPos caretAfter;
        if (last == '{' || last == '(' || last == '[' || last == ':') {
            ins += unit;
            if ((last == '{' && next == '}') || (last == '(' && next == ')') || (last == '[' && next == ']')) {
                TextPos end = buf.replace(r, ins + "\n" + indent);
                caretAfter = {r.start.line + 1, (int)(indent.size() + unit.size())};
                for (std::size_t j = 0; j < i; ++j) { d.cursors[j].pos = shiftAfter(d.cursors[j].pos, r, end); d.cursors[j].anchor = d.cursors[j].pos; }
                c.pos = c.anchor = caretAfter;
                continue;
            }
        }
        TextPos end = buf.replace(r, ins);
        for (std::size_t j = 0; j < i; ++j) { d.cursors[j].pos = shiftAfter(d.cursors[j].pos, r, end); d.cursors[j].anchor = d.cursors[j].pos; }
        c.pos = c.anchor = end;
    }
    buf.endGroup();
    if (callbacks.onKeystroke) callbacks.onKeystroke('\n');
    notifyChanged(d);
    d.scrollToCursor = true;
}

void EditorCore::moveCursors(Document& d, int dx, int dy, bool shift, bool word) {
    auto& buf = d.buffer;
    auto vis = visibleLines(d);
    for (auto& c : d.cursors) {
        if (!shift && c.hasSelection() && dy == 0) {
            // Без Shift стрелка схлопывает выделение к соответствующему краю
            c.pos = dx < 0 ? c.range().start : c.range().end;
            c.anchor = c.pos;
            continue;
        }
        if (dx < 0) c.pos = word ? buf.prevWord(c.pos) : buf.prevChar(c.pos);
        if (dx > 0) c.pos = word ? buf.nextWord(c.pos) : buf.nextChar(c.pos);
        if (dy != 0) {
            if (c.preferredX < 0) c.preferredX = xOf(buf.line(c.pos.line), c.pos.col);
            auto it = std::lower_bound(vis.begin(), vis.end(), c.pos.line);
            int idx = (int)(it - vis.begin());
            idx = std::clamp(idx + dy, 0, (int)vis.size() - 1);
            c.pos.line = vis[(std::size_t)idx];
            c.pos.col  = colOf(buf.line(c.pos.line), c.preferredX);
        } else c.preferredX = -1;
        if (!shift) c.anchor = c.pos;
    }
    mergeCursors(d);
    dismissGhost(d);
    d.scrollToCursor = true;
}

void EditorCore::mergeCursors(Document& d) {
    // Удаляем дубли и пересекающиеся курсоры
    std::sort(d.cursors.begin(), d.cursors.end(), [](auto& a, auto& b) { return a.range().start < b.range().start; });
    std::vector<Cursor> out;
    for (auto& c : d.cursors) {
        if (!out.empty()) {
            auto& p = out.back();
            if (c.range().start < p.range().end || c.pos == p.pos) {
                TextPos s = std::min(p.range().start, c.range().start), e = std::max(p.range().end, c.range().end);
                bool fwd = p.pos >= p.anchor;
                p.anchor = fwd ? s : e; p.pos = fwd ? e : s;
                continue;
            }
        }
        out.push_back(c);
    }
    if (out.empty()) out.push_back(Cursor{});
    d.cursors = std::move(out);
}

void EditorCore::notifyChanged(Document& d) {
    auto edits = d.buffer.takePendingEdits();
    if (edits.empty()) return;
    int minLine = d.buffer.lineCount();
    for (auto& e : edits) minLine = std::min(minLine, e.range.start.line);
    d.highlighter.invalidateFrom(minLine);
    recomputeFolds(d);
    // Диагностики после правок частично устаревают — сервер пришлёт новые
    if (callbacks.onChanged) callbacks.onChanged(d, edits);
}

// ---------------------------------------------------------------------------
//  Ghost text автодополнение
// ---------------------------------------------------------------------------
void EditorCore::scheduleCompletion(Document& d) {
    if (!enableGhost || d.cursors.size() != 1) { dismissGhost(d); return; }
    d.completionDueAt   = ImGui::GetTime() + completionDelay;
    d.completionVersion = d.buffer.version();
}

static std::string stripSnippet(std::string s) {
    // Удаляем плейсхолдеры сниппетов LSP: ${1:arg} -> arg, $0/$1 -> ""
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '$' && i + 1 < s.size()) {
            if (s[i + 1] == '{') {
                auto colon = s.find(':', i), close = s.find('}', i);
                if (close != std::string::npos) {
                    if (colon != std::string::npos && colon < close) out += s.substr(colon + 1, close - colon - 1);
                    i = close; continue;
                }
            } else if (std::isdigit((unsigned char)s[i + 1])) { ++i; while (i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])) ++i; continue; }
        }
        if (s[i] == '\\' && i + 1 < s.size()) { out += s[++i]; continue; }
        out += s[i];
    }
    return out;
}

static bool istartsWith(std::string_view s, std::string_view p) {
    if (p.size() > s.size()) return false;
    for (std::size_t i = 0; i < p.size(); ++i)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)p[i])) return false;
    return true;
}

std::vector<CompletionCandidate> EditorCore::bufferWordCandidates(Document& d, std::string_view prefix) {
    // Локальный источник: слова документа (работает без языкового сервера)
    std::vector<CompletionCandidate> out;
    if (prefix.size() < 2) return out;
    std::unordered_set<std::string> seen;
    const auto& b = d.buffer;
    int cl = d.cursors.front().pos.line;
    for (int dist = 0; dist < b.lineCount() && out.size() < 50; ++dist) {
        for (int sign : {-1, 1}) {
            int l = cl + sign * dist;
            if (l < 0 || l >= b.lineCount() || (dist == 0 && sign == 1)) continue;
            const auto& s = b.line(l);
            for (std::size_t i = 0; i < s.size();) {
                if (!isWordChar((unsigned char)s[i])) { ++i; continue; }
                std::size_t j = i; while (j < s.size() && isWordChar((unsigned char)s[j])) ++j;
                std::string w = s.substr(i, j - i);
                if (w.size() > prefix.size() && istartsWith(w, prefix) && seen.insert(w).second) {
                    CompletionCandidate c; c.label = c.insertText = w; c.kind = "Text"; c.source = "Buffer";
                    out.push_back(std::move(c));
                }
                i = j;
            }
        }
    }
    return out;
}

void EditorCore::setCompletionResult(int docId, std::uint64_t version, TextPos at, std::vector<CompletionCandidate> items) {
    Document* d = find(docId);
    if (!d || d->buffer.version() != version || d->cursors.size() != 1 || d->cursors.front().pos != at) return; // устаревший ответ
    TextRange wr = d->buffer.wordAt(at);
    wr.end = at;
    std::string prefix = d->buffer.getText(wr);
    std::vector<CompletionCandidate> filtered;
    for (auto& c : items) {
        std::string key = c.filterText.empty() ? c.label : c.filterText;
        if (prefix.empty() || istartsWith(key, prefix)) filtered.push_back(std::move(c));
    }
    std::stable_sort(filtered.begin(), filtered.end(), [](auto& a, auto& b) {
        return (a.sortText.empty() ? a.label : a.sortText) < (b.sortText.empty() ? b.label : b.sortText);
    });
    // Дополняем словами буфера (с меньшим приоритетом)
    for (auto& c : bufferWordCandidates(*d, prefix)) {
        bool dup = std::any_of(filtered.begin(), filtered.end(), [&](auto& f) { return f.label == c.label; });
        if (!dup) filtered.push_back(std::move(c));
    }
    d->ghost.candidates = std::move(filtered);
    d->ghost.index      = 0;
    d->ghost.anchor     = at;
    rebuildGhost(*d);
}

void EditorCore::rebuildGhost(Document& d) {
    auto& g = d.ghost;
    g.active = false;
    if (g.candidates.empty()) return;
    const auto& c = g.candidates[(std::size_t)g.index % g.candidates.size()];
    TextRange wr = d.buffer.wordAt(g.anchor);
    wr.end = g.anchor;
    if (c.editRange) wr = *c.editRange;
    std::string prefix = d.buffer.getText(TextRange{wr.start, g.anchor});
    std::string ins = stripSnippet(c.insertText.empty() ? c.label : c.insertText);
    if (!istartsWith(ins, prefix)) return;
    g.fullInsert   = ins;
    g.remaining    = ins.substr(prefix.size());
    g.replaceRange = {wr.start, std::max(wr.end, g.anchor)};
    g.active       = !g.remaining.empty();
}

void EditorCore::acceptGhost(Document& d) {
    auto& g = d.ghost;
    if (!g.active) return;
    TextPos end = d.buffer.replace(g.replaceRange, g.fullInsert);
    d.cursors = {Cursor{end, end}};
    g = GhostText{};
    notifyChanged(d);
}

void EditorCore::dismissGhost(Document& d) { d.ghost = GhostText{}; d.completionDueAt = -1; }

void EditorCore::cycleGhost(Document& d, int dir) {
    auto& g = d.ghost;
    if (g.candidates.size() < 2) return;
    g.index = (int)((g.index + dir + (int)g.candidates.size()) % (int)g.candidates.size());
    rebuildGhost(d);
}

// ---------------------------------------------------------------------------
//  Команды редактирования
// ---------------------------------------------------------------------------
void EditorCore::addNextOccurrence(Document& d) {
    auto& last = d.cursors.back();
    if (!last.hasSelection()) {
        TextRange w = d.buffer.wordAt(last.pos);
        last.anchor = w.start; last.pos = w.end;
        return;
    }
    std::string needle = d.buffer.getText(last.range());
    if (needle.empty()) return;
    TextPos from = last.range().end;
    for (int pass = 0; pass < 2; ++pass) {
        for (int l = from.line; l < d.buffer.lineCount(); ++l) {
            const auto& s = d.buffer.line(l);
            std::size_t start = l == from.line ? (std::size_t)from.col : 0;
            auto f = s.find(needle, start);
            if (f != std::string::npos) {
                Cursor c; c.anchor = {l, (int)f}; c.pos = {l, (int)(f + needle.size())};
                bool exists = std::any_of(d.cursors.begin(), d.cursors.end(), [&](auto& x) { return x.range().start == c.anchor; });
                if (!exists) { d.cursors.push_back(c); d.scrollToCursor = true; }
                return;
            }
        }
        from = {0, 0};   // поиск с начала документа
    }
}

void EditorCore::toggleLineComment(Document& d) {
    auto* lang = d.highlighter.language();
    if (!lang || lang->lineComment.empty()) return;
    const std::string mark = lang->lineComment + " ";
    std::set<int> lines;
    for (auto& c : d.cursors) for (int l = c.range().start.line; l <= c.range().end.line; ++l) lines.insert(l);
    bool allCommented = std::all_of(lines.begin(), lines.end(), [&](int l) {
        const auto& s = d.buffer.line(l);
        auto p = s.find_first_not_of(" \t");
        return p == std::string::npos || s.compare(p, lang->lineComment.size(), lang->lineComment) == 0;
    });
    d.buffer.beginGroup();
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::string s = d.buffer.line(*it);
        auto p = s.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        if (allCommented) {
            std::size_t len = s.compare(p, mark.size(), mark) == 0 ? mark.size() : lang->lineComment.size();
            d.buffer.replace({{*it, (int)p}, {*it, (int)(p + len)}}, "");
        } else d.buffer.replace({{*it, (int)p}, {*it, (int)p}}, mark);
    }
    d.buffer.endGroup();
    for (auto& c : d.cursors) { c.pos = d.buffer.clamp(c.pos); c.anchor = d.buffer.clamp(c.anchor); }
    notifyChanged(d);
}

void EditorCore::duplicateLines(Document& d) {
    d.buffer.beginGroup();
    for (auto& c : d.cursors) {
        int l = c.pos.line;
        std::string s = d.buffer.line(l);
        d.buffer.replace({{l, (int)s.size()}, {l, (int)s.size()}}, "\n" + s);
        c.pos.line += 1; c.anchor = c.pos;
    }
    d.buffer.endGroup();
    notifyChanged(d);
}

void EditorCore::executeCommand(std::string_view cmd) {
    if (cmd == "editor.find") { findOpen_ = true; findFocus_ = true; return; }
    Document* d = active();
    if (!d) return;
    auto& buf = d->buffer;
    if (cmd == "editor.undo" || cmd == "editor.redo") {
        std::vector<TextPos> cur;
        if (cmd == "editor.undo" ? buf.undo(&cur) : buf.redo(&cur)) {
            d->cursors.clear();
            for (auto p : cur) d->cursors.push_back(Cursor{p, p});
            mergeCursors(*d);
            notifyChanged(*d);
        }
    } else if (cmd == "editor.copy" || cmd == "editor.cut") {
        std::string clip;
        bool anySel = std::any_of(d->cursors.begin(), d->cursors.end(), [](auto& c) { return c.hasSelection(); });
        for (std::size_t i = 0; i < d->cursors.size(); ++i) {
            auto& c = d->cursors[i];
            if (i) clip += '\n';
            clip += anySel ? buf.getText(c.range()) : buf.line(c.pos.line);   // без выделения — вся строка
        }
        ImGui::SetClipboardText(clip.c_str());
        if (cmd == "editor.cut") {
            if (!anySel) for (auto& c : d->cursors) { c.anchor = {c.pos.line, 0}; c.pos = buf.nextChar({c.pos.line, (int)buf.line(c.pos.line).size()}); }
            insertText(*d, "", false);
        }
    } else if (cmd == "editor.paste") {
        if (const char* t = ImGui::GetClipboardText()) insertText(*d, t, false);
    } else if (cmd == "editor.selectAll") {
        d->cursors = {Cursor{buf.endPos(), {0, 0}}};
    } else if (cmd == "editor.addNextOccurrence") {
        addNextOccurrence(*d);
    } else if (cmd == "editor.addCursorAbove" || cmd == "editor.addCursorBelow") {
        int dir = cmd == "editor.addCursorAbove" ? -1 : 1;
        Cursor c = dir < 0 ? d->cursors.front() : d->cursors.back();
        int nl = c.pos.line + dir;
        if (nl >= 0 && nl < buf.lineCount()) {
            float x = xOf(buf.line(c.pos.line), c.pos.col);
            TextPos p{nl, colOf(buf.line(nl), x)};
            d->cursors.push_back(Cursor{p, p});
            mergeCursors(*d);
        }
    } else if (cmd == "editor.toggleComment") {
        toggleLineComment(*d);
    } else if (cmd == "editor.duplicateLine") {
        duplicateLines(*d);
    } else if (cmd == "editor.toggleFold") {
        int l = d->cursors.front().pos.line;
        for (auto& f : d->folds) if (l >= f.startLine && l <= f.endLine) { toggleFoldAt(*d, f.startLine); break; }
    } else if (cmd == "editor.foldAll") {
        for (auto& f : d->folds) d->folded.insert(f.startLine);
    } else if (cmd == "editor.unfoldAll") {
        d->folded.clear();
    } else if (cmd == "editor.triggerCompletion") {
        d->completionDueAt = ImGui::GetTime();
        d->completionVersion = buf.version();
    } else if (cmd == "editor.goToDefinition") {
        if (callbacks.requestDefinition) callbacks.requestDefinition(*d, d->cursors.front().pos);
    } else if (cmd == "editor.toggleBreakpoint") {
        int l = d->cursors.front().pos.line;
        bool on = !d->breakpoints.count(l);
        if (on) d->breakpoints.insert(l); else d->breakpoints.erase(l);
        if (callbacks.onBreakpoint) callbacks.onBreakpoint(*d, l, on);
    } else if (cmd == "file.save") {
        save(*d);
    } else if (cmd == "file.saveAll") {
        saveAll();
    } else if (cmd == "file.close") {
        d->open = false;
    } else if (cmd == "editor.zoomIn") {
        fontScale = std::min(3.f, fontScale + 0.1f);
    } else if (cmd == "editor.zoomOut") {
        fontScale = std::max(0.5f, fontScale - 0.1f);
    }
}

// ---------------------------------------------------------------------------
//  Клавиатура: только «редакторские» клавиши; глобальные сочетания обрабатывает
//  KeybindingManager до вызова render().
// ---------------------------------------------------------------------------
void EditorCore::handleKeyboard(Document& d) {
    ImGuiIO& io = ImGui::GetIO();
    io.WantCaptureKeyboard = true;
    io.WantTextInput       = true;
    const bool ctrl  = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
    const bool shift = io.KeyShift, alt = io.KeyAlt;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };

    // Ghost text: Tab — принять, Esc — отклонить, Alt+] / Alt+[ — листать варианты
    if (d.ghost.active) {
        if (pressed(ImGuiKey_Tab) && !shift) { acceptGhost(d); return; }
        if (pressed(ImGuiKey_Escape)) { dismissGhost(d); return; }
        if (alt && pressed(ImGuiKey_RightBracket)) { cycleGhost(d, +1); return; }
        if (alt && pressed(ImGuiKey_LeftBracket))  { cycleGhost(d, -1); return; }
    }
    if (pressed(ImGuiKey_Escape)) {
        if (d.cursors.size() > 1) d.cursors.resize(1);
        else d.cursors.front().anchor = d.cursors.front().pos;
        findOpen_ = false;
        return;
    }

    if (pressed(ImGuiKey_LeftArrow))  moveCursors(d, -1, 0, shift, ctrl);
    if (pressed(ImGuiKey_RightArrow)) moveCursors(d, +1, 0, shift, ctrl);
    if (pressed(ImGuiKey_UpArrow) && !(ctrl && alt))   moveCursors(d, 0, -1, shift, false);
    if (pressed(ImGuiKey_DownArrow) && !(ctrl && alt)) moveCursors(d, 0, +1, shift, false);
    if (pressed(ImGuiKey_PageUp))   moveCursors(d, 0, -30, shift, false);
    if (pressed(ImGuiKey_PageDown)) moveCursors(d, 0, +30, shift, false);
    if (pressed(ImGuiKey_Home)) {
        for (auto& c : d.cursors) {
            if (ctrl) c.pos = {0, 0};
            else {   // «умный» Home: сначала к первому непробельному символу
                const auto& s = d.buffer.line(c.pos.line);
                int first = (int)std::min(s.find_first_not_of(" \t"), s.size());
                c.pos.col = c.pos.col == first ? 0 : first;
            }
            if (!shift) c.anchor = c.pos;
        }
        d.scrollToCursor = true;
    }
    if (pressed(ImGuiKey_End)) {
        for (auto& c : d.cursors) {
            c.pos = ctrl ? d.buffer.endPos() : TextPos{c.pos.line, (int)d.buffer.line(c.pos.line).size()};
            if (!shift) c.anchor = c.pos;
        }
        d.scrollToCursor = true;
    }
    if (pressed(ImGuiKey_Backspace)) { deleteBackward(d, ctrl); scheduleCompletion(d); }
    if (pressed(ImGuiKey_Delete))    { deleteForward(d, ctrl); dismissGhost(d); }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) { dismissGhost(d); newline(d); }
    if (pressed(ImGuiKey_Tab) && !ctrl) {
        if (shift) {   // уменьшить отступ
            d.buffer.beginGroup();
            for (auto& c : d.cursors) {
                const auto& s = d.buffer.line(c.pos.line);
                int n = 0; while (n < tabSize && n < (int)s.size() && s[(std::size_t)n] == ' ') ++n;
                if (n) { d.buffer.replace({{c.pos.line, 0}, {c.pos.line, n}}, ""); c.pos.col = std::max(0, c.pos.col - n); c.anchor = c.pos; }
            }
            d.buffer.endGroup();
            notifyChanged(d);
        } else insertText(d, std::string((std::size_t)tabSize, ' '), false);
    }

    // Печатаемые символы (UTF-32 -> UTF-8). Игнорируем ввод при Ctrl (сочетания)
    if (!ctrl || alt) {
        std::string typed;
        for (ImWchar ch : io.InputQueueCharacters) {
            if (ch == '\t' || ch == '\n' || ch == '\r' || ch < 32) continue;
            char buf[5] = {};
            ImTextCharToUtf8(buf, ch);
            typed += buf;
        }
        if (!typed.empty()) {
            insertText(d, typed, true);
            unsigned char last = (unsigned char)typed.back();
            // Триггеры дополнения: идентификатор, '.', '->', '::'
            if (isWordChar(last) || last == '.' || last == '>' || last == ':') scheduleCompletion(d);
            else dismissGhost(d);
        }
    }
    io.InputQueueCharacters.resize(0);
}

// ---------------------------------------------------------------------------
//  Мышь: клики, выделение, Alt+клик — новый курсор, двойной клик — слово
// ---------------------------------------------------------------------------
void EditorCore::handleMouse(Document& d, ImVec2 origin, float lineH, float textX, const std::vector<int>& visible) {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowHovered()) { if (!ImGui::IsMouseDown(0)) draggingSelection_ = false; return; }
    ImVec2 m = ImGui::GetMousePos();
    auto posAt = [&](ImVec2 mp) {
        int row = (int)std::floor((mp.y - origin.y) / lineH);
        row = std::clamp(row, 0, (int)visible.size() - 1);
        int line = visible[(std::size_t)row];
        return TextPos{line, colOf(d.buffer.line(line), mp.x - origin.x - textX)};
    };
    TextPos p = posAt(m);

    if (m.x - origin.x < textX) {     // клик по полю номеров строк
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(0)) {
            bool foldable = std::any_of(d.folds.begin(), d.folds.end(), [&](auto& f) { return f.startLine == p.line; });
            float foldX = textX - 14.f;
            if (foldable && m.x - origin.x >= foldX) toggleFoldAt(d, p.line);
            else {
                bool on = !d.breakpoints.count(p.line);
                if (on) d.breakpoints.insert(p.line); else d.breakpoints.erase(p.line);
                if (callbacks.onBreakpoint) callbacks.onBreakpoint(d, p.line, on);
            }
        }
        return;
    }
    ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

    if (ImGui::IsMouseClicked(0)) {
        if (callbacks.onEditorClick) callbacks.onEditorClick(m);
        dismissGhost(d);
        if (io.KeyAlt) d.cursors.push_back(Cursor{p, p});
        else if (io.KeyShift) d.cursors.front().pos = p;
        else d.cursors = {Cursor{p, p}};
        if (io.KeyCtrl && callbacks.requestDefinition) callbacks.requestDefinition(d, p);
        draggingSelection_ = true;
        mergeCursors(d);
    }
    if (ImGui::IsMouseDoubleClicked(0)) {
        TextRange w = d.buffer.wordAt(p);
        d.cursors = {Cursor{w.end, w.start}};
        draggingSelection_ = false;
    }
    if (draggingSelection_ && ImGui::IsMouseDragging(0)) {
        d.cursors.back().pos = p;
        // Автоскролл при выделении за пределами окна
        ImVec2 wmin = ImGui::GetWindowPos(), wsz = ImGui::GetWindowSize();
        if (m.y < wmin.y) ImGui::SetScrollY(ImGui::GetScrollY() - lineH);
        if (m.y > wmin.y + wsz.y) ImGui::SetScrollY(ImGui::GetScrollY() + lineH);
    }
    if (!ImGui::IsMouseDown(0)) draggingSelection_ = false;

    // Hover: задержка 450 мс без движения -> запрос textDocument/hover
    if (p != d.hoverPos) { d.hoverPos = p; d.hoverSince = ImGui::GetTime(); d.hoverRequested = false; d.hoverText.clear(); }
    else if (!d.hoverRequested && ImGui::GetTime() - d.hoverSince > 0.45 && callbacks.requestHover) {
        d.hoverRequested = true;
        callbacks.requestHover(d, p);
    }
}

// ---------------------------------------------------------------------------
//  Главная отрисовка
// ---------------------------------------------------------------------------
void EditorCore::render(ImGuiID dockId) {
    for (auto& d : docs_) {
        if (dockId) ImGui::SetNextWindowDockID(dockId, ImGuiCond_FirstUseEver);
        if (d->focusRequested) { ImGui::SetNextWindowFocus(); d->focusRequested = false; }
        ImGuiWindowFlags wf = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (d->buffer.dirty()) wf |= ImGuiWindowFlags_UnsavedDocument;
        std::string title = d->title + "###doc" + std::to_string(d->id);
        ImGui::SetNextWindowBgAlpha(palette.backgroundAlpha);
        if (ImGui::Begin(title.c_str(), &d->open, wf)) {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) activeId_ = d->id;
            renderDocument(*d);
        }
        ImGui::End();
    }
    // Отложенное закрытие (после цикла, чтобы не инвалидировать итераторы)
    std::vector<int> toClose;
    for (auto& d : docs_) if (!d->open) toClose.push_back(d->id);
    for (int id : toClose) close(id);
}

void EditorCore::renderDocument(Document& d) {
    ImGuiIO& io = ImGui::GetIO();
    // --- Панель поиска --------------------------------------------------------
    if (findOpen_ && activeId_ == d.id) {
        ImGui::SetNextItemWidth(260);
        if (findFocus_) { ImGui::SetKeyboardFocusHere(); findFocus_ = false; }
        bool enter = ImGui::InputTextWithHint("##find", "Find / Найти", &findText_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button(">") || enter) && !findText_.empty()) {
            TextPos from = d.cursors.back().range().end;
            for (int pass = 0; pass < 2; ++pass, from = {0, 0}) {
                bool found = false;
                for (int l = from.line; l < d.buffer.lineCount() && !found; ++l) {
                    auto f = d.buffer.line(l).find(findText_, l == from.line ? (std::size_t)from.col : 0);
                    if (f != std::string::npos) {
                        d.cursors = {Cursor{{l, (int)(f + findText_.size())}, {l, (int)f}}};
                        d.scrollToLine = l; found = true;
                    }
                }
                if (found) break;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) findOpen_ = false;
    }

    ImFont* font = ImGui::GetFont();
    ImGui::SetWindowFontScale(fontScale);
    const float lineH   = ImGui::GetTextLineHeightWithSpacing();
    const float charW   = ImGui::CalcTextSize("M").x;
    const int   digits  = std::max(3, (int)std::to_string(d.buffer.lineCount()).size());
    const float gutterW = charW * (float)digits + 34.f;    // точка останова + номера + маркер свёртки
    const float miniW   = showMinimap ? 90.f : 0.f;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##text", ImVec2(avail.x - miniW, avail.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    const bool focused = ImGui::IsWindowFocused();

    if (d.foldsVersion != d.buffer.version()) recomputeFolds(d);
    auto vis = visibleLines(d);

    // Ширина самой длинной видимой строки — для горизонтального скролла
    float maxW = 0;
    for (int l : vis) maxW = std::max(maxW, (float)d.buffer.line(l).size() * charW);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(gutterW + maxW + 200.f, lineH * (float)vis.size() + ImGui::GetWindowHeight() * 0.5f));

    if (focused) handleKeyboard(d);
    handleMouse(d, origin, lineH, gutterW, vis);
    vis = visibleLines(d);   // после правок число строк могло измениться

    // Прокрутка к курсору / строке
    auto rowOf = [&](int line) { return (int)(std::lower_bound(vis.begin(), vis.end(), line) - vis.begin()); };
    const float scrollY = ImGui::GetScrollY(), winH = ImGui::GetWindowHeight();
    if (d.scrollToLine) {
        ImGui::SetScrollY(std::max(0.f, (float)rowOf(*d.scrollToLine) * lineH - winH * 0.35f));
        d.scrollToLine.reset();
    } else if (d.scrollToCursor) {
        float y = (float)rowOf(d.cursors.back().pos.line) * lineH;
        if (y < scrollY) ImGui::SetScrollY(y);
        else if (y + lineH * 2 > scrollY + winH) ImGui::SetScrollY(y + lineH * 2 - winH);
        float x = xOf(d.buffer.line(d.cursors.back().pos.line), d.cursors.back().pos.col) + gutterW;
        float sx = ImGui::GetScrollX(), ww = ImGui::GetWindowWidth();
        if (x < sx + gutterW) ImGui::SetScrollX(std::max(0.f, x - gutterW - 20));
        else if (x > sx + ww - 40) ImGui::SetScrollX(x - ww + 60);
        d.scrollToCursor = false;
    }

    // Отложенный запрос автодополнения (дебаунс)
    if (d.completionDueAt > 0 && ImGui::GetTime() >= d.completionDueAt) {
        d.completionDueAt = -1;
        if (d.completionVersion == d.buffer.version() && d.cursors.size() == 1) {
            TextPos at = d.cursors.front().pos;
            if (callbacks.requestCompletion) callbacks.requestCompletion(d, at);
            // Немедленно показываем локальные варианты; ответ LSP их заменит
            setCompletionResult(d.id, d.buffer.version(), at, {});
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float  scrollX = ImGui::GetScrollX();
    int firstRow = std::max(0, (int)(scrollY / lineH));
    int lastRow  = std::min((int)vis.size() - 1, (int)((scrollY + winH) / lineH) + 1);
    auto bracket = matchBracket(d);

    // Фон редактора (полупрозрачный — для живых тем)
    dl->AddRectFilled(winPos, ImVec2(winPos.x + ImGui::GetWindowWidth(), winPos.y + winH), palette.background);

    std::set<int> cursorLines;
    for (auto& c : d.cursors) cursorLines.insert(c.pos.line);

    for (int row = firstRow; row <= lastRow; ++row) {
        int line = vis[(std::size_t)row];
        const std::string& s = d.buffer.line(line);
        float y  = origin.y + (float)row * lineH;
        float tx = origin.x + gutterW;
        ImVec2 lineMin(winPos.x, y), lineMax(winPos.x + ImGui::GetWindowWidth(), y + lineH);

        if (line == d.executionLine) dl->AddRectFilled(lineMin, lineMax, palette.execLine);
        else if (cursorLines.count(line)) dl->AddRectFilled(lineMin, lineMax, palette.currentLine);

        // --- Выделение ---
        for (auto& c : d.cursors) {
            if (!c.hasSelection()) continue;
            TextRange r = c.range();
            if (line < r.start.line || line > r.end.line) continue;
            float x0 = line == r.start.line ? xOf(s, r.start.col) : 0.f;
            float x1 = line == r.end.line ? xOf(s, r.end.col) : xOf(s, (int)s.size()) + charW * 0.5f;
            dl->AddRectFilled(ImVec2(tx + x0, y), ImVec2(tx + x1, y + lineH), palette.selection);
        }
        // --- Совпадения поиска ---
        if (findOpen_ && !findText_.empty()) {
            for (auto f = s.find(findText_); f != std::string::npos; f = s.find(findText_, f + 1))
                dl->AddRectFilled(ImVec2(tx + xOf(s, (int)f), y), ImVec2(tx + xOf(s, (int)(f + findText_.size())), y + lineH), palette.findMatch);
        }
        // --- Парные скобки ---
        if (bracket) for (TextPos bp : {bracket->first, bracket->second}) if (bp.line == line)
            dl->AddRect(ImVec2(tx + xOf(s, bp.col), y), ImVec2(tx + xOf(s, bp.col + 1), y + lineH), palette.bracket, 2.f, 0, 1.5f);

        // --- Токены ---
        const auto& toks = d.highlighter.tokens(line, d.buffer.lineCount(), [&](int i) -> std::string_view { return d.buffer.line(i); });
        float fs = ImGui::GetFontSize();
        for (const auto& t : toks) {
            float x = tx + xOf(s, t.start);
            if (x - winPos.x > ImGui::GetWindowWidth() + scrollX + 50) break;
            dl->AddText(font, fs, ImVec2(x, y), palette.tokens[(std::size_t)t.kind], s.data() + t.start, s.data() + t.start + t.len);
        }
        // Маркер свёрнутого блока
        if (d.folded.count(line))
            dl->AddText(font, fs, ImVec2(tx + xOf(s, (int)s.size()) + charW, y), palette.lineNumber, " { ... } ");

        // --- Диагностика: волнистое подчёркивание ---
        for (auto& dg : d.diagnostics) {
            if (line < dg.range.start.line || line > dg.range.end.line) continue;
            int c0 = line == dg.range.start.line ? dg.range.start.col : 0;
            int c1 = line == dg.range.end.line ? dg.range.end.col : (int)s.size();
            if (c1 <= c0) c1 = c0 + 1;
            float x0 = tx + xOf(s, c0), x1 = tx + std::max(xOf(s, c1), xOf(s, c0) + charW);
            ImU32 col = dg.severity == 1 ? palette.errorSquiggle : palette.warningSquiggle;
            float by = y + lineH - 2.f;
            for (float x = x0; x < x1; x += 4.f)
                dl->AddLine(ImVec2(x, by), ImVec2(std::min(x + 2.f, x1), by - 2.f), col), dl->AddLine(ImVec2(x + 2.f, by - 2.f), ImVec2(std::min(x + 4.f, x1), by), col);
            ImVec2 mp = ImGui::GetMousePos();
            if (mp.x >= x0 && mp.x <= x1 && mp.y >= y && mp.y <= y + lineH && ImGui::IsWindowHovered())
                ImGui::SetTooltip("[%s] %s", dg.source.c_str(), dg.message.c_str());
        }

        // --- Курсоры (мигание 1 Гц) ---
        bool blinkOn = std::fmod(ImGui::GetTime(), 1.0) < 0.6 || !focused;
        for (auto& c : d.cursors) if (c.pos.line == line && blinkOn) {
            float cx = tx + xOf(s, c.pos.col);
            dl->AddRectFilled(ImVec2(cx, y + 1), ImVec2(cx + 2.f, y + lineH - 1), palette.cursor);
        }

        // --- Ghost text: полупрозрачный предпросмотр + индикатор источника ---
        if (d.ghost.active && d.ghost.anchor.line == line) {
            float gx = tx + xOf(s, d.ghost.anchor.col);
            std::string firstLine = d.ghost.remaining.substr(0, d.ghost.remaining.find('\n'));
            dl->AddText(font, fs, ImVec2(gx, y), palette.ghost, firstLine.c_str());
            float chipX = gx + ImGui::CalcTextSize(firstLine.c_str()).x + charW;
            const auto& cand = d.ghost.candidates[(std::size_t)d.ghost.index % d.ghost.candidates.size()];
            char chip[192];
            std::snprintf(chip, sizeof(chip), " Tab | %s%s%s  %d/%d ", cand.source.c_str(), cand.kind.empty() ? "" : " | ",
                          cand.kind.c_str(), d.ghost.index + 1, (int)d.ghost.candidates.size());
            ImVec2 csz = ImGui::CalcTextSize(chip);
            float sc = 0.85f;
            dl->AddRectFilled(ImVec2(chipX, y + 2), ImVec2(chipX + csz.x * sc, y + lineH - 2), palette.ghostChip, 4.f);
            dl->AddText(font, fs * sc, ImVec2(chipX, y + 2 + (lineH - fs * sc) * 0.25f), IM_COL32(255, 255, 255, 230), chip);
            // Многострочное дополнение: остальные строки «призраком» ниже
            std::size_t nl = d.ghost.remaining.find('\n');
            int extra = 1;
            while (nl != std::string::npos) {
                std::size_t next = d.ghost.remaining.find('\n', nl + 1);
                std::string part = d.ghost.remaining.substr(nl + 1, next == std::string::npos ? std::string::npos : next - nl - 1);
                dl->AddText(font, fs, ImVec2(tx, y + lineH * (float)extra++), palette.ghost, part.c_str());
                nl = next;
            }
            if (!cand.detail.empty() && ImGui::IsWindowFocused())
                dl->AddText(font, fs * 0.8f, ImVec2(gx, y + lineH * (float)extra), palette.lineNumber, cand.detail.c_str());
        }
    }

    // --- Поле номеров строк (фиксировано при горизонтальной прокрутке) ---
    float gx0 = winPos.x;
    dl->AddRectFilled(ImVec2(gx0, winPos.y), ImVec2(gx0 + gutterW - 6, winPos.y + winH), palette.gutter);
    for (int row = firstRow; row <= lastRow; ++row) {
        int line = vis[(std::size_t)row];
        float y = origin.y + (float)row * lineH;
        char num[16]; std::snprintf(num, sizeof(num), "%*d", digits, line + 1);
        dl->AddText(ImVec2(gx0 + 16.f, y), cursorLines.count(line) ? palette.lineNumberActive : palette.lineNumber, num);
        if (d.breakpoints.count(line))
            dl->AddCircleFilled(ImVec2(gx0 + 8.f, y + lineH * 0.5f), lineH * 0.3f, palette.breakpoint);
        if (line == d.executionLine)
            dl->AddTriangleFilled(ImVec2(gx0 + 3, y + 3), ImVec2(gx0 + 3, y + lineH - 3), ImVec2(gx0 + 13, y + lineH * 0.5f), IM_COL32(255, 220, 0, 255));
        bool foldable = std::any_of(d.folds.begin(), d.folds.end(), [&](auto& f) { return f.startLine == line; });
        if (foldable) {
            float fx = gx0 + gutterW - 18.f, cy = y + lineH * 0.5f;
            if (d.folded.count(line)) dl->AddTriangleFilled(ImVec2(fx, cy - 4), ImVec2(fx, cy + 4), ImVec2(fx + 6, cy), palette.lineNumber);
            else dl->AddTriangleFilled(ImVec2(fx - 1, cy - 3), ImVec2(fx + 7, cy - 3), ImVec2(fx + 3, cy + 3), palette.lineNumber);
        }
        // Значок ошибки на поле
        for (auto& dg : d.diagnostics) if (dg.range.start.line == line) {
            dl->AddRectFilled(ImVec2(gx0 + gutterW - 8, y + 2), ImVec2(gx0 + gutterW - 6, y + lineH - 2),
                              dg.severity == 1 ? palette.errorSquiggle : palette.warningSquiggle);
            break;
        }
    }

    // Подсказка hover (LSP или значение переменной отладчика)
    if (!d.hoverText.empty() && ImGui::IsWindowHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.f);
        ImGui::TextUnformatted(d.hoverText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::EndChild();
    (void)io;

    // --- Миникарта ---
    if (showMinimap) {
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("##minimap", ImVec2(miniW, avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        ImDrawList* mdl = ImGui::GetWindowDrawList();
        ImVec2 mp0 = ImGui::GetWindowPos();
        float  mh  = ImGui::GetWindowHeight();
        mdl->AddRectFilled(mp0, ImVec2(mp0.x + miniW, mp0.y + mh), palette.gutter);
        const float rowH = 2.f, colW = 1.2f;
        int maxRows = (int)(mh / rowH);
        int total = (int)vis.size();
        // Окно миникарты центрируется вокруг видимой области
        int start = std::clamp(firstRow - maxRows / 3, 0, std::max(0, total - maxRows));
        for (int r = start; r < std::min(total, start + maxRows); ++r) {
            int line = vis[(std::size_t)r];
            const auto& toks = d.highlighter.tokens(line, d.buffer.lineCount(), [&](int i) -> std::string_view { return d.buffer.line(i); });
            float y = mp0.y + (float)(r - start) * rowH;
            for (auto& t : toks) {
                if (t.kind == TokenKind::Default) continue;
                float x0 = mp0.x + 4 + (float)t.start * colW, x1 = std::min(mp0.x + miniW - 2, x0 + (float)t.len * colW);
                if (x0 >= x1) continue;
                ImU32 col = (palette.tokens[(std::size_t)t.kind] & 0x00FFFFFF) | 0xA0000000;
                mdl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rowH - 0.5f), col);
            }
        }
        float vy0 = mp0.y + (float)(firstRow - start) * rowH, vy1 = vy0 + (float)(lastRow - firstRow + 1) * rowH;
        mdl->AddRectFilled(ImVec2(mp0.x, vy0), ImVec2(mp0.x + miniW, vy1), IM_COL32(255, 255, 255, 25));
        if (ImGui::IsWindowHovered() && ImGui::IsMouseDown(0)) {
            int r = start + (int)((ImGui::GetMousePos().y - mp0.y) / rowH);
            r = std::clamp(r, 0, std::max(0, total - 1));
            d.scrollToLine = vis[(std::size_t)r];
        }
        ImGui::EndChild();
    }
}

} // namespace ide
