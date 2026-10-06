// ============================================================================
//  main.cpp — точка входа HybridIDE.
//
//  Использование:
//      HybridIDE [путь-к-папке-или-файлу] [--safe-mode]
//  --safe-mode — запуск без шейдеров и звука (если драйвер GPU/аудио
//                ведёт себя некорректно).
// ============================================================================
#include "app/Application.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);    // корректный вывод кириллицы в консоль
#endif
    ide::AppOptions opts;
    std::error_code ec;
    opts.exeDir = std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), ec).parent_path();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--safe-mode") == 0) opts.safeMode = true;
        else if (std::strcmp(argv[i], "--version") == 0) { std::printf("HybridIDE %s\n", IDE_VERSION); return 0; }
        else opts.openPath = std::filesystem::u8path(argv[i]);
    }
    try {
        ide::Application app(std::move(opts));
        return app.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 2;
    }
}
