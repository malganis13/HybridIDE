// ============================================================================
//  ThemeManager.cpp — стили, палитры, звуки и механики четырёх тем.
// ============================================================================
#include "themes/ThemeManager.hpp"

#include "audio/AudioEngine.hpp"
#include "graphics/FishSimulation.hpp"
#include "graphics/WaveSimulation.hpp"
#include "ui/Localization.hpp"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ide {

namespace {
constexpr float kPi = 3.14159265358979f;

ImVec4 rgb(int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
ImU32 u32(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImU32 u32f(const std::array<float, 3>& c, float a = 1.f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], a));
}

// --- JSON помощники: читаем поле, если оно есть и нужного типа --------------
template <class T> void rd(const nlohmann::json& j, const char* k, T& v) {
    if (auto it = j.find(k); it != j.end()) {
        try { v = it->get<T>(); } catch (...) {}
    }
}
} // namespace

const std::vector<std::string>& ThemeManager::names() {
    static const std::vector<std::string> n{"Aquarium", "Steampunk", "Hacker", "Cyberpunk"};
    return n;
}
ThemeId ThemeManager::idFromName(const std::string& n) {
    const auto& v = names();
    auto it = std::find(v.begin(), v.end(), n);
    return it == v.end() ? ThemeId::Aquarium : ThemeId(it - v.begin());
}
const char* ThemeManager::nameOf(ThemeId id) { return names()[std::size_t(id)].c_str(); }

void ThemeManager::apply(const std::string& name) {
    current_ = idFromName(name);
    applyStyle();
    buildPalette();
    syncAudio();
    syncSimulations();
    glitch_ = current_ == ThemeId::Cyberpunk ? 1.f : 0.f;   // «включение» неона
    if (applyBackgroundMedia) {
        std::string err;
        applyBackgroundMedia(common().backgroundMedia, &err);   // пустой путь = снять фон
        mediaError_ = err;
    }
    mediaInput_ = common().backgroundMedia;
}

void ThemeManager::next() {
    apply(nameOf(ThemeId(((int)current_ + 1) % (int)ThemeId::Count)));
    changed();
}

void ThemeManager::changed() { if (onSettingsChanged) onSettingsChanged(); }

// ---------------------------------------------------------------------------
//  Стили ImGui
// ---------------------------------------------------------------------------
void ThemeManager::applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    ImVec4* c = s.Colors;
    s.WindowRounding = 4; s.FrameRounding = 3; s.TabRounding = 3; s.ScrollbarRounding = 6;
    s.GrabRounding = 3; s.WindowBorderSize = 1; s.FrameBorderSize = 0; s.PopupRounding = 4;
    const float a = common().animations ? common().editorOpacity : 1.f;   // прозрачность окон

    switch (current_) {
    case ThemeId::Aquarium:
        c[ImGuiCol_WindowBg]        = rgb(6, 26, 44, a);
        c[ImGuiCol_ChildBg]         = rgb(6, 26, 44, 0.f);
        c[ImGuiCol_PopupBg]         = rgb(8, 34, 56, 0.97f);
        c[ImGuiCol_TitleBg]         = rgb(4, 40, 64);
        c[ImGuiCol_TitleBgActive]   = rgb(0, 92, 130);
        c[ImGuiCol_MenuBarBg]       = rgb(3, 30, 50, 0.95f);
        c[ImGuiCol_Header]          = rgb(0, 120, 150, 0.45f);
        c[ImGuiCol_HeaderHovered]   = rgb(0, 160, 190, 0.6f);
        c[ImGuiCol_HeaderActive]    = rgb(0, 180, 210, 0.8f);
        c[ImGuiCol_Button]          = rgb(0, 110, 140, 0.55f);
        c[ImGuiCol_ButtonHovered]   = rgb(0, 150, 180, 0.8f);
        c[ImGuiCol_ButtonActive]    = rgb(0, 190, 220);
        c[ImGuiCol_FrameBg]         = rgb(0, 50, 80, 0.6f);
        c[ImGuiCol_FrameBgHovered]  = rgb(0, 80, 110, 0.7f);
        c[ImGuiCol_Tab]             = rgb(0, 60, 90, 0.8f);
        c[ImGuiCol_TabHovered]      = rgb(0, 150, 180);
        c[ImGuiCol_TabSelected]     = rgb(0, 110, 145);
        c[ImGuiCol_CheckMark]       = rgb(120, 240, 255);
        c[ImGuiCol_SliderGrab]      = rgb(80, 210, 240);
        c[ImGuiCol_Text]            = rgb(214, 240, 250);
        c[ImGuiCol_Border]          = rgb(40, 140, 170, 0.35f);
        c[ImGuiCol_DockingPreview]  = rgb(80, 210, 240, 0.5f);
        s.WindowRounding = 8; s.FrameRounding = 6; s.TabRounding = 6;
        break;
    case ThemeId::Steampunk:
        c[ImGuiCol_WindowBg]        = rgb(40, 28, 18, a);
        c[ImGuiCol_ChildBg]         = rgb(40, 28, 18, 0.f);
        c[ImGuiCol_PopupBg]         = rgb(52, 36, 22, 0.97f);
        c[ImGuiCol_TitleBg]         = rgb(70, 46, 24);
        c[ImGuiCol_TitleBgActive]   = rgb(128, 84, 34);
        c[ImGuiCol_MenuBarBg]       = rgb(58, 38, 20, 0.97f);
        c[ImGuiCol_Header]          = rgb(160, 110, 50, 0.45f);
        c[ImGuiCol_HeaderHovered]   = rgb(190, 135, 60, 0.6f);
        c[ImGuiCol_HeaderActive]    = rgb(210, 150, 70, 0.8f);
        c[ImGuiCol_Button]          = rgb(140, 92, 40, 0.75f);
        c[ImGuiCol_ButtonHovered]   = rgb(181, 126, 56);
        c[ImGuiCol_ButtonActive]    = rgb(214, 160, 80);
        c[ImGuiCol_FrameBg]         = rgb(80, 54, 30, 0.8f);
        c[ImGuiCol_FrameBgHovered]  = rgb(110, 76, 40, 0.8f);
        c[ImGuiCol_Tab]             = rgb(90, 60, 30);
        c[ImGuiCol_TabHovered]      = rgb(181, 126, 56);
        c[ImGuiCol_TabSelected]     = rgb(150, 100, 45);
        c[ImGuiCol_CheckMark]       = rgb(240, 200, 120);
        c[ImGuiCol_SliderGrab]      = rgb(205, 160, 90);
        c[ImGuiCol_Text]            = rgb(240, 222, 190);
        c[ImGuiCol_Border]          = rgb(181, 136, 66, 0.7f);
        c[ImGuiCol_DockingPreview]  = rgb(214, 160, 80, 0.5f);
        s.FrameBorderSize = 1; s.WindowRounding = 2; s.FrameRounding = 2;
        break;
    case ThemeId::Hacker:
        c[ImGuiCol_WindowBg]        = rgb(0, 8, 2, a);
        c[ImGuiCol_ChildBg]         = rgb(0, 8, 2, 0.f);
        c[ImGuiCol_PopupBg]         = rgb(0, 14, 4, 0.97f);
        c[ImGuiCol_TitleBg]         = rgb(0, 20, 6);
        c[ImGuiCol_TitleBgActive]   = rgb(0, 60, 18);
        c[ImGuiCol_MenuBarBg]       = rgb(0, 14, 4, 0.97f);
        c[ImGuiCol_Header]          = rgb(0, 120, 30, 0.4f);
        c[ImGuiCol_HeaderHovered]   = rgb(0, 170, 45, 0.5f);
        c[ImGuiCol_HeaderActive]    = rgb(0, 210, 60, 0.7f);
        c[ImGuiCol_Button]          = rgb(0, 70, 20, 0.8f);
        c[ImGuiCol_ButtonHovered]   = rgb(0, 130, 35);
        c[ImGuiCol_ButtonActive]    = rgb(0, 190, 50);
        c[ImGuiCol_FrameBg]         = rgb(0, 30, 8, 0.8f);
        c[ImGuiCol_FrameBgHovered]  = rgb(0, 55, 14, 0.8f);
        c[ImGuiCol_Tab]             = rgb(0, 30, 8);
        c[ImGuiCol_TabHovered]      = rgb(0, 120, 30);
        c[ImGuiCol_TabSelected]     = rgb(0, 80, 22);
        c[ImGuiCol_CheckMark]       = rgb(60, 255, 100);
        c[ImGuiCol_SliderGrab]      = rgb(60, 255, 100);
        c[ImGuiCol_Text]            = rgb(80, 255, 120);
        c[ImGuiCol_TextDisabled]    = rgb(30, 130, 50);
        c[ImGuiCol_Border]          = rgb(0, 160, 40, 0.5f);
        c[ImGuiCol_DockingPreview]  = rgb(60, 255, 100, 0.4f);
        s.WindowRounding = 0; s.FrameRounding = 0; s.TabRounding = 0; s.ScrollbarRounding = 0;
        break;
    case ThemeId::Cyberpunk:
        c[ImGuiCol_WindowBg]        = rgb(14, 6, 26, a);
        c[ImGuiCol_ChildBg]         = rgb(14, 6, 26, 0.f);
        c[ImGuiCol_PopupBg]         = rgb(22, 8, 38, 0.97f);
        c[ImGuiCol_TitleBg]         = rgb(30, 8, 50);
        c[ImGuiCol_TitleBgActive]   = rgb(120, 0, 110);
        c[ImGuiCol_MenuBarBg]       = rgb(20, 6, 34, 0.97f);
        c[ImGuiCol_Header]          = rgb(255, 0, 170, 0.35f);
        c[ImGuiCol_HeaderHovered]   = rgb(0, 240, 255, 0.4f);
        c[ImGuiCol_HeaderActive]    = rgb(0, 240, 255, 0.6f);
        c[ImGuiCol_Button]          = rgb(140, 0, 120, 0.7f);
        c[ImGuiCol_ButtonHovered]   = rgb(0, 190, 220, 0.85f);
        c[ImGuiCol_ButtonActive]    = rgb(255, 230, 0);
        c[ImGuiCol_FrameBg]         = rgb(40, 10, 70, 0.8f);
        c[ImGuiCol_FrameBgHovered]  = rgb(70, 16, 110, 0.8f);
        c[ImGuiCol_Tab]             = rgb(50, 10, 80);
        c[ImGuiCol_TabHovered]      = rgb(0, 200, 230);
        c[ImGuiCol_TabSelected]     = rgb(170, 0, 150);
        c[ImGuiCol_CheckMark]       = rgb(255, 230, 0);
        c[ImGuiCol_SliderGrab]      = rgb(0, 240, 255);
        c[ImGuiCol_Text]            = rgb(240, 230, 255);
        c[ImGuiCol_Border]          = rgb(0, 240, 255, 0.45f);
        c[ImGuiCol_Separator]       = rgb(255, 0, 170, 0.6f);
        c[ImGuiCol_DockingPreview]  = rgb(255, 0, 170, 0.5f);
        s.FrameBorderSize = 1; s.WindowRounding = 0; s.FrameRounding = 0; s.TabRounding = 0;
        break;
    default: break;
    }
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
    c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_ResizeGrip] = c[ImGuiCol_Button];
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_TextSelectedBg] = ImVec4(c[ImGuiCol_HeaderActive].x, c[ImGuiCol_HeaderActive].y, c[ImGuiCol_HeaderActive].z, 0.45f);
    // Окна-платформы (multi-viewport) должны быть непрозрачными: за ними нет нашего фона.
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) s.Colors[ImGuiCol_DockingEmptyBg].w = 0.f;
}

// ---------------------------------------------------------------------------
//  Палитры подсветки синтаксиса
// ---------------------------------------------------------------------------
void ThemeManager::buildPalette() {
    EditorPalette p;
    auto& t = p.tokens;
    auto set = [&](TokenKind k, ImU32 c) { t[(std::size_t)k] = c; };
    switch (current_) {
    case ThemeId::Aquarium:
        set(TokenKind::Default, u32(214, 240, 250)); set(TokenKind::Keyword, u32(86, 200, 255));
        set(TokenKind::Type, u32(78, 230, 200)); set(TokenKind::Identifier, u32(200, 230, 245));
        set(TokenKind::Function, u32(255, 210, 120)); set(TokenKind::Number, u32(255, 160, 140));
        set(TokenKind::String, u32(170, 240, 150)); set(TokenKind::Char, u32(170, 240, 150));
        set(TokenKind::Comment, u32(90, 150, 170)); set(TokenKind::Preprocessor, u32(210, 150, 255));
        set(TokenKind::Operator, u32(150, 220, 240)); set(TokenKind::Punctuation, u32(140, 190, 210));
        set(TokenKind::Attribute, u32(255, 190, 220));
        p.background = u32(4, 22, 38); p.gutter = u32(3, 28, 46); p.selection = u32(0, 130, 170, 140);
        p.cursor = u32(130, 240, 255); p.currentLine = u32(120, 220, 255, 18); p.ghost = u32(150, 210, 230, 110);
        p.ghostChip = u32(0, 150, 190, 220); p.bracket = u32(120, 240, 255, 80);
        break;
    case ThemeId::Steampunk:
        set(TokenKind::Default, u32(240, 222, 190)); set(TokenKind::Keyword, u32(230, 160, 70));
        set(TokenKind::Type, u32(214, 190, 110)); set(TokenKind::Identifier, u32(232, 214, 182));
        set(TokenKind::Function, u32(250, 200, 120)); set(TokenKind::Number, u32(200, 120, 80));
        set(TokenKind::String, u32(170, 190, 110)); set(TokenKind::Char, u32(170, 190, 110));
        set(TokenKind::Comment, u32(140, 116, 86)); set(TokenKind::Preprocessor, u32(190, 120, 90));
        set(TokenKind::Operator, u32(220, 190, 140)); set(TokenKind::Punctuation, u32(190, 160, 120));
        set(TokenKind::Attribute, u32(210, 150, 110));
        p.background = u32(34, 24, 15); p.gutter = u32(46, 32, 19); p.selection = u32(160, 110, 50, 140);
        p.cursor = u32(255, 210, 130); p.currentLine = u32(255, 200, 120, 16); p.ghost = u32(200, 170, 130, 110);
        p.ghostChip = u32(150, 100, 45, 220); p.bracket = u32(255, 200, 120, 70);
        p.lineNumber = u32(150, 120, 80); p.lineNumberActive = u32(240, 200, 130);
        break;
    case ThemeId::Hacker: {
        auto ph = hacker.phosphor;
        auto shade = [&](float k, int a = 255) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(1.f, ph[0] * k), std::min(1.f, ph[1] * k), std::min(1.f, ph[2] * k), a / 255.f));
        };
        set(TokenKind::Default, shade(0.85f)); set(TokenKind::Keyword, shade(1.25f));
        set(TokenKind::Type, shade(1.1f)); set(TokenKind::Identifier, shade(0.8f));
        set(TokenKind::Function, shade(1.2f)); set(TokenKind::Number, shade(1.0f));
        set(TokenKind::String, shade(0.95f)); set(TokenKind::Char, shade(0.95f));
        set(TokenKind::Comment, shade(0.45f)); set(TokenKind::Preprocessor, shade(0.7f));
        set(TokenKind::Operator, shade(0.9f)); set(TokenKind::Punctuation, shade(0.6f));
        set(TokenKind::Attribute, shade(0.75f));
        p.background = u32(0, 6, 1); p.gutter = u32(0, 10, 3); p.selection = shade(0.4f, 120);
        p.cursor = shade(1.3f); p.currentLine = shade(1.f, 14); p.ghost = shade(0.6f, 120);
        p.ghostChip = shade(0.5f, 200); p.bracket = shade(1.f, 60);
        p.lineNumber = shade(0.35f); p.lineNumberActive = shade(1.0f);
        break;
    }
    case ThemeId::Cyberpunk:
        set(TokenKind::Default, u32(240, 230, 255)); set(TokenKind::Keyword, u32f(cyberpunk.neonA));
        set(TokenKind::Type, u32f(cyberpunk.neonB)); set(TokenKind::Identifier, u32(225, 215, 245));
        set(TokenKind::Function, u32(255, 230, 0)); set(TokenKind::Number, u32(255, 120, 80));
        set(TokenKind::String, u32(120, 255, 170)); set(TokenKind::Char, u32(120, 255, 170));
        set(TokenKind::Comment, u32(120, 100, 160)); set(TokenKind::Preprocessor, u32(255, 90, 200));
        set(TokenKind::Operator, u32(0, 220, 255)); set(TokenKind::Punctuation, u32(170, 150, 210));
        set(TokenKind::Attribute, u32(255, 160, 255));
        p.background = u32(12, 4, 22); p.gutter = u32(18, 6, 32); p.selection = u32(255, 0, 170, 90);
        p.cursor = u32(255, 230, 0); p.currentLine = u32(0, 240, 255, 16); p.ghost = u32(0, 220, 255, 110);
        p.ghostChip = u32(170, 0, 150, 220); p.bracket = u32(255, 230, 0, 70);
        break;
    default: break;
    }
    p.backgroundAlpha = common().animations ? common().editorOpacity : 1.f;
    palette_ = p;
}

// ---------------------------------------------------------------------------
//  Аудио и симуляции
// ---------------------------------------------------------------------------
void ThemeManager::syncAudio() {
    if (!audio_) return;
    const auto& c = common();
    audio_->enabled = c.audioEnabled;
    audio_->effectsVolume = c.effectsVolume;
    audio_->ambienceVolume = c.ambienceVolume;
    Ambience amb = Ambience::None;
    switch (current_) {
    case ThemeId::Aquarium:  amb = Ambience::Ocean; break;
    case ThemeId::Steampunk: amb = Ambience::Workshop; break;
    case ThemeId::Hacker:    amb = Ambience::CrtHum; break;
    case ThemeId::Cyberpunk: amb = Ambience::NeonHum; break;
    default: break;
    }
    audio_->setAmbience(c.audioEnabled ? amb : Ambience::None);
}

void ThemeManager::syncSimulations() {
    if (fish_) {
        fish_->params.maxSpeed = 70.f * aquarium.fishSpeed;
        fish_->params.panicSpeed = 260.f * std::max(0.5f, aquarium.fishSpeed);
        fish_->params.scareStrength = aquarium.scareStrength;
        if (aquarium.fishCount != lastFishCount_) {
            fish_->reset(aquarium.fishCount, vpSize_.x, vpSize_.y);
            lastFishCount_ = aquarium.fishCount;
        }
    }
    if (wave_) wave_->damping = aquarium.waveDamping;
}

void ThemeManager::update(const ThemeFrameInput& in) {
    time_ = in.time;
    cpu_ = in.cpuPercent; ram_ = in.ramPercent;
    if (in.viewportSize.x != vpSize_.x || in.viewportSize.y != vpSize_.y) {
        vpSize_ = in.viewportSize;
        if (fish_) fish_->resize(vpSize_.x, vpSize_.y);
    }
    vpPos_ = in.viewportPos;
    if (lastFishCount_ < 0) syncSimulations();

    // Шестерни: базовая скорость + разгон пропорционально загрузке CPU.
    const float speed = steampunk.gearSpeed * (0.15f + 2.5f * cpu_ / 100.f);
    const float ratios[4] = {1.f, -1.6f, 0.75f, -2.2f};       // передаточные отношения
    for (int i = 0; i < 4; ++i) gearAngles_[i] = std::fmod(gearAngles_[i] + in.dt * speed * ratios[i], 2 * kPi);

    // Сглаживание стрелок манометров (инерция ~0.3 с).
    const float k = 1.f - std::exp(-in.dt / 0.3f);
    cpuNeedle_ += (cpu_ - cpuNeedle_) * k;
    ramNeedle_ += (ram_ - ramNeedle_) * k;

    glitch_ = std::max(0.f, glitch_ - in.dt * 2.5f);
    steam_ = std::max(0.f, steam_ - in.dt * 0.6f);

    if (common().animations) {
        if (current_ == ThemeId::Aquarium) {
            if (fish_) fish_->update(in.dt);
            if (wave_) wave_->step(in.dt);
        }
    }
}

void ThemeManager::fillUniforms(ShaderUniforms& u) const {
    const auto& c = common();
    u.time = time_;
    u.intensity = c.intensity;
    u.glitch = 0.f;
    u.post = PostKind::None;
    if (!c.animations) { u.background = BackgroundKind::None; }
    switch (current_) {
    case ThemeId::Aquarium:
        if (c.animations) u.background = BackgroundKind::Aquarium;
        u.rays = aquarium.rays; u.deepColor = aquarium.deep; u.shallowColor = aquarium.shallow;
        u.fishFog = aquarium.fishFog; u.clear = {0.0f, 0.05f, 0.1f};
        break;
    case ThemeId::Steampunk:
        if (c.animations) u.background = BackgroundKind::Steampunk;
        u.gearAngles = gearAngles_;
        u.post = PostKind::Sepia;
        u.sepia = std::min(1.f, steampunk.sepia + steam_ * 0.3f);
        u.clear = {0.13f, 0.09f, 0.05f};
        break;
    case ThemeId::Hacker:
        if (c.animations) u.background = BackgroundKind::Matrix;
        u.post = PostKind::CRT;
        u.tint = hacker.phosphor; u.curvature = hacker.curvature; u.scanlines = hacker.scanlines;
        u.glow = hacker.glow; u.mono = hacker.mono;
        u.intensity = c.intensity * hacker.rainSpeed;
        u.clear = {0.f, 0.02f, 0.f};
        break;
    case ThemeId::Cyberpunk:
        if (c.animations) u.background = BackgroundKind::Cyberpunk;
        u.post = PostKind::Cyber;
        u.colorA = cyberpunk.neonA; u.colorB = cyberpunk.neonB;
        u.aberration = cyberpunk.aberration; u.glow = cyberpunk.glow;
        u.glitch = glitch_ * cyberpunk.glitchStrength;
        u.clear = {0.05f, 0.02f, 0.09f};
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
//  События
// ---------------------------------------------------------------------------
void ThemeManager::onKeystroke(unsigned int ch) {
    if (!audio_ || !common().typingSounds) return;
    PlayParams p;
    // Небольшая случайность высоты тона, чтобы звук не «пулемётил».
    p.pitch = 0.94f + 0.12f * float((ch * 2654435761u) % 1000u) / 1000.f;
    p.gain = 0.5f;
    switch (current_) {
    case ThemeId::Aquarium:  p.gain = 0.25f; audio_->play(Sfx::Bubble, p); break;
    case ThemeId::Steampunk:
        if ((ch == '\n' || ch == '\r') && steampunk.bellOnEnter) audio_->play(Sfx::TypewriterBell, {0.6f});
        else audio_->play(Sfx::Typewriter, p);
        break;
    case ThemeId::Hacker:    audio_->play(Sfx::ReedSwitch, p); break;
    case ThemeId::Cyberpunk: p.gain = 0.35f; audio_->play(Sfx::SynthBlip, p); break;
    default: break;
    }
}

void ThemeManager::onEditorClick(ImVec2 sp) {
    const float lx = sp.x - vpPos_.x, ly = sp.y - vpPos_.y;
    if (current_ == ThemeId::Aquarium) {
        // Рябь на воде (UV, ось V снизу вверх, как в шейдере) и испуг рыб.
        if (wave_ && vpSize_.x > 0 && vpSize_.y > 0)
            wave_->disturb(lx / vpSize_.x, 1.f - ly / vpSize_.y, aquarium.rippleStrength, 0.025f);
        if (fish_) fish_->scare(Vec2{lx, ly}, aquarium.scareStrength);
        if (audio_) audio_->playAt(Sfx::Splash, lx, ly, vpSize_.x, vpSize_.y, 0.4f, 0.6f);
    } else if (audio_) {
        if (current_ == ThemeId::Cyberpunk) audio_->playAt(Sfx::SynthBlip, lx, ly, vpSize_.x, vpSize_.y, 0.1f, 0.3f);
        else audio_->playAt(Sfx::Click, lx, ly, vpSize_.x, vpSize_.y, 0.1f, 0.35f);
    }
}

void ThemeManager::onFocusChanged() {
    if (current_ == ThemeId::Cyberpunk && cyberpunk.glitchOnFocus) {
        glitch_ = 1.f;
        if (audio_) audio_->play(Sfx::SynthChord, {0.25f, 0.f, 1.5f, 0.2f});
    }
}

void ThemeManager::onBuildStarted() {
    if (current_ == ThemeId::Steampunk && steampunk.steamOnBuild) {
        steam_ = 1.f;
        if (audio_) audio_->play(Sfx::SteamValve, {0.7f, 0.f, 1.f, 0.3f});
    }
}

void ThemeManager::onBuildFinished(bool ok) {
    if (!audio_) return;
    if (!ok) { audio_->play(Sfx::Error, {0.6f}); return; }
    switch (current_) {
    case ThemeId::Steampunk: audio_->play(Sfx::TypewriterBell, {0.7f}); break;
    case ThemeId::Cyberpunk: audio_->play(Sfx::SynthChord, {0.5f}); break;
    case ThemeId::Aquarium:  audio_->play(Sfx::Bubble, {0.6f, 0.f, 0.8f, 0.5f}); break;
    default:                 audio_->play(Sfx::Click, {0.6f}); break;
    }
}

void ThemeManager::onError() { if (audio_) audio_->play(Sfx::Error, {0.5f}); }

// ---------------------------------------------------------------------------
//  Манометры (Steampunk)
// ---------------------------------------------------------------------------
void ThemeManager::drawGauge(ImDrawList* dl, ImVec2 c, float r, float value, const char* label) const {
    const ImU32 brass = u32(181, 136, 66), brassDark = u32(110, 76, 34), face = u32(236, 222, 190);
    dl->AddCircleFilled(c, r, brassDark, 48);
    dl->AddCircleFilled(c, r * 0.9f, brass, 48);
    dl->AddCircleFilled(c, r * 0.8f, face, 48);
    // Шкала 0..100 на дуге 240°
    const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
    for (int i = 0; i <= 10; ++i) {
        float a = a0 + (a1 - a0) * i / 10.f;
        float r0 = r * (i % 5 == 0 ? 0.6f : 0.68f);
        dl->AddLine(ImVec2(c.x + std::cos(a) * r0, c.y + std::sin(a) * r0),
                    ImVec2(c.x + std::cos(a) * r * 0.76f, c.y + std::sin(a) * r * 0.76f), u32(60, 40, 20), 1.5f);
    }
    // Красная зона > 85 %
    dl->PathArcTo(c, r * 0.72f, a0 + (a1 - a0) * 0.85f, a1, 16);
    dl->PathStroke(u32(190, 40, 30), 0, r * 0.08f);
    // Стрелка с лёгкой вибрацией (как у настоящего манометра под давлением)
    float v = std::clamp(value, 0.f, 100.f) / 100.f;
    float jitter = 0.006f * std::sin(time_ * 37.f) * v;
    float a = a0 + (a1 - a0) * (v + jitter);
    dl->AddLine(c, ImVec2(c.x + std::cos(a) * r * 0.7f, c.y + std::sin(a) * r * 0.7f), u32(140, 20, 10), 2.5f);
    dl->AddCircleFilled(c, r * 0.1f, brassDark);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s %.0f%%", label, value);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r * 0.35f), u32(60, 40, 20), buf);
}

void ThemeManager::renderGaugesWindow(bool* open, float cpu, float ram) {
    (void)cpu; (void)ram;
    if (current_ != ThemeId::Steampunk || !steampunk.showGauges) return;
    ImGui::SetNextWindowSize(ImVec2(300, 170), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Localization::instance().tr("theme.gauges_window"), open)) { ImGui::End(); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    float r = std::max(20.f, std::min(avail.x / 4.2f, avail.y / 2.1f));
    drawGauge(dl, ImVec2(p.x + avail.x * 0.25f, p.y + r + 4), r, cpuNeedle_, "CPU");
    drawGauge(dl, ImVec2(p.x + avail.x * 0.75f, p.y + r + 4), r, ramNeedle_, "RAM");
    ImGui::Dummy(ImVec2(avail.x, r * 2 + 8));
    ImGui::End();
}

void ThemeManager::renderStatusBarDecor(float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    switch (current_) {
    case ThemeId::Steampunk: {
        // Мини-шестерёнка, крутящаяся со скоростью, зависящей от CPU
        ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
        float r = h * 0.42f;
        for (int i = 0; i < 8; ++i) {
            float a = gearAngles_[0] + i * kPi / 4;
            dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.6f, c.y + std::sin(a) * r * 0.6f),
                        ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), u32(205, 160, 90), h * 0.14f);
        }
        dl->AddCircleFilled(c, r * 0.7f, u32(181, 136, 66));
        dl->AddCircleFilled(c, r * 0.25f, u32(60, 40, 20));
        ImGui::Dummy(ImVec2(h, h));
        break;
    }
    case ThemeId::Aquarium: {
        // Пузырёк, всплывающий по кругу
        float t = std::fmod(time_ * 0.6f, 1.f);
        dl->AddCircle(ImVec2(p.x + h * 0.5f, p.y + h * (0.85f - 0.7f * t)), h * 0.18f, u32(150, 230, 255, int(255 * (1 - t))));
        ImGui::Dummy(ImVec2(h, h));
        break;
    }
    case ThemeId::Hacker: {
        bool blink = std::fmod(time_, 1.f) < 0.5f;
        ImGui::TextColored(ImVec4(hacker.phosphor[0], hacker.phosphor[1], hacker.phosphor[2], 1), blink ? ">_" : "> ");
        break;
    }
    case ThemeId::Cyberpunk: {
        float pulse = 0.5f + 0.5f * std::sin(time_ * 4.f);
        auto A = cyberpunk.neonA;
        dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.3f), ImVec2(p.x + h * 1.6f, p.y + h * 0.7f),
                          ImGui::ColorConvertFloat4ToU32(ImVec4(A[0], A[1], A[2], 0.4f + 0.6f * pulse)));
        ImGui::Dummy(ImVec2(h * 1.6f, h));
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------------------
//  Подпанели настроек
// ---------------------------------------------------------------------------
void ThemeManager::renderSettings() {
    auto& L = Localization::instance();
    // Выбор темы
    ImGui::SeparatorText(L.tr("theme.select"));
    for (int i = 0; i < (int)ThemeId::Count; ++i) {
        if (i) ImGui::SameLine();
        std::string key = std::string("theme.name.") + nameOf(ThemeId(i));
        if (ImGui::RadioButton(L.tr(key), current_ == ThemeId(i))) { apply(nameOf(ThemeId(i))); changed(); }
    }
    settingsCommon();
    switch (current_) {
    case ThemeId::Aquarium:  settingsAquarium(); break;
    case ThemeId::Steampunk: settingsSteampunk(); break;
    case ThemeId::Hacker:    settingsHacker(); break;
    case ThemeId::Cyberpunk: settingsCyberpunk(); break;
    default: break;
    }
}

void ThemeManager::settingsCommon() {
    auto& L = Localization::instance();
    auto& c = common();
    bool ch = false;
    ImGui::SeparatorText(L.tr("theme.audio"));
    ch |= ImGui::Checkbox(L.tr("theme.audio_enabled"), &c.audioEnabled);
    ImGui::SameLine();
    ch |= ImGui::Checkbox(L.tr("theme.typing_sounds"), &c.typingSounds);
    ImGui::BeginDisabled(!c.audioEnabled);
    if (audio_) {
        float master = audio_->masterVolume.load();
        if (ImGui::SliderFloat(L.tr("theme.master_volume"), &master, 0.f, 1.f, "%.2f")) { audio_->masterVolume = master; ch = true; }
    }
    ch |= ImGui::SliderFloat(L.tr("theme.effects_volume"), &c.effectsVolume, 0.f, 1.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.ambience_volume"), &c.ambienceVolume, 0.f, 1.f, "%.2f");
    if (ImGui::Button(L.tr("theme.test_sound"))) onKeystroke('a'), onBuildFinished(true);
    ImGui::EndDisabled();

    ImGui::SeparatorText(L.tr("theme.visual"));
    bool styleCh = false;
    styleCh |= ImGui::Checkbox(L.tr("theme.animations"), &c.animations);
    styleCh |= ImGui::SliderFloat(L.tr("theme.editor_opacity"), &c.editorOpacity, 0.3f, 1.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.intensity"), &c.intensity, 0.f, 2.f, "%.2f");

    ImGui::SeparatorText(L.tr("theme.background"));
    ImGui::SetNextItemWidth(-160);
    ImGui::InputTextWithHint("##media", L.tr("theme.media_hint"), &mediaInput_);
    ImGui::SameLine();
    if (ImGui::Button(L.tr("theme.apply"))) {
        c.backgroundMedia = mediaInput_;
        mediaError_.clear();
        if (applyBackgroundMedia && !applyBackgroundMedia(c.backgroundMedia, &mediaError_)) c.backgroundMedia.clear();
        ch = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(L.tr("theme.clear"))) {
        c.backgroundMedia.clear(); mediaInput_.clear(); mediaError_.clear();
        if (applyBackgroundMedia) applyBackgroundMedia("", nullptr);
        ch = true;
    }
    if (!mediaError_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", mediaError_.c_str());
    ImGui::TextDisabled("%s", L.tr("theme.media_note"));

    if (styleCh) { applyStyle(); buildPalette(); ch = true; }
    if (ch) { syncAudio(); changed(); }
}

void ThemeManager::settingsAquarium() {
    auto& L = Localization::instance();
    auto& a = aquarium;
    bool ch = false;
    ImGui::SeparatorText(L.tr("theme.name.Aquarium"));
    ch |= ImGui::SliderInt(L.tr("theme.fish_count"), &a.fishCount, 0, 300);
    ch |= ImGui::SliderFloat(L.tr("theme.fish_speed"), &a.fishSpeed, 0.2f, 3.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.scare"), &a.scareStrength, 0.f, 3.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.fish_fog"), &a.fishFog, 0.f, 1.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.rays"), &a.rays, 0.f, 2.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.ripple"), &a.rippleStrength, 0.f, 3.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.wave_damping"), &a.waveDamping, 0.95f, 0.999f, "%.3f");
    ch |= ImGui::ColorEdit3(L.tr("theme.deep_color"), a.deep.data());
    ch |= ImGui::ColorEdit3(L.tr("theme.shallow_color"), a.shallow.data());
    if (ch) { syncSimulations(); changed(); }
}

void ThemeManager::settingsSteampunk() {
    auto& L = Localization::instance();
    auto& s = steampunk;
    bool ch = false;
    ImGui::SeparatorText(L.tr("theme.name.Steampunk"));
    ch |= ImGui::SliderFloat(L.tr("theme.gear_speed"), &s.gearSpeed, 0.f, 4.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.sepia"), &s.sepia, 0.f, 1.f, "%.2f");
    ch |= ImGui::Checkbox(L.tr("theme.show_gauges"), &s.showGauges);
    ch |= ImGui::Checkbox(L.tr("theme.steam_on_build"), &s.steamOnBuild);
    ch |= ImGui::Checkbox(L.tr("theme.bell_on_enter"), &s.bellOnEnter);
    if (ch) changed();
}

void ThemeManager::settingsHacker() {
    auto& L = Localization::instance();
    auto& h = hacker;
    bool ch = false, pal = false;
    ImGui::SeparatorText(L.tr("theme.name.Hacker"));
    pal |= ImGui::ColorEdit3(L.tr("theme.phosphor"), h.phosphor.data());
    ImGui::SameLine();
    if (ImGui::SmallButton(L.tr("theme.green"))) { h.phosphor = {0.2f, 1.0f, 0.35f}; pal = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton(L.tr("theme.amber"))) { h.phosphor = {1.0f, 0.7f, 0.1f}; pal = true; }
    ch |= ImGui::SliderFloat(L.tr("theme.curvature"), &h.curvature, 0.f, 0.3f, "%.3f");
    ch |= ImGui::SliderFloat(L.tr("theme.scanlines"), &h.scanlines, 0.f, 1.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.glow"), &h.glow, 0.f, 2.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.mono"), &h.mono, 0.f, 1.f, "%.2f");
    ch |= ImGui::SliderFloat(L.tr("theme.rain_speed"), &h.rainSpeed, 0.1f, 3.f, "%.2f");
    if (pal) buildPalette();
    if (ch || pal) changed();
}

void ThemeManager::settingsCyberpunk() {
    auto& L = Localization::instance();
    auto& c = cyberpunk;
    bool ch = false, pal = false;
    ImGui::SeparatorText(L.tr("theme.name.Cyberpunk"));
    pal |= ImGui::ColorEdit3(L.tr("theme.neon_a"), c.neonA.data());
    pal |= ImGui::ColorEdit3(L.tr("theme.neon_b"), c.neonB.data());
    ch |= ImGui::SliderFloat(L.tr("theme.aberration"), &c.aberration, 0.f, 6.f, "%.1f px");
    ch |= ImGui::SliderFloat(L.tr("theme.glow"), &c.glow, 0.f, 2.f, "%.2f");
    ch |= ImGui::Checkbox(L.tr("theme.glitch_on_focus"), &c.glitchOnFocus);
    ch |= ImGui::SliderFloat(L.tr("theme.glitch_strength"), &c.glitchStrength, 0.f, 1.f, "%.2f");
    if (ImGui::Button(L.tr("theme.test_glitch"))) { glitch_ = 1.f; if (audio_) audio_->play(Sfx::SynthChord, {0.3f}); }
    if (pal) buildPalette();
    if (ch || pal) changed();
}

// ---------------------------------------------------------------------------
//  JSON
// ---------------------------------------------------------------------------
nlohmann::json ThemeManager::toJson() const {
    nlohmann::json j = nlohmann::json::object();
    for (int i = 0; i < (int)ThemeId::Count; ++i) {
        const auto& c = common_[i];
        j[nameOf(ThemeId(i))] = {
            {"audioEnabled", c.audioEnabled}, {"effectsVolume", c.effectsVolume},
            {"ambienceVolume", c.ambienceVolume}, {"typingSounds", c.typingSounds},
            {"animations", c.animations}, {"editorOpacity", c.editorOpacity},
            {"intensity", c.intensity}, {"backgroundMedia", c.backgroundMedia}};
    }
    auto& a = j["Aquarium"];
    a["fishCount"] = aquarium.fishCount; a["fishSpeed"] = aquarium.fishSpeed; a["scareStrength"] = aquarium.scareStrength;
    a["fishFog"] = aquarium.fishFog; a["rays"] = aquarium.rays; a["waveDamping"] = aquarium.waveDamping;
    a["rippleStrength"] = aquarium.rippleStrength; a["deep"] = aquarium.deep; a["shallow"] = aquarium.shallow;
    auto& s = j["Steampunk"];
    s["gearSpeed"] = steampunk.gearSpeed; s["sepia"] = steampunk.sepia; s["showGauges"] = steampunk.showGauges;
    s["steamOnBuild"] = steampunk.steamOnBuild; s["bellOnEnter"] = steampunk.bellOnEnter;
    auto& h = j["Hacker"];
    h["phosphor"] = hacker.phosphor; h["curvature"] = hacker.curvature; h["scanlines"] = hacker.scanlines;
    h["glow"] = hacker.glow; h["mono"] = hacker.mono; h["rainSpeed"] = hacker.rainSpeed;
    auto& c = j["Cyberpunk"];
    c["neonA"] = cyberpunk.neonA; c["neonB"] = cyberpunk.neonB; c["aberration"] = cyberpunk.aberration;
    c["glitchStrength"] = cyberpunk.glitchStrength; c["glow"] = cyberpunk.glow; c["glitchOnFocus"] = cyberpunk.glitchOnFocus;
    return j;
}

void ThemeManager::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    for (int i = 0; i < (int)ThemeId::Count; ++i) {
        auto it = j.find(nameOf(ThemeId(i)));
        if (it == j.end() || !it->is_object()) continue;
        auto& c = common_[i];
        rd(*it, "audioEnabled", c.audioEnabled); rd(*it, "effectsVolume", c.effectsVolume);
        rd(*it, "ambienceVolume", c.ambienceVolume); rd(*it, "typingSounds", c.typingSounds);
        rd(*it, "animations", c.animations); rd(*it, "editorOpacity", c.editorOpacity);
        rd(*it, "intensity", c.intensity); rd(*it, "backgroundMedia", c.backgroundMedia);
    }
    if (auto it = j.find("Aquarium"); it != j.end()) {
        auto& a = *it;
        rd(a, "fishCount", aquarium.fishCount); rd(a, "fishSpeed", aquarium.fishSpeed);
        rd(a, "scareStrength", aquarium.scareStrength); rd(a, "fishFog", aquarium.fishFog);
        rd(a, "rays", aquarium.rays); rd(a, "waveDamping", aquarium.waveDamping);
        rd(a, "rippleStrength", aquarium.rippleStrength); rd(a, "deep", aquarium.deep); rd(a, "shallow", aquarium.shallow);
    }
    if (auto it = j.find("Steampunk"); it != j.end()) {
        auto& s = *it;
        rd(s, "gearSpeed", steampunk.gearSpeed); rd(s, "sepia", steampunk.sepia); rd(s, "showGauges", steampunk.showGauges);
        rd(s, "steamOnBuild", steampunk.steamOnBuild); rd(s, "bellOnEnter", steampunk.bellOnEnter);
    }
    if (auto it = j.find("Hacker"); it != j.end()) {
        auto& h = *it;
        rd(h, "phosphor", hacker.phosphor); rd(h, "curvature", hacker.curvature); rd(h, "scanlines", hacker.scanlines);
        rd(h, "glow", hacker.glow); rd(h, "mono", hacker.mono); rd(h, "rainSpeed", hacker.rainSpeed);
    }
    if (auto it = j.find("Cyberpunk"); it != j.end()) {
        auto& c = *it;
        rd(c, "neonA", cyberpunk.neonA); rd(c, "neonB", cyberpunk.neonB); rd(c, "aberration", cyberpunk.aberration);
        rd(c, "glitchStrength", cyberpunk.glitchStrength); rd(c, "glow", cyberpunk.glow);
        rd(c, "glitchOnFocus", cyberpunk.glitchOnFocus);
    }
    // Защита от неадекватных значений из вручную отредактированного файла.
    aquarium.fishCount = std::clamp(aquarium.fishCount, 0, 1000);
    aquarium.waveDamping = std::clamp(aquarium.waveDamping, 0.9f, 0.9995f);
    for (auto& c : common_) c.editorOpacity = std::clamp(c.editorOpacity, 0.2f, 1.f);
    lastFishCount_ = -1;
}

} // namespace ide
