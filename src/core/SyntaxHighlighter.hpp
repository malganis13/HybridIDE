// =============================================================================
//  SyntaxHighlighter.hpp — быстрый табличный токенизатор подсветки синтаксиса.
//  Работает построчно с переносом состояния (многострочные комментарии/строки),
//  поэтому при правке пересчитываются только изменённые строки и ниже.
//  Архитектурно заменяем на tree-sitter (интерфейс ILanguageTokenizer).
// =============================================================================
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ide {

enum class TokenKind : std::uint8_t {
    Default, Keyword, Type, Identifier, Function, Number, String, Char,
    Comment, Preprocessor, Operator, Punctuation, Attribute, Count
};

struct Token {
    int       start = 0;   // байтовый столбец
    int       len   = 0;
    TokenKind kind  = TokenKind::Default;
};

struct LanguageDef {
    std::string                     id;              // "cpp", "rust", "python"...
    std::string                     displayName;
    std::vector<std::string>        extensions;
    std::unordered_set<std::string> keywords, types, builtins;
    std::string                     lineComment;     // "//" или "#"
    std::string                     blockStart, blockEnd;
    bool                            hasPreprocessor = false;
    bool                            tripleQuoteStrings = false;   // Python
    bool                            rawStringsCpp      = false;   // R"( )"
    std::string                     lspCommand;      // сервер по умолчанию
    std::string                     lspLanguageId;
};

class SyntaxHighlighter {
public:
    // Состояние на конец строки — передаётся следующей строке
    enum State : std::uint8_t { Normal = 0, InBlockComment = 1, InTripleDq = 2, InTripleSq = 3 };

    explicit SyntaxHighlighter(const LanguageDef* lang = nullptr) : lang_(lang) {}
    void setLanguage(const LanguageDef* lang) { lang_ = lang; invalidateFrom(0); }
    [[nodiscard]] const LanguageDef* language() const { return lang_; }

    // Инвалидация кэша начиная со строки (после правки)
    void invalidateFrom(int line);
    // Получить токены строки (кэшируется). getLine(i) — доступ к тексту
    template <typename GetLine>
    const std::vector<Token>& tokens(int line, int lineCount, GetLine&& getLine) {
        ensure(line, lineCount, getLine);
        return cache_[(std::size_t)line].tokens;
    }

    static std::vector<Token> tokenizeLine(const LanguageDef* lang, std::string_view s, State& state);

private:
    struct LineCache { std::vector<Token> tokens; State endState = Normal; bool valid = false; };
    template <typename GetLine>
    void ensure(int line, int lineCount, GetLine& getLine) {
        if ((int)cache_.size() != lineCount) {
            cache_.resize((std::size_t)lineCount);
            firstInvalid_ = std::min(firstInvalid_, lineCount);
        }
        if (line < firstInvalid_ && cache_[(std::size_t)line].valid) return;   // строка уже актуальна
        int from = std::min(firstInvalid_, line);
        State st = from > 0 ? cache_[(std::size_t)from - 1].endState : Normal;
        for (int i = from; i <= line; ++i) {
            auto& c    = cache_[(std::size_t)i];
            c.tokens   = tokenizeLine(lang_, getLine(i), st);
            c.endState = st;
            c.valid    = true;
        }
        firstInvalid_ = line + 1;
    }

    const LanguageDef*     lang_ = nullptr;
    std::vector<LineCache> cache_;
    int                    firstInvalid_ = 0;
};

// Реестр встроенных языков
class LanguageRegistry {
public:
    static LanguageRegistry& instance();
    [[nodiscard]] const LanguageDef* byExtension(std::string_view ext) const;
    [[nodiscard]] const LanguageDef* byId(std::string_view id) const;
    [[nodiscard]] const std::vector<LanguageDef>& all() const { return langs_; }
private:
    LanguageRegistry();
    std::vector<LanguageDef> langs_;
};

} // namespace ide
