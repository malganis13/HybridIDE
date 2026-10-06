# =============================================================================
#  HybridIDE — универсальная C++20/23 IDE (VS Code + Visual Studio Community)
#  Кроссплатформенная система сборки: Windows (MSVC 2022 / MinGW), Linux, macOS
# =============================================================================
cmake_minimum_required(VERSION 3.20)

# Статический CRT для MSVC: HybridIDE.exe запускается на чистой Windows 10/11
# без установки Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

project(HybridIDE
    VERSION 1.0.0
    DESCRIPTION "Universal hybrid C++ IDE: Dear ImGui + LSP + DAP + GitHub + shader themes"
    LANGUAGES C CXX)

# ---- Опции сборки -----------------------------------------------------------
option(IDE_BUILD_APP      "Собирать графическое приложение (GLFW + OpenGL)" ON)
option(IDE_BUILD_TESTS    "Собирать модульные тесты ядра"                    ON)
option(IDE_USE_CXX23      "Использовать стандарт C++23 (если поддерживается)" OFF)
option(IDE_WARNINGS_AS_ERRORS "Считать предупреждения ошибками"               OFF)
option(IDE_FETCH_DEPS     "Скачивать отсутствующие зависимости через FetchContent" ON)

if(IDE_USE_CXX23)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

# ---- Общие флаги компилятора -----------------------------------------------
add_library(ide_options INTERFACE)
target_compile_features(ide_options INTERFACE cxx_std_20)
if(MSVC)
    # /Zc:char8_t- : path::u8string() возвращает std::string (UTF-8) — единый тип строк во всём проекте
    target_compile_options(ide_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:char8_t- /EHsc /MP)
    target_compile_definitions(ide_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(IDE_WARNINGS_AS_ERRORS)
        target_compile_options(ide_options INTERFACE /WX)
    endif()
else()
    # -fno-char8_t: path::u8string() возвращает std::string (UTF-8)
    target_compile_options(ide_options INTERFACE -Wall -Wextra -Wpedantic -Wno-unused-parameter -fno-char8_t)
    # GCC < 12 требует явного флага для корутин C++20
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
        target_compile_options(ide_options INTERFACE -fcoroutines)
    endif()
    if(IDE_WARNINGS_AS_ERRORS)
        target_compile_options(ide_options INTERFACE -Werror)
    endif()
endif()

find_package(Threads REQUIRED)

# ---- Ядро IDE (не зависит от оконной системы) ------------------------------
# Всё, кроме бэкендов GLFW/OpenGL, собирается в статическую библиотеку —
# это позволяет тестировать логику без графического окружения (CI, headless).
add_library(ide_core STATIC
    src/core/Process.cpp
    src/core/TextBuffer.cpp
    src/core/SyntaxHighlighter.cpp
    src/core/EditorCore.cpp
    src/core/TerminalEngine.cpp
    src/core/GitManager.cpp
    src/core/Workspace.cpp
    src/core/BuildSystem.cpp
    src/core/FileExplorer.cpp
    src/lsp/JsonRpc.cpp
    src/lsp/LSPClient.cpp
    src/lsp/DAPClient.cpp
    src/github/HttpClient.cpp
    src/github/GitHubManager.cpp
    src/github/ToolchainManager.cpp
    src/templates/ProjectTemplates.cpp
    src/audio/AudioEngine.cpp
    src/graphics/GLLoader.cpp
    src/graphics/ShaderEngine.cpp
    src/graphics/WaveSimulation.cpp
    src/graphics/FishSimulation.cpp
    src/themes/ThemeManager.cpp
    src/ui/Localization.cpp
    src/ui/KeybindingManager.cpp
    src/ui/TelemetryPanel.cpp
    src/ui/Panels.cpp
    src/ui/FileDialog.cpp
    src/platform/SystemInfo.cpp
)
target_include_directories(ide_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ide_core
    PUBLIC  ide_options imgui_core implot nlohmann_json::nlohmann_json Threads::Threads miniaudio_hdr stb_hdr
    PRIVATE CURL::libcurl)
target_compile_definitions(ide_core PUBLIC
    IDE_VERSION="${PROJECT_VERSION}"
    IDE_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")

if(WIN32)
    target_link_libraries(ide_core PUBLIC pdh psapi)          # телеметрия CPU/RAM
elseif(UNIX AND NOT APPLE)
    target_link_libraries(ide_core PUBLIC ${CMAKE_DL_LIBS} m) # miniaudio: dlopen backend'ов
    find_library(UTIL_LIB util)                                # forkpty / posix_openpt
    if(UTIL_LIB)
        target_link_libraries(ide_core PUBLIC ${UTIL_LIB})
    endif()
elseif(APPLE)
    target_link_libraries(ide_core PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
endif()

# ---- Графическое приложение -------------------------------------------------
if(IDE_BUILD_APP)
    find_package(OpenGL REQUIRED)
    set(IDE_PLATFORM_SOURCES)
    if(WIN32)
        # Версия файла, иконка-манифест: PerMonitorV2 DPI, UTF-8 code page, Win10/11
        configure_file(packaging/windows/HybridIDE.rc.in ${CMAKE_CURRENT_BINARY_DIR}/HybridIDE.rc @ONLY)
        set(IDE_PLATFORM_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/HybridIDE.rc)
    endif()
    add_executable(HybridIDE
        src/main.cpp
        src/app/Application.cpp
        ${IMGUI_BACKEND_SOURCES}
        ${IDE_PLATFORM_SOURCES})
    target_include_directories(HybridIDE PRIVATE ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(HybridIDE PRIVATE ide_core glfw OpenGL::GL)
    if(WIN32)
        set_target_properties(HybridIDE PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
        # Без консоли в Release, но с обычной точкой входа main()
        if(MSVC)
            target_link_options(HybridIDE PRIVATE $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
        endif()
    endif()
    # Ресурсы (шейдеры, раскладки, словари) копируются рядом с бинарником
    add_custom_command(TARGET HybridIDE POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/resources $<TARGET_FILE_DIR:HybridIDE>/resources)
    install(TARGETS HybridIDE RUNTIME DESTINATION bin)
    install(DIRECTORY resources DESTINATION share/HybridIDE)
endif()

# ---- Тесты ------------------------------------------------------------------
if(IDE_BUILD_TESTS)
    enable_testing()
    add_executable(ide_tests tests/test_core.cpp)
    target_link_libraries(ide_tests PRIVATE ide_core)
    add_test(NAME ide_core_tests COMMAND ide_tests)
endif()
