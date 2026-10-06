// =============================================================================
//  ShaderEngine.hpp — рендер-конвейер тем:
//    1) сцена (FBO): фоновый процедурный шейдер темы + рыбы (аквариум)
//    2) поверх — интерфейс Dear ImGui (рендерится в тот же FBO)
//    3) пост-проход на экран: CRT / хроматическая аберрация + глитч / сепия
//  Поле ряби (WaveSimulation) загружается в R32F-текстуру каждый кадр.
// =============================================================================
#pragma once
#include "graphics/FishSimulation.hpp"
#include "graphics/GLLoader.hpp"
#include "graphics/WaveSimulation.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ide {

enum class BackgroundKind { None, Aquarium, Steampunk, Matrix, Cyberpunk };
enum class PostKind { None = 0, CRT = 1, Cyber = 2, Sepia = 3 };

// Набор параметров, которые ThemeManager передаёт в шейдеры
struct ShaderUniforms {
    BackgroundKind background = BackgroundKind::None;
    PostKind       post = PostKind::None;
    float time = 0, intensity = 1, rays = 1;
    std::array<float, 3> deepColor{0.0f, 0.06f, 0.16f}, shallowColor{0.0f, 0.35f, 0.55f};
    std::array<float, 4> gearAngles{};
    std::array<float, 3> tint{0.2f, 1.0f, 0.35f};
    std::array<float, 3> colorA{1.0f, 0.1f, 0.75f}, colorB{0.0f, 0.95f, 1.0f};
    float curvature = 0.08f, scanlines = 0.35f, glow = 0.6f, mono = 0.85f;
    float aberration = 1.5f, glitch = 0.f, sepia = 0.25f;
    float fishFog = 0.25f;
    std::array<float, 3> clear{0.1f, 0.1f, 0.1f};
};

class ShaderEngine {
public:
    ~ShaderEngine() { destroy(); }
    bool init(gl::GetProcFn getProc, std::string* err = nullptr);
    void destroy();
    bool reloadFromDirectory(const std::filesystem::path& dir, std::string* err = nullptr);

    // Кадр
    void beginScene(int fbWidth, int fbHeight, const ShaderUniforms& u, const WaveSimulation* wave,
                    const std::vector<FishVertex>* fish, float logicalW, float logicalH);
    void endSceneToScreen(const ShaderUniforms& u);

    bool setBackgroundMedia(const std::filesystem::path& image, std::string* err = nullptr);
    void clearBackgroundMedia();
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] const std::string& lastError() const { return lastError_; }

    static gl::GLuint compile(const char* vs, const char* fs, std::string* err);

private:
    void ensureTargets(int w, int h);
    void uploadWave(const WaveSimulation& w);
    gl::GLint loc(gl::GLuint prog, const char* name);

    bool ready_ = false;
    std::string lastError_;
    std::map<BackgroundKind, gl::GLuint> bgPrograms_;
    gl::GLuint postProgram_ = 0, fishProgram_ = 0;
    gl::GLuint emptyVao_ = 0, fishVao_ = 0, fishVbo_ = 0;
    gl::GLuint fbo_ = 0, sceneTex_ = 0, waveTex_ = 0, mediaTex_ = 0;
    int fbW_ = 0, fbH_ = 0, waveW_ = 0, waveH_ = 0;
    bool hasMedia_ = false;
    std::map<std::pair<gl::GLuint, std::string>, gl::GLint> locCache_;
};

} // namespace ide
