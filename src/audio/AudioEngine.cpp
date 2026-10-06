// =============================================================================
//  AudioEngine.cpp
// =============================================================================
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>

#include "audio/AudioEngine.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace ide {

static constexpr float kPi = 3.14159265358979f;

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { shutdown(); }

static void dataCallback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    static_cast<AudioEngine*>(dev->pUserData)->mix(static_cast<float*>(out), frames);
}

bool AudioEngine::init(std::string* err) {
    synthesizeAll();
    auto* dev = new ma_device();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = sampleRate_;
    cfg.dataCallback      = dataCallback;
    cfg.pUserData         = this;
    cfg.performanceProfile = ma_performance_profile_low_latency;   // минимальная задержка для кликов
    if (ma_device_init(nullptr, &cfg, dev) != MA_SUCCESS) {
        delete dev;
        if (err) *err = "Аудиоустройство недоступно (miniaudio)";
        return false;
    }
    if (ma_device_start(dev) != MA_SUCCESS) {
        ma_device_uninit(dev); delete dev;
        if (err) *err = "Не удалось запустить аудиопоток";
        return false;
    }
    device_ = dev;
    return true;
}

void AudioEngine::shutdown() {
    if (!device_) return;
    auto* dev = static_cast<ma_device*>(device_);
    ma_device_uninit(dev);
    delete dev;
    device_ = nullptr;
}

void AudioEngine::play(Sfx s, PlayParams p) {
    if (!enabled || !device_) return;
    cmds_.push({Command::Play, s, p, Ambience::None, 0.f});
}

void AudioEngine::playAt(Sfx s, float x, float y, float w, float h, float depth, float gain) {
    PlayParams p;
    p.pan    = std::clamp((x / std::max(1.f, w)) * 2.f - 1.f, -1.f, 1.f);
    // Чем ниже по экрану (глубже в «воде»), тем больше реверберации и глуше звук
    float yn = std::clamp(y / std::max(1.f, h), 0.f, 1.f);
    p.reverb = std::clamp(depth + yn * 0.5f, 0.f, 1.f);
    p.gain   = gain * (1.f - 0.35f * yn);
    static thread_local std::mt19937 rng{1234};
    p.pitch  = std::uniform_real_distribution<float>(0.9f, 1.12f)(rng) * (1.f - 0.15f * yn);
    play(s, p);
}

void AudioEngine::setAmbience(Ambience a, float fade) {
    if (!device_) return;
    cmds_.push({Command::Ambient, Sfx::Click, {}, a, fade});
}

bool AudioEngine::loadSample(Sfx slot, const std::filesystem::path& file, std::string* err) {
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 1, sampleRate_);
    ma_decoder dec;
    if (ma_decoder_init_file(file.string().c_str(), &dc, &dec) != MA_SUCCESS) { if (err) *err = "Не удалось декодировать " + file.string(); return false; }
    auto buf = std::make_shared<Buffer>();
    float tmp[4096];
    for (;;) {
        ma_uint64 read = 0;
        ma_decoder_read_pcm_frames(&dec, tmp, 4096, &read);
        if (read == 0) break;
        buf->data.insert(buf->data.end(), tmp, tmp + read);
        if (buf->data.size() > (std::size_t)sampleRate_ * 20) break;   // лимит 20 с для эффектов
    }
    ma_decoder_uninit(&dec);
    std::lock_guard lk(loadMtx_);
    // Старый буфер может ещё звучать в аудиопотоке — держим его живым до shutdown()
    retired_.push_back(std::atomic_load(&sfx_[(std::size_t)slot]));
    std::atomic_store(&sfx_[(std::size_t)slot], buf);
    return true;
}

// ---------------------------------------------------------------------------
//  Процедурный синтез
// ---------------------------------------------------------------------------
void AudioEngine::synthesizeAll() {
    for (std::size_t i = 0; i < sfx_.size(); ++i) sfx_[i] = std::make_shared<Buffer>(synth((Sfx)i));
    for (std::size_t i = 1; i < amb_.size(); ++i) amb_[i] = std::make_shared<Buffer>(synthAmbience((Ambience)i));
    // Длины линий задержки ревербератора (взаимно простые, ~25–40 мс)
    const int combLen[4] = {1557, 1617, 1491, 1422}, apLen[2] = {556, 441};
    for (int i = 0; i < 4; ++i) { combs_[(std::size_t)i].buf.assign((std::size_t)(combLen[i] * sampleRate_ / 44100), 0.f); combs_[(std::size_t)i].fb = 0.84f; }
    for (int i = 0; i < 2; ++i) allpass_[(std::size_t)i].buf.assign((std::size_t)(apLen[i] * sampleRate_ / 44100), 0.f);
    voices_.reserve(64);
}

AudioEngine::Buffer AudioEngine::synth(Sfx s) const {
    const float sr = (float)sampleRate_;
    Buffer b;
    std::mt19937 rng(42 + (unsigned)s);
    std::uniform_real_distribution<float> noise(-1.f, 1.f);
    auto alloc = [&](float secs) { b.data.assign((std::size_t)(secs * sr), 0.f); };
    auto env = [](float t, float a, float d) { return t < a ? t / a : std::exp(-(t - a) / d); };   // атака/экспоненциальный спад

    switch (s) {
    case Sfx::Splash: {
        // Всплеск: полосовой шум с падающей частотой + «пузырьки» (синусоиды с растущим тоном)
        alloc(0.9f);
        float lp = 0, bp = 0;
        for (std::size_t i = 0; i < b.data.size(); ++i) {
            float t = (float)i / sr;
            float fc = 2400.f * std::exp(-t * 4.f) + 300.f;
            float f = 2.f * std::sin(kPi * fc / sr);          // фильтр Чемберлена (state-variable)
            float hp = noise(rng) - lp - 0.6f * bp;
            bp += f * hp; lp += f * bp;
            b.data[i] = bp * env(t, 0.004f, 0.18f) * 0.9f;
        }
        for (int k = 0; k < 7; ++k) {   // пузырьки по модели Миннерта: тон растёт по мере всплытия
            float start = 0.05f + 0.09f * (float)k + 0.03f * noise(rng), f0 = 500.f + 700.f * std::abs(noise(rng));
            for (float t = 0; t < 0.12f; t += 1.f / sr) {
                std::size_t i = (std::size_t)((start + t) * sr);
                if (i >= b.data.size()) break;
                float f = f0 * (1.f + 2.5f * t);
                b.data[i] += 0.25f * std::sin(2 * kPi * f * t) * std::exp(-t * 30.f);
            }
        }
        break;
    }
    case Sfx::Bubble: {
        alloc(0.15f);
        for (std::size_t i = 0; i < b.data.size(); ++i) { float t = (float)i / sr; b.data[i] = 0.5f * std::sin(2 * kPi * (600.f + 2400.f * t) * t) * std::exp(-t * 28.f); }
        break;
    }
    case Sfx::Typewriter: {
        // Удар литеры: короткий щелчок шума + низкий «тук» каретки + металлический призвук
        alloc(0.12f);
        for (std::size_t i = 0; i < b.data.size(); ++i) {
            float t = (float)i / sr;
            float click = noise(rng) * std::exp(-t * 400.f);
            float thump = std::sin(2 * kPi * 140.f * t) * std::exp(-t * 60.f);
            float ring  = std::sin(2 * kPi * 3150.f * t) * std::exp(-t * 90.f) * 0.15f;
            b.data[i] = 0.7f * click + 0.5f * thump + ring;
        }
        break;
    }
    case Sfx::TypewriterBell: {
        alloc(1.2f);
        for (std::size_t i = 0; i < b.data.size(); ++i) {
            float t = (float)i / sr;
            b.data[i] = (std::sin(2 * kPi * 2093.f * t) + 0.5f * std::sin(2 * kPi * 5230.f * t) + 0.3f * std::sin(2 * kPi * 7400.f * t)) * std::exp(-t * 3.5f) * 0.3f;
        }
        break;
    }
    case Sfx::SteamValve: {
        // Сброс пара: широкополосное шипение с резонансом, огибающая «пшшш» 1.6 с
        alloc(1.8f);
        float lp = 0, bp = 0;
        for (std::size_t i = 0; i < b.data.size(); ++i) {
            float t = (float)i / sr;
            float fc = 3500.f + 1500.f * std::sin(t * 9.f);
            float f = 2.f * std::sin(kPi * fc / sr);
            float hp = noise(rng) - lp - 0.25f * bp;
            bp += f * hp; lp += f * bp;
            float e = std::min(1.f, t / 0.05f) * (t < 1.1f ? 1.f : std::exp(-(t - 1.1f) * 6.f));
            b.data[i] = (hp * 0.35f + bp * 0.4f) * e * 0.7f;
        }
        break;
    }
    case Sfx::ReedSwitch: {
        // Герконовый переключатель терминала 80-х: два быстрых металлических щелчка (замыкание + дребезг)
        alloc(0.09f);
        for (std::size_t i = 0; i < b.data.size(); ++i) {
            float t = (float)i / sr;
            float c1 = noise(rng) * std::exp(-t * 900.f);
            float t2 = t - 0.012f;
            float c2 = t2 > 0 ? noise(rng) * std::exp(-t2 * 1200.f) * 0.6f : 0.f;
            float body = std::sin(2 * kPi * 1800.f * t) * std::exp(-t * 120.f) * 0.3f + std::sin(2 * kPi * 220.f * t) * std::exp(-t * 70.f) * 0.4f;
            b.data[i] = c1 * 0.8f + c2 + body;
        }
        break;
    }
    case Sfx::SynthBlip: {
        // Синтвейв: пила с вибрато через резонансный ФНЧ со «свипом» вниз
        alloc(0.35f);
        float lp = 0, bp = 0, ph = 0;
        for (std::size_t i = 0; i < b.data.size(); ++i) {
            float t = (float)i / sr;
            float f0 = 440.f * (1.f + 0.01f * std::sin(2 * kPi * 6.f * t));
            ph += f0 / sr; ph -= std::floor(ph);
            float saw = 2.f * ph - 1.f;
            float fc = 200.f + 4000.f * std::exp(-t * 12.f);
            float f = 2.f * std::sin(kPi * std::min(fc, sr * 0.2f) / sr);
            float hp = saw - lp - 0.3f * bp;
            bp += f * hp; lp += f * bp;
            b.data[i] = lp * env(t, 0.003f, 0.09f) * 0.6f;
        }
        break;
    }
    case Sfx::SynthChord: {
        // Аккорд успешной сборки (Am add9) с медленной атакой
        alloc(1.6f);
        const float notes[] = {220.f, 261.63f, 329.63f, 493.88f};
        for (std::size_t i = 0; i < b.data.size(); ++i) {
            float t = (float)i / sr, v = 0;
            for (float n : notes) { float ph = std::fmod(n * t, 1.f); v += (2.f * ph - 1.f) * 0.15f + std::sin(2 * kPi * n * 1.005f * t) * 0.1f; }
            b.data[i] = v * env(t, 0.08f, 0.5f);
        }
        break;
    }
    case Sfx::Click: {
        alloc(0.03f);
        for (std::size_t i = 0; i < b.data.size(); ++i) { float t = (float)i / sr; b.data[i] = (noise(rng) * 0.5f + std::sin(2 * kPi * 2000.f * t)) * std::exp(-t * 300.f) * 0.5f; }
        break;
    }
    case Sfx::Error: {
        alloc(0.4f);
        for (std::size_t i = 0; i < b.data.size(); ++i) {
            float t = (float)i / sr;
            float sq = std::sin(2 * kPi * 160.f * t) > 0 ? 0.3f : -0.3f;
            b.data[i] = (sq + 0.3f * std::sin(2 * kPi * 120.f * t)) * (t < 0.15f || (t > 0.2f && t < 0.35f) ? 1.f : 0.f) * 0.6f;
        }
        break;
    }
    default: alloc(0.01f); break;
    }
    // Нормализация пика до -1 dBFS
    float peak = 1e-6f;
    for (float v : b.data) peak = std::max(peak, std::abs(v));
    for (float& v : b.data) v *= 0.89f / peak;
    return b;
}

AudioEngine::Buffer AudioEngine::synthAmbience(Ambience a) const {
    // Бесшовные 8-секундные петли (кроссфейд концов)
    const float sr = (float)sampleRate_;
    Buffer b;
    b.data.assign((std::size_t)(8.f * sr), 0.f);
    std::mt19937 rng(777 + (unsigned)a);
    std::uniform_real_distribution<float> noise(-1.f, 1.f);
    float brown = 0, lp = 0;
    for (std::size_t i = 0; i < b.data.size(); ++i) {
        float t = (float)i / sr, v = 0;
        switch (a) {
        case Ambience::Ocean:     // «гидроакустический шум»: коричневый шум с медленными волнами
            brown = std::clamp(brown + noise(rng) * 0.02f, -1.f, 1.f);
            v = brown * (0.6f + 0.4f * std::sin(2 * kPi * 0.125f * t)) * 0.8f;
            break;
        case Ambience::Workshop:  // мастерская: тиканье механизмов + низкий гул
            v = 0.15f * std::sin(2 * kPi * 50.f * t) + (std::fmod(t, 0.5f) < 0.004f ? noise(rng) * 0.6f : 0.f);
            lp += 0.01f * (noise(rng) - lp); v += lp * 0.5f;
            break;
        case Ambience::CrtHum:    // строчная развёртка CRT 15.7 кГц (слышимая) + сетевой фон
            v = 0.04f * std::sin(2 * kPi * 15734.f * t) + 0.2f * std::sin(2 * kPi * 60.f * t) + 0.08f * std::sin(2 * kPi * 120.f * t);
            break;
        case Ambience::NeonHum:   // неоновый гул: 50 Гц с гармониками и «треском»
            v = 0.25f * std::sin(2 * kPi * 50.f * t) + 0.12f * std::sin(2 * kPi * 100.f * t) + 0.06f * std::sin(2 * kPi * 150.f * t);
            v *= 0.8f + 0.2f * std::sin(2 * kPi * 0.25f * t);
            if (std::abs(noise(rng)) > 0.9995f) v += noise(rng) * 0.4f;
            break;
        default: break;
        }
        b.data[i] = v;
    }
    const std::size_t xf = (std::size_t)(0.5f * sr);
    for (std::size_t i = 0; i < xf; ++i) {
        float w = (float)i / (float)xf;
        b.data[i] = b.data[i] * w + b.data[b.data.size() - xf + i] * (1.f - w);
    }
    b.data.resize(b.data.size() - xf);
    return b;
}

// ---------------------------------------------------------------------------
//  Микшер (аудиопоток). Без блокировок: команды приходят через MpscQueue.
// ---------------------------------------------------------------------------
void AudioEngine::mix(float* out, std::uint32_t frames) {
    while (auto c = cmds_.pop()) {
        if (c->type == Command::Play) {
            auto buf = std::atomic_load(&sfx_[(std::size_t)c->sfx]);
            if (buf && voices_.size() < 48) voices_.push_back({buf.get(), 0.0, c->p});   // лимит полифонии
        } else {
            ambNext_ = c->amb == Ambience::None ? nullptr : amb_[(std::size_t)c->amb].get();
            ambFadeStep_ = 1.f / std::max(1.f, c->fade * (float)sampleRate_ * 0.5f);
        }
    }
    const float master = enabled ? masterVolume.load() : 0.f, fx = effectsVolume.load(), ambVol = ambienceVolume.load();
    for (std::uint32_t f = 0; f < frames; ++f) {
        float l = 0, r = 0, rvIn = 0;
        for (auto& v : voices_) {
            std::size_t i = (std::size_t)v.pos;
            if (i + 1 >= v.buf->data.size()) continue;
            float frac = (float)(v.pos - (double)i);
            float s = (v.buf->data[i] * (1.f - frac) + v.buf->data[i + 1] * frac) * v.p.gain * fx;   // линейная интерполяция
            float ang = (v.p.pan + 1.f) * 0.25f * kPi;                                                   // equal-power панорама
            l += s * std::cos(ang) * (1.f - v.p.reverb * 0.5f);
            r += s * std::sin(ang) * (1.f - v.p.reverb * 0.5f);
            rvIn += s * v.p.reverb;
            v.pos += v.p.pitch;
        }
        // Эмбиент с кроссфейдом: затухание текущего -> переключение -> нарастание нового
        if (ambCur_ != ambNext_) {
            ambGain_ -= ambFadeStep_;
            if (ambGain_ <= 0.f) { ambGain_ = 0.f; ambCur_ = ambNext_; ambPos_ = 0; }
        } else if (ambGain_ < 1.f) ambGain_ = std::min(1.f, ambGain_ + ambFadeStep_);
        if (ambCur_ && !ambCur_->data.empty()) {
            float s = ambCur_->data[(std::size_t)ambPos_] * ambGain_ * ambVol;
            ambPos_ += 1.0; if (ambPos_ >= (double)ambCur_->data.size()) ambPos_ = 0;
            l += s; r += s;
            rvIn += s * 0.2f;
        }
        // Ревербератор Шрёдера (моно-вход, стерео-выход с декорреляцией)
        float rv = 0;
        for (auto& cmb : combs_) {
            float y = cmb.buf[cmb.idx];
            cmb.store = y * (1.f - cmb.damp) + cmb.store * cmb.damp;
            cmb.buf[cmb.idx] = rvIn * 0.3f + cmb.store * cmb.fb;
            cmb.idx = (cmb.idx + 1) % cmb.buf.size();
            rv += y;
        }
        for (auto& ap : allpass_) {
            float bufout = ap.buf[ap.idx];
            float y = -rv + bufout;
            ap.buf[ap.idx] = rv + bufout * 0.5f;
            ap.idx = (ap.idx + 1) % ap.buf.size();
            rv = y;
        }
        l += rv * 0.25f; r += rv * 0.22f;
        out[f * 2]     = std::tanh(l * master);   // мягкий лимитер
        out[f * 2 + 1] = std::tanh(r * master);
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return (std::size_t)v.pos + 1 >= v.buf->data.size(); }), voices_.end());
}

} // namespace ide
