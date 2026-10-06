// =============================================================================
//  TextBuffer.cpp — реализация модели текста
// =============================================================================
#include "core/TextBuffer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace ide {

bool isWordChar(unsigned char c) {
    // Буквы/цифры/подчёркивание + все байты UTF-8 > 0x7F (кириллица в идентификаторах)
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

static bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

bool TextBuffer::loadFromFile(const std::filesystem::path& p, std::string* err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { if (err) *err = "Не удалось открыть файл: " + p.string(); return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    // Срезаем UTF-8 BOM
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);
    eol_ = data.find("\r\n") != std::string::npos ? LineEnding::CRLF : LineEnding::LF;
    setText(data);
    undo_.clear(); redo_.clear(); pending_.clear();
    savedVersion_ = version_;
    return true;
}

bool TextBuffer::saveToFile(const std::filesystem::path& p, std::string* err) {
    // Атомарное сохранение: пишем во временный файл, затем переименовываем
    auto tmp = p; tmp += ".ide_tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "Не удалось записать: " + tmp.string(); return false; }
        const char* nl = eol_ == LineEnding::CRLF ? "\r\n" : "\n";
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            f << lines_[i];
            if (i + 1 < lines_.size()) f << nl;
        }
        if (!f) { if (err) *err = "Ошибка записи на диск"; return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        // На Windows rename поверх существующего файла может не сработать
        std::filesystem::copy_file(tmp, p, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp);
        if (ec) { if (err) *err = ec.message(); return false; }
    }
    savedVersion_ = version_;
    return true;
}

void TextBuffer::setText(std::string_view text) {
    lines_.clear();
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') { if (i + 1 < text.size() && text[i + 1] == '\n') continue; lines_.push_back(std::move(cur)); cur.clear(); }
        else if (c == '\n') { lines_.push_back(std::move(cur)); cur.clear(); }
        else cur += c;
    }
    lines_.push_back(std::move(cur));
    ++version_;
}

std::string TextBuffer::text() const {
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        out += lines_[i];
        if (i + 1 < lines_.size()) out += '\n';
    }
    return out;
}

TextPos TextBuffer::clamp(TextPos p) const {
    p.line = std::clamp(p.line, 0, lineCount() - 1);
    p.col  = std::clamp(p.col, 0, (int)lines_[(std::size_t)p.line].size());
    return p;
}

std::string TextBuffer::getText(TextRange r) const {
    r = r.normalized();
    r.start = clamp(r.start); r.end = clamp(r.end);
    if (r.start.line == r.end.line)
        return lines_[(std::size_t)r.start.line].substr((std::size_t)r.start.col, (std::size_t)(r.end.col - r.start.col));
    std::string out = lines_[(std::size_t)r.start.line].substr((std::size_t)r.start.col);
    for (int l = r.start.line + 1; l < r.end.line; ++l) { out += '\n'; out += lines_[(std::size_t)l]; }
    out += '\n';
    out += lines_[(std::size_t)r.end.line].substr(0, (std::size_t)r.end.col);
    return out;
}

TextPos TextBuffer::nextChar(TextPos p) const {
    p = clamp(p);
    const auto& s = lines_[(std::size_t)p.line];
    if (p.col >= (int)s.size()) return p.line + 1 < lineCount() ? TextPos{p.line + 1, 0} : p;
    ++p.col;
    while (p.col < (int)s.size() && isContinuation((unsigned char)s[(std::size_t)p.col])) ++p.col;
    return p;
}

TextPos TextBuffer::prevChar(TextPos p) const {
    p = clamp(p);
    if (p.col == 0) return p.line > 0 ? TextPos{p.line - 1, (int)lines_[(std::size_t)p.line - 1].size()} : p;
    const auto& s = lines_[(std::size_t)p.line];
    --p.col;
    while (p.col > 0 && isContinuation((unsigned char)s[(std::size_t)p.col])) --p.col;
    return p;
}

TextPos TextBuffer::nextWord(TextPos p) const {
    p = clamp(p);
    const auto& s = lines_[(std::size_t)p.line];
    if (p.col >= (int)s.size()) return nextChar(p);
    bool w = isWordChar((unsigned char)s[(std::size_t)p.col]);
    while (p.col < (int)s.size() && isWordChar((unsigned char)s[(std::size_t)p.col]) == w && s[(std::size_t)p.col] != ' ') ++p.col;
    while (p.col < (int)s.size() && s[(std::size_t)p.col] == ' ') ++p.col;
    return p;
}

TextPos TextBuffer::prevWord(TextPos p) const {
    p = clamp(p);
    if (p.col == 0) return prevChar(p);
    const auto& s = lines_[(std::size_t)p.line];
    while (p.col > 0 && s[(std::size_t)p.col - 1] == ' ') --p.col;
    if (p.col == 0) return p;
    bool w = isWordChar((unsigned char)s[(std::size_t)p.col - 1]);
    while (p.col > 0 && isWordChar((unsigned char)s[(std::size_t)p.col - 1]) == w && s[(std::size_t)p.col - 1] != ' ') --p.col;
    return p;
}

TextRange TextBuffer::wordAt(TextPos p) const {
    p = clamp(p);
    const auto& s = lines_[(std::size_t)p.line];
    int a = p.col, b = p.col;
    while (a > 0 && isWordChar((unsigned char)s[(std::size_t)a - 1])) --a;
    while (b < (int)s.size() && isWordChar((unsigned char)s[(std::size_t)b])) ++b;
    return {{p.line, a}, {p.line, b}};
}

TextPos TextBuffer::applyRaw(TextRange r, std::string_view text, std::string* removedOut) {
    r = r.normalized();
    r.start = clamp(r.start); r.end = clamp(r.end);
    if (removedOut) *removedOut = getText(r);

    // Склеиваем «хвост» последней строки диапазона
    std::string head = lines_[(std::size_t)r.start.line].substr(0, (std::size_t)r.start.col);
    std::string tail = lines_[(std::size_t)r.end.line].substr((std::size_t)r.end.col);
    lines_.erase(lines_.begin() + r.start.line + 1, lines_.begin() + r.end.line + 1);

    // Разбиваем вставляемый текст на строки
    std::vector<std::string> ins{""};
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') ins.emplace_back(); else ins.back() += c;
    }
    TextPos endP{r.start.line + (int)ins.size() - 1, 0};
    if (ins.size() == 1) {
        endP.col = (int)(head.size() + ins[0].size());
        lines_[(std::size_t)r.start.line] = head + ins[0] + tail;
    } else {
        lines_[(std::size_t)r.start.line] = head + ins[0];
        endP.col = (int)ins.back().size();
        ins.back() += tail;
        lines_.insert(lines_.begin() + r.start.line + 1, ins.begin() + 1, ins.end());
    }
    ++version_;
    return endP;
}

TextPos TextBuffer::replace(TextRange r, std::string_view text, bool recordUndo) {
    TextEdit e;
    e.range    = r.normalized();
    e.inserted = std::string(text);
    TextPos endP = applyRaw(r, text, &e.removed);
    pending_.push_back({e.range, e.removed, e.inserted});
    if (recordUndo) {
        redo_.clear();
        if (groupDepth_ > 0) openGroup_.push_back(std::move(e));
        else {
            // Слияние посимвольного ввода в одну запись undo
            if (!undo_.empty() && undo_.back().size() == 1 && e.removed.empty() && e.inserted.size() == 1 &&
                e.inserted[0] != '\n' && e.inserted[0] != ' ') {
                auto& prev = undo_.back().back();
                TextPos prevEnd = prev.range.start;
                prevEnd.col += (int)prev.inserted.size();
                if (prev.removed.empty() && prev.inserted.find('\n') == std::string::npos && prevEnd == e.range.start) {
                    prev.inserted += e.inserted;
                    return endP;
                }
            }
            undo_.push_back({std::move(e)});
            if (undo_.size() > 2000) undo_.erase(undo_.begin());   // ограничение памяти истории
        }
    }
    return endP;
}

void TextBuffer::beginGroup() { if (groupDepth_++ == 0) openGroup_.clear(); }
void TextBuffer::endGroup() {
    if (groupDepth_ == 0) return;
    if (--groupDepth_ == 0 && !openGroup_.empty()) undo_.push_back(std::move(openGroup_)), openGroup_.clear();
}

// Конец вставленного текста, начиная с позиции start
static TextPos endOfInserted(TextPos start, const std::string& s) {
    TextPos p = start;
    for (char c : s) { if (c == '\n') { ++p.line; p.col = 0; } else ++p.col; }
    return p;
}

bool TextBuffer::undo(std::vector<TextPos>* cursorsOut) {
    if (undo_.empty()) return false;
    Group g = std::move(undo_.back());
    undo_.pop_back();
    if (cursorsOut) cursorsOut->clear();
    for (auto it = g.rbegin(); it != g.rend(); ++it) {
        TextRange insertedRange{it->range.start, endOfInserted(it->range.start, it->inserted)};
        TextPos p = applyRaw(insertedRange, it->removed, nullptr);
        pending_.push_back({insertedRange, it->inserted, it->removed});
        if (cursorsOut) cursorsOut->push_back(p);
    }
    redo_.push_back(std::move(g));
    return true;
}

bool TextBuffer::redo(std::vector<TextPos>* cursorsOut) {
    if (redo_.empty()) return false;
    Group g = std::move(redo_.back());
    redo_.pop_back();
    if (cursorsOut) cursorsOut->clear();
    for (auto& e : g) {
        TextRange removedRange{e.range.start, endOfInserted(e.range.start, e.removed)};
        TextPos p = applyRaw(removedRange, e.inserted, nullptr);
        pending_.push_back({removedRange, e.removed, e.inserted});
        if (cursorsOut) cursorsOut->push_back(p);
    }
    undo_.push_back(std::move(g));
    return true;
}

int TextBuffer::byteToUtf16(int line, int byteCol) const {
    const auto& s = lines_[(std::size_t)std::clamp(line, 0, lineCount() - 1)];
    int u16 = 0;
    for (int i = 0; i < byteCol && i < (int)s.size();) {
        unsigned char c = (unsigned char)s[(std::size_t)i];
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        u16 += len == 4 ? 2 : 1;   // символы вне BMP — суррогатная пара
        i += len;
    }
    return u16;
}

int TextBuffer::utf16ToByte(int line, int u16Col) const {
    const auto& s = lines_[(std::size_t)std::clamp(line, 0, lineCount() - 1)];
    int u16 = 0, i = 0;
    while (i < (int)s.size() && u16 < u16Col) {
        unsigned char c = (unsigned char)s[(std::size_t)i];
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        u16 += len == 4 ? 2 : 1;
        i += len;
    }
    return std::min(i, (int)s.size());
}

} // namespace ide
