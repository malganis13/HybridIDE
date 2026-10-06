// =============================================================================
//  WaveSimulation.cpp
// =============================================================================
#include "graphics/WaveSimulation.hpp"

#include <algorithm>
#include <cmath>

namespace ide {

void WaveSimulation::resize(int w, int h) {
    w_ = std::max(8, w); h_ = std::max(8, h);
    std::size_t n = (std::size_t)(w_ * h_);
    prev_.assign(n, 0.f); cur_.assign(n, 0.f); next_.assign(n, 0.f);
}

void WaveSimulation::disturb(float u, float v, float strength, float radius) {
    int cx = (int)(u * (float)w_), cy = (int)(v * (float)h_);
    int r = std::max(1, (int)(radius * (float)w_));
    for (int y = std::max(1, cy - r); y < std::min(h_ - 1, cy + r + 1); ++y)
        for (int x = std::max(1, cx - r); x < std::min(w_ - 1, cx + r + 1); ++x) {
            float d = std::hypot((float)(x - cx), (float)(y - cy)) / (float)r;
            if (d > 1.f) continue;
            // Гладкий «колокол» (cos²) — без высокочастотного шума на фронте волны
            float k = std::cos(d * 1.5707963f);
            cur_[(std::size_t)(y * w_ + x)] -= strength * k * k;
        }
}

void WaveSimulation::step(float dt) {
    // Фиксированный шаг 1/120 с — независимость физики от FPS
    accum_ += std::min(dt, 0.1f);
    const float fixed = 1.f / 120.f;
    const float c2 = waveSpeed * waveSpeed;
    while (accum_ >= fixed) {
        accum_ -= fixed;
        for (int y = 1; y < h_ - 1; ++y) {
            const std::size_t row = (std::size_t)(y * w_);
            for (int x = 1; x < w_ - 1; ++x) {
                std::size_t i = row + (std::size_t)x;
                float lap = cur_[i - 1] + cur_[i + 1] + cur_[i - (std::size_t)w_] + cur_[i + (std::size_t)w_] - 4.f * cur_[i];
                next_[i] = (2.f * cur_[i] - prev_[i] + c2 * lap) * damping;
            }
        }
        std::swap(prev_, cur_);
        std::swap(cur_, next_);
    }
}

float WaveSimulation::energy() const {
    float e = 0;
    for (float v : cur_) e += v * v;
    return e;
}

} // namespace ide
