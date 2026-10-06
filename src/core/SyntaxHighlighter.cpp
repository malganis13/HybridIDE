// =============================================================================
//  SyntaxHighlighter.cpp — токенизатор и встроенные определения языков
// =============================================================================
#include "core/SyntaxHighlighter.hpp"

#include <algorithm>
#include <cctype>

namespace ide {

void SyntaxHighlighter::invalidateFrom(int line) {
    // Ограничиваем «грязную» границу; ensure() пересчитает лениво
    firstInvalid_ = std::min(firstInvalid_, std::max(0, line));
    for (std::size_t i = (std::size_t)firstInvalid_; i < cache_.size(); ++i) cache_[i].valid = false;
}

static bool startsWith(std::string_view s, std::size_t i, std::string_view p) {
    return !p.empty() && s.size() >= i + p.size() && s.compare(i, p.size(), p) == 0;
}

std::vector<Token> SyntaxHighlighter::tokenizeLine(const LanguageDef* lang, std::string_view s, State& state) {
    std::vector<Token> out;
    if (!lang) { if (!s.empty()) out.push_back({0, (int)s.size(), TokenKind::Default}); return out; }
    std::size_t i = 0, n = s.size();
    auto push = [&](std::size_t a, std::size_t b, TokenKind k) { if (b > a) out.push_back({(int)a, (int)(b - a), k}); };

    // Продолжение многострочных конструкций с предыдущей строки
    if (state == InBlockComment) {
        auto e = s.find(lang->blockEnd);
        if (e == std::string_view::npos) { push(0, n, TokenKind::Comment); return out; }
        i = e + lang->blockEnd.size(); push(0, i, TokenKind::Comment); state = Normal;
    } else if (state == InTripleDq || state == InTripleSq) {
        auto e = s.find(state == InTripleDq ? "\"\"\"" : "'''");
        if (e == std::string_view::npos) { push(0, n, TokenKind::String); return out; }
        i = e + 3; push(0, i, TokenKind::String); state = Normal;
    }

    // Директивы препроцессора: вся строка (до комментария)
    if (lang->hasPreprocessor) {
        std::size_t j = i; while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
        if (j < n && s[j] == '#') {
            std::size_t k = j + 1; while (k < n && std::isalpha((unsigned char)s[k])) ++k;
            push(j, k, TokenKind::Preprocessor);
            i = k;
            // #include <...> подсвечиваем как строку
            std::size_t a = s.find('<', i);
            if (s.substr(j, k - j) == "#include" && a != std::string_view::npos) {
                std::size_t b = s.find('>', a);
                if (b != std::string_view::npos) { push(i, a, TokenKind::Default); push(a, b + 1, TokenKind::String); i = b + 1; }
            }
        }
    }

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (startsWith(s, i, lang->lineComment)) { push(i, n, TokenKind::Comment); break; }
        if (startsWith(s, i, lang->blockStart)) {
            auto e = s.find(lang->blockEnd, i + lang->blockStart.size());
            if (e == std::string_view::npos) { push(i, n, TokenKind::Comment); state = InBlockComment; break; }
            push(i, e + lang->blockEnd.size(), TokenKind::Comment); i = e + lang->blockEnd.size(); continue;
        }
        if (lang->tripleQuoteStrings && (startsWith(s, i, "\"\"\"") || startsWith(s, i, "'''"))) {
            std::string_view q = s.substr(i, 3);
            auto e = s.find(q, i + 3);
            if (e == std::string_view::npos) { push(i, n, TokenKind::String); state = q[0] == '"' ? InTripleDq : InTripleSq; break; }
            push(i, e + 3, TokenKind::String); i = e + 3; continue;
        }
        if (lang->rawStringsCpp && startsWith(s, i, "R\"(")) {
            auto e = s.find(")\"", i + 3);
            std::size_t end = e == std::string_view::npos ? n : e + 2;
            push(i, end, TokenKind::String); i = end; continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && lang->id == "javascript")) {
            std::size_t j = i + 1;
            while (j < n && (unsigned char)s[j] != c) { if (s[j] == '\\') ++j; ++j; }
            j = std::min(j + 1, n);
            bool isChar = c == '\'' && lang->id != "python" && lang->id != "javascript";
            // В Rust ' — также лайфтайм ('a): если нет закрывающей кавычки рядом — атрибут
            if (c == '\'' && lang->id == "rust" && (j - i) > 4) { std::size_t k = i + 1; while (k < n && isalnum((unsigned char)s[k])) ++k; push(i, k, TokenKind::Attribute); i = k; continue; }
            push(i, j, isChar ? TokenKind::Char : TokenKind::String); i = j; continue;
        }
        if (std::isdigit(c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            std::size_t j = i + 1;
            while (j < n && (std::isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '\'' || s[j] == '_')) ++j;
            push(i, j, TokenKind::Number); i = j; continue;
        }
        if (std::isalpha(c) || c == '_' || c >= 0x80) {
            std::size_t j = i + 1;
            while (j < n && (std::isalnum((unsigned char)s[j]) || s[j] == '_' || (unsigned char)s[j] >= 0x80)) ++j;
            std::string word(s.substr(i, j - i));
            TokenKind k = TokenKind::Identifier;
            if (lang->keywords.count(word))       k = TokenKind::Keyword;
            else if (lang->types.count(word))     k = TokenKind::Type;
            else if (lang->builtins.count(word))  k = TokenKind::Function;
            else {
                std::size_t m = j; while (m < n && s[m] == ' ') ++m;
                if (m < n && (s[m] == '(' || (s[m] == '!' && lang->id == "rust"))) k = TokenKind::Function;
                else if (!word.empty() && std::isupper((unsigned char)word[0]) && lang->id != "cmake") k = TokenKind::Type;
            }
            push(i, j, k); i = j; continue;
        }
        if (c == '@' || (c == '#' && lang->id == "rust")) {   // декораторы / атрибуты
            std::size_t j = i + 1; while (j < n && (std::isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '[' || s[j] == '!')) ++j;
            push(i, j, TokenKind::Attribute); i = j; continue;
        }
        if (std::ispunct(c)) {
            bool br = c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' || c == ';' || c == ',';
            push(i, i + 1, br ? TokenKind::Punctuation : TokenKind::Operator); ++i; continue;
        }
        std::size_t j = i + 1; while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
        push(i, j, TokenKind::Default); i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Встроенные языки. Сервер LSP по умолчанию можно переопределить в настройках.
// ---------------------------------------------------------------------------
LanguageRegistry& LanguageRegistry::instance() { static LanguageRegistry r; return r; }

LanguageRegistry::LanguageRegistry() {
    LanguageDef cpp;
    cpp.id = "cpp"; cpp.displayName = "C++"; cpp.lspLanguageId = "cpp"; cpp.lspCommand = "clangd --background-index";
    cpp.extensions = {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx", ".h", ".c", ".ipp", ".ixx", ".cppm", ".inl"};
    cpp.keywords = {"alignas","alignof","asm","auto","break","case","catch","class","co_await","co_return","co_yield","concept",
        "const","consteval","constexpr","constinit","const_cast","continue","decltype","default","delete","do","dynamic_cast",
        "else","enum","explicit","export","extern","false","final","for","friend","goto","if","import","inline","module",
        "mutable","namespace","new","noexcept","nullptr","operator","override","private","protected","public","register",
        "reinterpret_cast","requires","return","sizeof","static","static_assert","static_cast","struct","switch","template",
        "this","thread_local","throw","true","try","typedef","typeid","typename","union","using","virtual","volatile","while"};
    cpp.types = {"bool","char","char8_t","char16_t","char32_t","double","float","int","long","short","signed","unsigned",
        "void","wchar_t","size_t","int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t","std","string","vector"};
    cpp.lineComment = "//"; cpp.blockStart = "/*"; cpp.blockEnd = "*/"; cpp.hasPreprocessor = true; cpp.rawStringsCpp = true;
    langs_.push_back(cpp);

    LanguageDef rs;
    rs.id = "rust"; rs.displayName = "Rust"; rs.lspLanguageId = "rust"; rs.lspCommand = "rust-analyzer"; rs.extensions = {".rs"};
    rs.keywords = {"as","async","await","break","const","continue","crate","dyn","else","enum","extern","false","fn","for","if",
        "impl","in","let","loop","match","mod","move","mut","pub","ref","return","self","Self","static","struct","super","trait",
        "true","type","unsafe","use","where","while","macro_rules"};
    rs.types = {"i8","i16","i32","i64","i128","isize","u8","u16","u32","u64","u128","usize","f32","f64","bool","char","str","String","Vec","Option","Result","Box"};
    rs.lineComment = "//"; rs.blockStart = "/*"; rs.blockEnd = "*/";
    langs_.push_back(rs);

    LanguageDef py;
    py.id = "python"; py.displayName = "Python"; py.lspLanguageId = "python"; py.lspCommand = "pyright-langserver --stdio";
    py.extensions = {".py", ".pyw", ".pyi"};
    py.keywords = {"False","None","True","and","as","assert","async","await","break","class","continue","def","del","elif","else",
        "except","finally","for","from","global","if","import","in","is","lambda","nonlocal","not","or","pass","raise","return",
        "try","while","with","yield","match","case"};
    py.types = {"int","float","str","bytes","list","dict","set","tuple","bool","object"};
    py.builtins = {"print","len","range","open","isinstance","super","enumerate","zip","map","filter","sorted","min","max","sum"};
    py.lineComment = "#"; py.tripleQuoteStrings = true;
    langs_.push_back(py);

    LanguageDef go;
    go.id = "go"; go.displayName = "Go"; go.lspLanguageId = "go"; go.lspCommand = "gopls"; go.extensions = {".go"};
    go.keywords = {"break","case","chan","const","continue","default","defer","else","fallthrough","for","func","go","goto","if",
        "import","interface","map","package","range","return","select","struct","switch","type","var","nil","true","false","iota"};
    go.types = {"bool","byte","complex64","complex128","error","float32","float64","int","int8","int16","int32","int64","rune",
        "string","uint","uint8","uint16","uint32","uint64","uintptr","any"};
    go.builtins = {"append","cap","close","copy","delete","len","make","new","panic","print","println","recover"};
    go.lineComment = "//"; go.blockStart = "/*"; go.blockEnd = "*/";
    langs_.push_back(go);

    LanguageDef js;
    js.id = "javascript"; js.displayName = "JavaScript / TypeScript"; js.lspLanguageId = "typescript";
    js.lspCommand = "typescript-language-server --stdio";
    js.extensions = {".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx", ".mts"};
    js.keywords = {"async","await","break","case","catch","class","const","continue","debugger","default","delete","do","else",
        "export","extends","false","finally","for","from","function","if","import","in","instanceof","interface","let","new",
        "null","of","return","super","switch","this","throw","true","try","type","typeof","undefined","var","void","while",
        "with","yield","enum","implements","private","protected","public","readonly","declare","namespace","as"};
    js.types = {"string","number","boolean","any","unknown","never","object","Promise","Array","Record","Map","Set"};
    js.builtins = {"console","require","JSON","Math","Object"};
    js.lineComment = "//"; js.blockStart = "/*"; js.blockEnd = "*/";
    langs_.push_back(js);

    LanguageDef json;
    json.id = "json"; json.displayName = "JSON"; json.lspLanguageId = "json"; json.extensions = {".json", ".jsonc"};
    json.keywords = {"true", "false", "null"}; json.lineComment = "//";
    langs_.push_back(json);

    LanguageDef cm;
    cm.id = "cmake"; cm.displayName = "CMake"; cm.lspLanguageId = "cmake"; cm.lspCommand = "cmake-language-server";
    cm.extensions = {".cmake", "CMakeLists.txt"};
    cm.keywords = {"if","elseif","else","endif","foreach","endforeach","while","endwhile","function","endfunction","macro",
        "endmacro","return","set","option","project","add_executable","add_library","target_link_libraries",
        "target_include_directories","find_package","include","message","cmake_minimum_required","install"};
    cm.lineComment = "#";
    langs_.push_back(cm);

    LanguageDef toml;
    toml.id = "toml"; toml.displayName = "TOML"; toml.extensions = {".toml"}; toml.keywords = {"true", "false"}; toml.lineComment = "#";
    langs_.push_back(toml);

    LanguageDef glsl = cpp;
    glsl.id = "glsl"; glsl.displayName = "GLSL"; glsl.lspCommand = ""; glsl.lspLanguageId = "glsl";
    glsl.extensions = {".glsl", ".vert", ".frag", ".comp"};
    glsl.keywords.insert({"uniform", "in", "out", "inout", "layout", "precision", "highp", "mediump", "lowp"});
    glsl.types.insert({"vec2", "vec3", "vec4", "mat2", "mat3", "mat4", "sampler2D", "ivec2"});
    langs_.push_back(glsl);
}

const LanguageDef* LanguageRegistry::byExtension(std::string_view ext) const {
    std::string e(ext);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (const auto& l : langs_)
        for (const auto& x : l.extensions) {
            std::string xl = x;
            std::transform(xl.begin(), xl.end(), xl.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (xl == e) return &l;
        }
    return nullptr;
}

const LanguageDef* LanguageRegistry::byId(std::string_view id) const {
    for (const auto& l : langs_) if (l.id == id) return &l;
    return nullptr;
}

} // namespace ide
