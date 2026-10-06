#pragma once
// ============================================================================
//  Localization — горячее переключение языка интерфейса (ru-RU / en-US).
//
//  * Встроенные словари компилируются в бинарник: IDE работает «из коробки»
//    даже без каталога resources/.
//  * Файлы resources/locales/<lang>.json (плоский объект "ключ": "строка")
//    накладываются поверх встроенных строк — так переводчики/пользователи
//    могут править тексты без пересборки и добавлять новые языки.
//  * setLanguage() меняет активную таблицу мгновенно: ImGui перерисовывает
//    интерфейс каждый кадр, поэтому перезапуск не нужен.
//
//  ВАЖНО про ImGui ID: строка окна, возвращаемая tr(), содержит суффикс
//  "###<стабильный-id>", чтобы при смене языка окна не теряли положение в
//  докинг-раскладке (ImGui хеширует только часть после "###").
//
//  tr() возвращает const char*, валидный до следующего setLanguage()/
//  loadOverrides() — вызывайте его каждый кадр, не кешируйте указатель.
//  Работа с классом — только из главного (UI) потока.
// ============================================================================
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ide {

class Localization {
public:
    static Localization& instance();

    // Перевод по ключу; неизвестный ключ возвращается как есть (видно в UI,
    // что перевод забыли, и это ловит юнит-тест).
    [[nodiscard]] const char* tr(const char* key) const;
    [[nodiscard]] const char* tr(const std::string& key) const { return tr(key.c_str()); }
    [[nodiscard]] bool has(const std::string& key) const;

    [[nodiscard]] const std::string& language() const { return lang_; }
    bool setLanguage(const std::string& lang);                       // false — неизвестный язык
    [[nodiscard]] std::vector<std::string> availableLanguages() const;
    [[nodiscard]] static const char* languageDisplayName(const std::string& lang);

    // Загрузить JSON-переопределения из каталога (<dir>/<lang>.json).
    void loadOverrides(const std::filesystem::path& dir);

    // Подписка на смену языка (например, пересобрать шрифты или меню ОС).
    void onLanguageChanged(std::function<void(const std::string&)> cb) { listeners_.push_back(std::move(cb)); }

    // Список всех встроенных ключей (для тестов полноты перевода).
    [[nodiscard]] std::vector<std::string> builtinKeys() const;

private:
    Localization();
    void rebuild();

    using Table = std::unordered_map<std::string, std::string>;
    std::unordered_map<std::string, Table> builtin_;     // lang -> table
    std::unordered_map<std::string, Table> overrides_;   // lang -> table (из JSON)
    Table        active_;                                 // итоговая таблица текущего языка
    std::string  lang_ = "ru-RU";
    std::vector<std::function<void(const std::string&)>> listeners_;
};

// Короткий помощник: ide::tr("menu.file").
inline const char* tr(const char* key) { return Localization::instance().tr(key); }

} // namespace ide
