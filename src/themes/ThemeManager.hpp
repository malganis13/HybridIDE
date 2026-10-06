#pragma once
// ============================================================================
//  ThemeManager — четыре «живые» темы оформления и их подпанели настроек.
//
//  Тема — это не только цвета ImGui. Каждая тема управляет:
//    1. стилем ImGui и палитрой подсветки редактора (EditorPalette);
//    2. фоновым шейдером и пост-эффектом (заполняет ShaderUniforms);
//    3. звуком: эффекты на нажатия/клики/сборку + фоновый эмбиент;
//    4. собственными интерактивными механиками:
//         Aquarium  — рябь (WaveSimulation) и косяк рыб (FishSimulation),
//                     которые пугаются кликов по редактору;
//         Steampunk — шестерни, скорость которых зависит от загрузки CPU,
//                     манометры CPU/RAM, пар при запуске сборки, сепия;
//         Hacker    — «матрица» на фоне, CRT-кривизна, строки развёртки,
//                     фосфорное свечение, клики реле;
//         Cyberpunk — неон, хроматическая аберрация и глитч-импульс при
//                     смене активной панели, синтезаторные звуки.
//
//  Все параметры тем хранятся в JSON (UserSettings::themes[<имя>]),
//  подпанель renderSettings() редактирует их «вживую».
// ============================================================================
#include "core/EditorCore.hpp"
#include "graphics/ShaderEngine.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace ide {

class AudioEngine;
class FishSimulation;
class WaveSimulation;

enum class ThemeId { Aquarium, Steampunk, Hacker, Cyberpunk, Count };

// Общие для всех тем параметры.
struct CommonThemeSettings {
    bool        audioEnabled    = true;
    float       effectsVolume   = 0.7f;
    float       ambienceVolume  = 0.3f;
    bool        typingSounds    = true;
    bool        animations      = true;      // фон/частицы; выключение экономит GPU
    float       editorOpacity   = 0.82f;     // прозрачность фона редактора
    float       intensity       = 1.0f;      // яркость фонового эффекта
    std::string backgroundMedia;             // путь к фоновому изображению (до 4K)
};

struct AquariumSettings {
    int   fishCount = 48;
    float fishSpeed = 1.f, scareStrength = 1.f, fishFog = 0.25f;
    float rays = 1.f, waveDamping = 0.985f, rippleStrength = 1.f;
    std::array<float, 3> deep{0.0f, 0.06f, 0.16f}, shallow{0.0f, 0.35f, 0.55f};
};
struct SteampunkSettings {
    float gearSpeed = 1.f, sepia = 0.3f;
    bool  showGauges = true, steamOnBuild = true, bellOnEnter = true;
};
struct HackerSettings {
    std::array<float, 3> phosphor{0.2f, 1.0f, 0.35f};
    float curvature = 0.08f, scanlines = 0.35f, glow = 0.6f, mono = 0.85f, rainSpeed = 1.f;
};
struct CyberpunkSettings {
    std::array<float, 3> neonA{1.0f, 0.1f, 0.75f}, neonB{0.0f, 0.95f, 1.0f};
    float aberration = 1.5f, glitchStrength = 0.8f, glow = 0.6f;
    bool  glitchOnFocus = true;
};

// Данные кадра, нужные теме (телеметрия влияет на шестерни/манометры).
struct ThemeFrameInput {
    float dt = 0.016f;
    float time = 0.f;
    float cpuPercent = 0.f, ramPercent = 0.f;
    ImVec2 viewportPos{0, 0}, viewportSize{1280, 720};
};

class ThemeManager {
public:
    ThemeManager() = default;
    void attach(AudioEngine* audio, FishSimulation* fish, WaveSimulation* wave) { audio_ = audio; fish_ = fish; wave_ = wave; }

    static const std::vector<std::string>& names();
    static ThemeId idFromName(const std::string& n);
    static const char* nameOf(ThemeId id);

    void apply(const std::string& name);        // переключить тему (стиль, эмбиент, рыбы)
    [[nodiscard]] ThemeId current() const { return current_; }
    [[nodiscard]] const char* currentName() const { return nameOf(current_); }
    void next();

    // Кадр: анимации, затухание глитча, симуляции.
    void update(const ThemeFrameInput& in);
    void fillUniforms(ShaderUniforms& u) const;
    [[nodiscard]] const EditorPalette& editorPalette() const { return palette_; }
    [[nodiscard]] bool wantsFish() const { return current_ == ThemeId::Aquarium && common().animations; }

    // События от приложения.
    void onKeystroke(unsigned int ch);
    void onEditorClick(ImVec2 screenPos);
    void onFocusChanged();
    void onBuildStarted();
    void onBuildFinished(bool ok);
    void onError();

    // UI
    void renderSettings();                       // подпанель «Оформление»
    void renderStatusBarDecor(float height);     // манометры/индикаторы темы в статус-строке
    void renderGaugesWindow(bool* open, float cpu, float ram);

    // Персистентность: json-объект { "<Theme>": {...}, ... }
    [[nodiscard]] nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);

    // Применение фонового изображения (реализует Application через ShaderEngine).
    std::function<bool(const std::string& path, std::string* err)> applyBackgroundMedia;
    std::function<void()> onSettingsChanged;    // сохранить настройки

    CommonThemeSettings&       common()       { return common_[(int)current_]; }
    const CommonThemeSettings& common() const { return common_[(int)current_]; }
    AquariumSettings  aquarium;
    SteampunkSettings steampunk;
    HackerSettings    hacker;
    CyberpunkSettings cyberpunk;

private:
    void applyStyle();
    void buildPalette();
    void syncAudio();
    void syncSimulations();
    void drawGauge(ImDrawList* dl, ImVec2 c, float r, float value, const char* label) const;
    void settingsCommon();
    void settingsAquarium();
    void settingsSteampunk();
    void settingsHacker();
    void settingsCyberpunk();
    void changed();

    ThemeId         current_ = ThemeId::Aquarium;
    std::array<CommonThemeSettings, (int)ThemeId::Count> common_{};
    EditorPalette   palette_;
    AudioEngine*    audio_ = nullptr;
    FishSimulation* fish_ = nullptr;
    WaveSimulation* wave_ = nullptr;

    // Анимационное состояние
    std::array<float, 4> gearAngles_{};
    float time_ = 0.f, glitch_ = 0.f, steam_ = 0.f, cpu_ = 0.f, ram_ = 0.f;
    float cpuNeedle_ = 0.f, ramNeedle_ = 0.f;   // сглаженные стрелки манометров
    ImVec2 vpPos_{0, 0}, vpSize_{1280, 720};
    int    lastFishCount_ = -1;
    std::string mediaError_;
    std::string mediaInput_;
};

} // namespace ide
