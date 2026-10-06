// =============================================================================
//  TextBuffer.hpp — модель текста документа: строки, позиции, правки,
//  транзакционная история Undo/Redo с группировкой и счётчик версий для LSP.
// =============================================================================
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ide {

// Позиция в документе: строка и байтовый столбец (UTF-8)
struct TextPos {
    int line = 0;
    int col  = 0;
    auto operator<=>(const TextPos&) const = default;
};

struct TextRange {
    TextPos start, end;
    [[nodiscard]] bool empty() const { return start == end; }
    [[nodiscard]] TextRange normalized() const { return start <= end ? *this : TextRange{end, start}; }
};

// Атомарная правка: заменить текст в range на text
struct TextEdit {
    TextRange   range;
    std::string removed;   // удалённый текст (для undo)
    std::string inserted;
};

enum class LineEnding { LF, CRLF };

class TextBuffer {
public:
    TextBuffer() : lines_{""} {}

    // --- Файловые операции ---------------------------------------------------
    bool loadFromFile(const std::filesystem::path& p, std::string* err = nullptr);
    bool saveToFile(const std::filesystem::path& p, std::string* err = nullptr);
    void setText(std::string_view text);
    [[nodiscard]] std::string text() const;

    // --- Доступ -------------------------------------------------------------
    [[nodiscard]] int lineCount() const { return (int)lines_.size(); }
    [[nodiscard]] const std::string& line(int i) const { return lines_[(std::size_t)i]; }
    [[nodiscard]] std::string getText(TextRange r) const;
    [[nodiscard]] TextPos clamp(TextPos p) const;
    [[nodiscard]] TextPos endPos() const { return {lineCount() - 1, (int)lines_.back().size()}; }

    // Перемещения с учётом UTF-8 (не разрываем многобайтовые символы)
    [[nodiscard]] TextPos nextChar(TextPos p) const;
    [[nodiscard]] TextPos prevChar(TextPos p) const;
    [[nodiscard]] TextPos nextWord(TextPos p) const;
    [[nodiscard]] TextPos prevWord(TextPos p) const;
    [[nodiscard]] TextRange wordAt(TextPos p) const;

    // --- Правки -------------------------------------------------------------
    // Вставляет/заменяет текст, возвращает позицию конца вставки
    TextPos replace(TextRange r, std::string_view text, bool recordUndo = true);
    TextPos insert(TextPos p, std::string_view text) { return replace({p, p}, text); }
    void    erase(TextRange r) { replace(r, ""); }

    // Группировка правок в одну транзакцию (мультикурсорный ввод = 1 undo)
    void beginGroup();
    void endGroup();
    bool undo(std::vector<TextPos>* cursorsOut = nullptr);
    bool redo(std::vector<TextPos>* cursorsOut = nullptr);
    [[nodiscard]] bool canUndo() const { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const { return !redo_.empty(); }

    // --- Состояние ------------------------------------------------------------
    [[nodiscard]] std::uint64_t version() const { return version_; }   // для LSP didChange
    [[nodiscard]] bool dirty() const { return version_ != savedVersion_; }
    [[nodiscard]] LineEnding lineEnding() const { return eol_; }

    // Последние правки с момента takePendingEdits (инкрементальная синхронизация LSP)
    std::vector<TextEdit> takePendingEdits() { return std::exchange(pending_, {}); }

    // Конвертация столбцов: байты UTF-8 <-> UTF-16 code units (LSP по умолчанию UTF-16)
    [[nodiscard]] int byteToUtf16(int line, int byteCol) const;
    [[nodiscard]] int utf16ToByte(int line, int u16Col) const;

private:
    TextPos applyRaw(TextRange r, std::string_view text, std::string* removedOut);

    using Group = std::vector<TextEdit>;
    std::vector<std::string> lines_;
    std::vector<Group>       undo_, redo_;
    Group                    openGroup_;
    int                      groupDepth_ = 0;
    std::vector<TextEdit>    pending_;
    std::uint64_t            version_ = 1, savedVersion_ = 1;
    LineEnding               eol_ = LineEnding::LF;
};

bool isWordChar(unsigned char c);

} // namespace ide
