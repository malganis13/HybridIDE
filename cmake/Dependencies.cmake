# =============================================================================
#  Управление зависимостями: сначала ищем системные пакеты (vcpkg/apt/brew),
#  при их отсутствии — скачиваем через FetchContent с закреплёнными версиями.
# =============================================================================
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- nlohmann/json ----------------------------------------------------------
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---- libcurl (HTTPS к GitHub REST API) -------------------------------------
find_package(CURL QUIET)
if(NOT CURL_FOUND)
    message(STATUS "libcurl не найден в системе — собираем из исходников")
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)    # нативный TLS Windows
    endif()
    FetchContent_Declare(curl
        URL https://github.com/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(curl)
endif()

# ---- GLFW -------------------------------------------------------------------
if(IDE_BUILD_APP)
    find_package(glfw3 3.3 QUIET)
    if(NOT glfw3_FOUND)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(glfw)
    endif()
endif()

# ---- Dear ImGui (ветка docking: докинг + multi-viewport) -------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b-docking
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui_core STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui_core PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_features(imgui_core PUBLIC cxx_std_17)

set(IMGUI_BACKEND_SOURCES
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)

# ---- ImPlot (графики телеметрии) -------------------------------------------
FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    # Зафиксированный коммит ветки master (API ImPlotSpec v1.0, совместим с imgui 1.91.9)
    GIT_TAG 09e2ba71766e25d88053a2173936c9d1043bae42)
FetchContent_MakeAvailable(implot)
add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp)
target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui_core)

# ---- miniaudio (single-header аудиодвижок) ---------------------------------
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake_)  # только заголовок, без add_subdirectory
FetchContent_MakeAvailable(miniaudio)
add_library(miniaudio_hdr INTERFACE)
target_include_directories(miniaudio_hdr INTERFACE ${miniaudio_SOURCE_DIR})

# ---- stb_image (загрузка фоновых изображений тем) ---------------------------
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(stb)
add_library(stb_hdr INTERFACE)
target_include_directories(stb_hdr INTERFACE ${stb_SOURCE_DIR})
