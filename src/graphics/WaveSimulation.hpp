// =============================================================================
//  WaveSimulation.hpp — численное решение двумерного волнового уравнения
//      ∂²h/∂t² = c²·∇²h − γ·∂h/∂t
//  явной схемой «чехарда» (leapfrog) на регулярной сетке. Поле высот
//  загружается в R32F-текстуру и используется шейдером аквариума для
//  преломления (смещения UV) и бликов ряби.
// =============================================================================
#pragma once
#include <vector>

namespace ide {

class WaveSimulation {
public:
    WaveSimulation(int w = 256, int h = 160) { resize(w, h); }
    void resize(int w, int h);
    // Возмущение в нормализованных координатах [0..1]
    void disturb(float u, float v, float strength = 1.f, float radius = 0.02f);
    void step(float dt);
    [[nodiscard]] int width() const { return w_; }
    [[nodiscard]] int height() const { return h_; }
    [[nodiscard]] const float* data() const { return cur_.data(); }
    [[nodiscard]] float at(int x, int y) const { return cur_[(std::size_t)(y * w_ + x)]; }
    [[nodiscard]] float energy() const;

    float waveSpeed = 0.45f;   // c·dt/dx < 1/√2 — условие устойчивости Куранта
    float damping   = 0.985f;

private:
    int w_ = 0, h_ = 0;
    std::vector<float> prev_, cur_, next_;
    float accum_ = 0.f;
};

} // namespace ide
