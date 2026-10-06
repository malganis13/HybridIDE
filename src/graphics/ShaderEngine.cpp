// =============================================================================
//  ShaderEngine.cpp
// =============================================================================
#include "graphics/ShaderEngine.hpp"

#include "graphics/Shaders.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include <stb_image.h>

#include <fstream>
#include <sstream>

namespace ide {

using namespace gl;

GLuint ShaderEngine::compile(const char* vsSrc, const char* fsSrc, std::string* err) {
    auto stage = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = CreateShader(type);
        ShaderSource(s, 1, &src, nullptr);
        CompileShader(s);
        GLint ok = 0;
        GetShaderiv(s, COMPILE_STATUS, &ok);
        if (!ok) {
            GLint len = 0; GetShaderiv(s, INFO_LOG_LENGTH, &len);
            std::string log((std::size_t)std::max(1, len), '\0');
            GetShaderInfoLog(s, len, nullptr, log.data());
            if (err) *err = std::string(type == VERTEX_SHADER ? "VS: " : "FS: ") + log;
            DeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = stage(VERTEX_SHADER, vsSrc);
    if (!vs) return 0;
    GLuint fs = stage(FRAGMENT_SHADER, fsSrc);
    if (!fs) { DeleteShader(vs); return 0; }
    GLuint p = CreateProgram();
    AttachShader(p, vs); AttachShader(p, fs);
    LinkProgram(p);
    DeleteShader(vs); DeleteShader(fs);
    GLint ok = 0;
    GetProgramiv(p, LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; GetProgramiv(p, INFO_LOG_LENGTH, &len);
        std::string log((std::size_t)std::max(1, len), '\0');
        GetProgramInfoLog(p, len, nullptr, log.data());
        if (err) *err = "LINK: " + log;
        DeleteProgram(p);
        return 0;
    }
    return p;
}

bool ShaderEngine::init(GetProcFn getProc, std::string* err) {
    const char* missing = nullptr;
    if (!gl::load(getProc, &missing)) {
        lastError_ = std::string("OpenGL function not found: ") + (missing ? missing : "?");
        if (err) *err = lastError_;
        return false;
    }
    struct { BackgroundKind k; const char* fs; const char* name; } progs[] = {
        {BackgroundKind::Aquarium, shaders::kAquariumFS, "aquarium"},
        {BackgroundKind::Steampunk, shaders::kSteampunkFS, "steampunk"},
        {BackgroundKind::Matrix, shaders::kMatrixFS, "matrix"},
        {BackgroundKind::Cyberpunk, shaders::kCyberFS, "cyberpunk"}};
    for (auto& p : progs) {
        std::string e;
        GLuint id = compile(shaders::kFullscreenVS, p.fs, &e);
        if (!id) { lastError_ = std::string(p.name) + ": " + e; if (err) *err = lastError_; return false; }
        bgPrograms_[p.k] = id;
    }
    std::string e;
    postProgram_ = compile(shaders::kFullscreenVS, shaders::kPostFS, &e);
    if (!postProgram_) { lastError_ = "post: " + e; if (err) *err = lastError_; return false; }
    fishProgram_ = compile(shaders::kFishVS, shaders::kFishFS, &e);
    if (!fishProgram_) { lastError_ = "fish: " + e; if (err) *err = lastError_; return false; }

    GenVertexArrays(1, &emptyVao_);
    GenVertexArrays(1, &fishVao_);
    GenBuffers(1, &fishVbo_);
    BindVertexArray(fishVao_);
    BindBuffer(ARRAY_BUFFER, fishVbo_);
    EnableVertexAttribArray(0);
    VertexAttribPointer(0, 2, FLOAT, 0, sizeof(FishVertex), (void*)0);
    EnableVertexAttribArray(1);
    VertexAttribPointer(1, 4, FLOAT, 0, sizeof(FishVertex), (void*)(2 * sizeof(float)));
    BindVertexArray(0);
    ready_ = true;
    return true;
}

bool ShaderEngine::reloadFromDirectory(const std::filesystem::path& dir, std::string* err) {
    // Горячая перезагрузка: resources/shaders/<theme>.frag переопределяет встроенный
    auto read = [](const std::filesystem::path& p) -> std::string {
        std::ifstream f(p); if (!f) return {};
        std::stringstream ss; ss << f.rdbuf(); return ss.str();
    };
    std::pair<BackgroundKind, const char*> files[] = {{BackgroundKind::Aquarium, "aquarium.frag"}, {BackgroundKind::Steampunk, "steampunk.frag"},
                                                      {BackgroundKind::Matrix, "matrix.frag"}, {BackgroundKind::Cyberpunk, "cyberpunk.frag"}};
    bool any = false;
    for (auto& [k, name] : files) {
        std::string src = read(dir / name);
        if (src.empty()) continue;
        std::string e;
        GLuint p = compile(shaders::kFullscreenVS, src.c_str(), &e);
        if (!p) { if (err) *err = std::string(name) + ": " + e; return false; }
        DeleteProgram(bgPrograms_[k]);
        bgPrograms_[k] = p;
        any = true;
    }
    std::string post = read(dir / "post.frag");
    if (!post.empty()) {
        std::string e;
        GLuint p = compile(shaders::kFullscreenVS, post.c_str(), &e);
        if (!p) { if (err) *err = "post.frag: " + e; return false; }
        DeleteProgram(postProgram_);
        postProgram_ = p;
        any = true;
    }
    locCache_.clear();
    return any;
}

void ShaderEngine::destroy() {
    if (!ready_) return;
    for (auto& [_, p] : bgPrograms_) DeleteProgram(p);
    bgPrograms_.clear();
    DeleteProgram(postProgram_); DeleteProgram(fishProgram_);
    DeleteVertexArrays(1, &emptyVao_); DeleteVertexArrays(1, &fishVao_); DeleteBuffers(1, &fishVbo_);
    if (fbo_) DeleteFramebuffers(1, &fbo_);
    GLuint texs[] = {sceneTex_, waveTex_, mediaTex_};
    for (GLuint t : texs) if (t) DeleteTextures(1, &t);
    ready_ = false;
}

GLint ShaderEngine::loc(GLuint prog, const char* name) {
    auto key = std::make_pair(prog, std::string(name));
    auto it = locCache_.find(key);
    if (it != locCache_.end()) return it->second;
    GLint l = GetUniformLocation(prog, name);
    locCache_[key] = l;
    return l;
}

void ShaderEngine::ensureTargets(int w, int h) {
    if (w == fbW_ && h == fbH_ && fbo_) return;
    fbW_ = w; fbH_ = h;
    if (!fbo_) GenFramebuffers(1, &fbo_);
    if (!sceneTex_) GenTextures(1, &sceneTex_);
    BindTexture(TEXTURE_2D, sceneTex_);
    TexImage2D(TEXTURE_2D, 0, (GLint)RGBA8, w, h, 0, RGBA, UNSIGNED_BYTE, nullptr);
    TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, (GLint)LINEAR);
    TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, (GLint)LINEAR);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, (GLint)CLAMP_TO_EDGE);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, (GLint)CLAMP_TO_EDGE);
    BindFramebuffer(FRAMEBUFFER, fbo_);
    FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, sceneTex_, 0);
    if (CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) lastError_ = "FBO incomplete";
    BindFramebuffer(FRAMEBUFFER, 0);
}

void ShaderEngine::uploadWave(const WaveSimulation& w) {
    if (!waveTex_) GenTextures(1, &waveTex_);
    BindTexture(TEXTURE_2D, waveTex_);
    PixelStorei(UNPACK_ALIGNMENT, 4);
    if (w.width() != waveW_ || w.height() != waveH_) {
        waveW_ = w.width(); waveH_ = w.height();
        TexImage2D(TEXTURE_2D, 0, (GLint)R32F, waveW_, waveH_, 0, RED, FLOAT, w.data());
        TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, (GLint)LINEAR);
        TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, (GLint)LINEAR);
        TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, (GLint)CLAMP_TO_EDGE);
        TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, (GLint)CLAMP_TO_EDGE);
    } else {
        TexSubImage2D(TEXTURE_2D, 0, 0, 0, waveW_, waveH_, RED, FLOAT, w.data());
    }
}

bool ShaderEngine::setBackgroundMedia(const std::filesystem::path& image, std::string* err) {
    int w = 0, h = 0, n = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load(image.string().c_str(), &w, &h, &n, 4);
    if (!px) { if (err) *err = std::string("stb_image: ") + stbi_failure_reason(); return false; }
    if (!mediaTex_) GenTextures(1, &mediaTex_);
    BindTexture(TEXTURE_2D, mediaTex_);
    PixelStorei(UNPACK_ALIGNMENT, 1);
    TexImage2D(TEXTURE_2D, 0, (GLint)RGBA8, w, h, 0, RGBA, UNSIGNED_BYTE, px);
    TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, (GLint)LINEAR);
    TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, (GLint)LINEAR);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, (GLint)CLAMP_TO_EDGE);
    TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, (GLint)CLAMP_TO_EDGE);
    stbi_image_free(px);
    hasMedia_ = true;
    return true;
}

void ShaderEngine::clearBackgroundMedia() { hasMedia_ = false; }

void ShaderEngine::beginScene(int fbW, int fbH, const ShaderUniforms& u, const WaveSimulation* wave,
                              const std::vector<FishVertex>* fish, float logicalW, float logicalH) {
    if (!ready_ || fbW <= 0 || fbH <= 0) return;
    ensureTargets(fbW, fbH);
    BindFramebuffer(FRAMEBUFFER, fbo_);
    Viewport(0, 0, fbW, fbH);
    Disable(SCISSOR_TEST);
    ClearColor(u.clear[0], u.clear[1], u.clear[2], 1.f);
    Clear(COLOR_BUFFER_BIT);

    auto it = bgPrograms_.find(u.background);
    if (it != bgPrograms_.end()) {
        GLuint p = it->second;
        UseProgram(p);
        Uniform1f(loc(p, "uTime"), u.time);
        Uniform2f(loc(p, "uRes"), (float)fbW, (float)fbH);
        Uniform1f(loc(p, "uIntensity"), u.intensity);
        Uniform1i(loc(p, "uHasMedia"), hasMedia_ ? 1 : 0);
        if (hasMedia_) { ActiveTexture(TEXTURE1); BindTexture(TEXTURE_2D, mediaTex_); Uniform1i(loc(p, "uMedia"), 1); }
        switch (u.background) {
            case BackgroundKind::Aquarium:
                if (wave) { ActiveTexture(TEXTURE0); uploadWave(*wave); Uniform1i(loc(p, "uWave"), 0); }
                Uniform1f(loc(p, "uRays"), u.rays);
                Uniform3f(loc(p, "uDeepColor"), u.deepColor[0], u.deepColor[1], u.deepColor[2]);
                Uniform3f(loc(p, "uShallowColor"), u.shallowColor[0], u.shallowColor[1], u.shallowColor[2]);
                break;
            case BackgroundKind::Steampunk:
                Uniform4f(loc(p, "uGearAngles"), u.gearAngles[0], u.gearAngles[1], u.gearAngles[2], u.gearAngles[3]);
                break;
            case BackgroundKind::Matrix:
                Uniform3f(loc(p, "uTint"), u.tint[0], u.tint[1], u.tint[2]);
                break;
            case BackgroundKind::Cyberpunk:
                Uniform3f(loc(p, "uColorA"), u.colorA[0], u.colorA[1], u.colorA[2]);
                Uniform3f(loc(p, "uColorB"), u.colorB[0], u.colorB[1], u.colorB[2]);
                break;
            default: break;
        }
        BindVertexArray(emptyVao_);
        DrawArrays(TRIANGLES, 0, 3);
        ActiveTexture(TEXTURE0);
    }

    // Рыбы поверх фона (за панелями редактора)
    if (fish && !fish->empty()) {
        Enable(BLEND);
        BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
        UseProgram(fishProgram_);
        Uniform2f(loc(fishProgram_, "uRes"), logicalW, logicalH);
        Uniform1f(loc(fishProgram_, "uDepthFog"), u.fishFog);
        Uniform3f(loc(fishProgram_, "uFogColor"), u.deepColor[0], u.deepColor[1] + 0.1f, u.deepColor[2] + 0.15f);
        BindVertexArray(fishVao_);
        BindBuffer(ARRAY_BUFFER, fishVbo_);
        BufferData(ARRAY_BUFFER, (GLsizeiptr)(fish->size() * sizeof(FishVertex)), fish->data(), STREAM_DRAW);
        DrawArrays(TRIANGLES, 0, (GLsizei)fish->size());
    }
    BindVertexArray(0);
    UseProgram(0);
    // Далее ImGui рисует интерфейс в этот же FBO
}

void ShaderEngine::endSceneToScreen(const ShaderUniforms& u) {
    if (!ready_) return;
    BindFramebuffer(FRAMEBUFFER, 0);
    Viewport(0, 0, fbW_, fbH_);
    Disable(BLEND);
    Disable(SCISSOR_TEST);
    UseProgram(postProgram_);
    ActiveTexture(TEXTURE0);
    BindTexture(TEXTURE_2D, sceneTex_);
    Uniform1i(loc(postProgram_, "uScene"), 0);
    Uniform2f(loc(postProgram_, "uRes"), (float)fbW_, (float)fbH_);
    Uniform1f(loc(postProgram_, "uTime"), u.time);
    Uniform1i(loc(postProgram_, "uMode"), (int)u.post);
    Uniform1f(loc(postProgram_, "uCurvature"), u.curvature);
    Uniform1f(loc(postProgram_, "uScanlines"), u.scanlines);
    Uniform1f(loc(postProgram_, "uGlow"), u.glow);
    Uniform3f(loc(postProgram_, "uTint"), u.tint[0], u.tint[1], u.tint[2]);
    Uniform1f(loc(postProgram_, "uMono"), u.mono);
    Uniform1f(loc(postProgram_, "uAberration"), u.aberration);
    Uniform1f(loc(postProgram_, "uGlitch"), u.glitch);
    Uniform1f(loc(postProgram_, "uSepia"), u.sepia);
    BindVertexArray(emptyVao_);
    DrawArrays(TRIANGLES, 0, 3);
    BindVertexArray(0);
    UseProgram(0);
}

} // namespace ide
