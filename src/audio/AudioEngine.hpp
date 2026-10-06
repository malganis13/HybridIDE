// =============================================================================
//  AudioEngine.hpp — звуковая подсистема на miniaudio:
//   • процедурный синтез всех эффектов при старте (без внешних файлов):
//     всплеск воды, пузырьки, печатная машинка, клапан пара, герконовый
//     клик, синтвейв-импульс, низкочастотный гул, океанский эмбиент
//   • пространственное микширование: панорама по X экрана (equal-power),
//     затухание по расстоянию, посыл в ревербератор (глубина источника)
//   • lock-free очередь команд UI -> аудиопоток
//   • загрузка пользовательских сэмплов (wav/mp3/flac) через ma_decoder
// =============================================================================
#pragma once
#include "core/EventQueue.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ide {

enum class Sfx : std::uint8_t {
    Splash, Bubble, Typewriter, TypewriterBell, SteamValve, ReedSwitch, SynthBlip, SynthChord, Click, Error, Count
};
enum class Ambience : std::uint8_t { None, Ocean, Workshop, CrtHum, NeonHum, Count };

struct PlayParams {
    float gain   = 1.f;
    float pan    = 0.f;    // -1 (лево) .. +1 (право)
    float pitch  = 1.f;    // множитель скорости воспроизведения
    float reverb = 0.f;    // посыл в ревербератор 0..1 («глубина» источника)
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    bool init(std::string* err = nullptr);
    void shutdown();
    [[nodiscard]] bool ready() const { return device_ != nullptr; }

    void play(Sfx s, PlayParams p = {});
    // Пространственный вариант: позиция в пикселях относительно окна
    void playAt(Sfx s, float screenX, float screenY, float screenW, float screenH, float depth = 0.3f, float gain = 1.f);
    void setAmbience(Ambience a, float fadeSeconds = 1.5f);
    bool loadSample(Sfx slot, const std::filesystem::path& file, std::string* err = nullptr);  // заменить процедурный звук

    std::atomic<float> masterVolume{0.8f};
    std::atomic<float> effectsVolume{0.8f};
    std::atomic<float> ambienceVolume{0.35f};
    std::atomic<bool>  enabled{true};

    // Вызывается из аудиопотока miniaudio
    void mix(float* out, std::uint32_t frames);

private:
    struct Buffer { std::vector<float> data; };              // моно, sampleRate_
    struct Voice  { const Buffer* buf = nullptr; double pos = 0; PlayParams p; };
    struct Command { enum Type { Play, Ambient } type; Sfx sfx; PlayParams p; Ambience amb; float fade; };

    void synthesizeAll();
    Buffer synth(Sfx s) const;
    Buffer synthAmbience(Ambience a) const;

    void*                 device_ = nullptr;   // ma_device*
    std::uint32_t         sampleRate_ = 48000;
    std::array<std::shared_ptr<Buffer>, (std::size_t)Sfx::Count>      sfx_;
    std::array<std::shared_ptr<Buffer>, (std::size_t)Ambience::Count> amb_;
    MpscQueue<Command>    cmds_;
    // Состояние аудиопотока (доступно только из mix)
    std::vector<Voice>    voices_;
    const Buffer*         ambCur_ = nullptr; double ambPos_ = 0; float ambGain_ = 0, ambFadeStep_ = 1e-4f;
    const Buffer*         ambNext_ = nullptr;
    // Ревербератор Шрёдера: 4 гребенчатых + 2 всепропускающих фильтра
    struct Comb { std::vector<float> buf; std::size_t idx = 0; float fb = 0.8f, damp = 0.2f, store = 0; };
    struct AllPass { std::vector<float> buf; std::size_t idx = 0; };
    std::array<Comb, 4>    combs_;
    std::array<AllPass, 2> allpass_;
    std::mutex             loadMtx_;
    std::vector<std::shared_ptr<Buffer>> retired_;
};

} // namespace ide
