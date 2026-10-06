// =============================================================================
//  FishSimulation.cpp
// =============================================================================
#include "graphics/FishSimulation.hpp"

#include <algorithm>
#include <cmath>

namespace ide {

float Vec2::len() const { return std::sqrt(x * x + y * y); }
Vec2 Vec2::normalized() const { float l = len(); return l > 1e-5f ? Vec2{x / l, y / l} : Vec2{}; }
Vec2 Vec2::limited(float m) const { float l = len(); return l > m ? *this * (m / l) : *this; }

float FishSimulation::rand01() {
    // xorshift32 — детерминированный и быстрый
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return (float)(rng_ & 0xFFFFFF) / (float)0xFFFFFF;
}

void FishSimulation::reset(int count, float width, float height, std::uint32_t seed) {
    w_ = width; h_ = height; rng_ = seed ? seed : 1;
    fish_.clear();
    for (int i = 0; i < count; ++i) {
        Fish f;
        f.pos = {rand01() * w_, rand01() * h_};
        float a = rand01() * 6.2831853f;
        f.vel = {std::cos(a) * 40.f, std::sin(a) * 15.f};
        f.size = 0.7f + rand01() * 0.8f;
        f.phase = rand01() * 6.28f;
        f.species = (int)(rand01() * 3.f) % 3;
        f.hue = rand01();
        fish_.push_back(f);
    }
}

void FishSimulation::scare(Vec2 at, float strength) {
    threats_.push_back({at, strength * params.scareStrength, 0.f});
    if (threats_.size() > 16) threats_.erase(threats_.begin());
}

void FishSimulation::rebuildGrid() {
    cell_ = std::max(20.f, params.neighborRadius);
    gw_ = std::max(1, (int)(w_ / cell_) + 1);
    gh_ = std::max(1, (int)(h_ / cell_) + 1);
    grid_.assign((std::size_t)(gw_ * gh_), {});
    for (int i = 0; i < (int)fish_.size(); ++i) {
        int cx = std::clamp((int)(fish_[(std::size_t)i].pos.x / cell_), 0, gw_ - 1);
        int cy = std::clamp((int)(fish_[(std::size_t)i].pos.y / cell_), 0, gh_ - 1);
        grid_[(std::size_t)(cy * gw_ + cx)].push_back(i);
    }
}

void FishSimulation::update(float dt) {
    dt = std::min(dt, 0.05f);
    rebuildGrid();
    const float nr2 = params.neighborRadius * params.neighborRadius;
    const float sr2 = params.separationRadius * params.separationRadius;
    std::vector<Vec2> accel(fish_.size());

    for (std::size_t i = 0; i < fish_.size(); ++i) {
        Fish& f = fish_[i];
        Vec2 sep, ali, coh;
        int n = 0;
        int cx = std::clamp((int)(f.pos.x / cell_), 0, gw_ - 1), cy = std::clamp((int)(f.pos.y / cell_), 0, gh_ - 1);
        for (int gy = std::max(0, cy - 1); gy <= std::min(gh_ - 1, cy + 1); ++gy)
            for (int gx = std::max(0, cx - 1); gx <= std::min(gw_ - 1, cx + 1); ++gx)
                for (int j : grid_[(std::size_t)(gy * gw_ + gx)]) {
                    if ((std::size_t)j == i) continue;
                    const Fish& o = fish_[(std::size_t)j];
                    if (o.species != f.species) continue;   // косяки по видам
                    Vec2 d = f.pos - o.pos;
                    float d2 = d.x * d.x + d.y * d.y;
                    if (d2 > nr2 || d2 < 1e-6f) continue;
                    if (d2 < sr2) sep += d * (1.f / d2);     // отталкивание ~ 1/r
                    ali += o.vel;
                    coh += o.pos;
                    ++n;
                }
        Vec2 force;
        const float maxV = params.maxSpeed * (1.f + f.fear * (params.panicSpeed / params.maxSpeed - 1.f));
        auto steer = [&](Vec2 desired) { return (desired.normalized() * maxV - f.vel).limited(params.maxForce); };
        if (n > 0) {
            if (sep.len() > 0) force += steer(sep) * params.separation;
            force += steer(ali * (1.f / (float)n)) * params.alignment;
            force += steer(coh * (1.f / (float)n) - f.pos) * params.cohesion;
        }
        // Блуждание: медленно меняющийся случайный вектор
        float wa = std::sin(f.phase * 0.13f + (float)i) * 3.14159f;
        force += Vec2{std::cos(wa), std::sin(wa) * 0.4f} * (params.maxForce * params.wander * 0.3f);
        // Границы: мягкое возвращение (рыбы предпочитают среднюю глубину)
        const float margin = 80.f;
        if (f.pos.x < margin) force.x += params.maxForce * (1.f - f.pos.x / margin);
        if (f.pos.x > w_ - margin) force.x -= params.maxForce * (1.f - (w_ - f.pos.x) / margin);
        if (f.pos.y < margin) force.y += params.maxForce * (1.f - f.pos.y / margin);
        if (f.pos.y > h_ - margin) force.y -= params.maxForce * (1.f - (h_ - f.pos.y) / margin);
        // Испуг: импульс от точек опасности, сила ~ (1 - r/R)², затухает с возрастом угрозы
        for (auto& t : threats_) {
            Vec2 d = f.pos - t.pos;
            float dist = d.len();
            if (dist < params.scareRadius) {
                float k = 1.f - dist / params.scareRadius;
                float s = k * k * t.strength * std::exp(-t.age * 2.f);
                force += d.normalized() * (params.maxForce * 12.f * s);
                f.fear = std::min(1.f, f.fear + s * 2.f);
            }
        }
        accel[i] = force;
    }
    for (std::size_t i = 0; i < fish_.size(); ++i) {
        Fish& f = fish_[i];
        const float maxV = params.maxSpeed * (1.f + f.fear * (params.panicSpeed / params.maxSpeed - 1.f));
        f.vel = (f.vel + accel[i] * dt).limited(maxV);
        // Рыбы плавают преимущественно горизонтально
        f.vel.y *= 1.f - 0.5f * dt;
        f.pos += f.vel * dt;
        f.pos.x = std::clamp(f.pos.x, -50.f, w_ + 50.f);
        f.pos.y = std::clamp(f.pos.y, -50.f, h_ + 50.f);
        f.phase += dt * (4.f + f.vel.len() * 0.08f);   // испуганная рыба машет хвостом чаще
        f.fear = std::max(0.f, f.fear - dt * 0.6f);
    }
    for (auto& t : threats_) t.age += dt;
    threats_.erase(std::remove_if(threats_.begin(), threats_.end(), [](auto& t) { return t.age > 2.5f; }), threats_.end());
}

static void hsv(float h, float s, float v, float& r, float& g, float& b) {
    h = std::fmod(h, 1.f) * 6.f;
    int i = (int)h; float f = h - (float)i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                     case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; }
}

void FishSimulation::buildVertices(std::vector<FishVertex>& out, float time, float alpha) const {
    out.clear();
    out.reserve(fish_.size() * 60);
    for (const Fish& f : fish_) {
        Vec2 dir = f.vel.normalized();
        if (dir.len() < 0.5f) dir = {1, 0};
        Vec2 nrm{-dir.y, dir.x};
        float L = 22.f * f.size, H = (f.species == 2 ? 11.f : 6.5f) * f.size;
        float r, g, b;
        float hue = f.species == 0 ? 0.05f + f.hue * 0.1f : f.species == 1 ? 0.5f + f.hue * 0.1f : 0.12f + f.hue * 0.05f;
        hsv(hue, 0.75f, 0.95f, r, g, b);
        auto P = [&](float along, float side) { Vec2 p = f.pos + dir * along + nrm * side; return p; };
        auto V = [&](Vec2 p, float rr, float gg, float bb, float aa) { out.push_back({p.x, p.y, rr, gg, bb, aa * alpha}); };
        // Тело — веер треугольников по эллипсу с волнообразным изгибом
        const int seg = 10;
        Vec2 c = P(0, 0);
        for (int k = 0; k < seg; ++k) {
            float a0 = (float)k / seg * 6.2831853f, a1 = (float)(k + 1) / seg * 6.2831853f;
            auto pt = [&](float a) {
                float x = std::cos(a) * L * 0.5f;
                float bend = std::sin(f.phase - x * 0.08f) * 1.5f * f.size;
                return P(x, std::sin(a) * H + bend * (0.5f - x / L));
            };
            float shade = 0.75f + 0.25f * std::sin(a0);   // светлая спинка, тёмное брюшко
            V(c, r, g, b, 1.f);
            V(pt(a0), r * shade, g * shade, b * shade, 1.f);
            V(pt(a1), r * shade, g * shade, b * shade, 1.f);
        }
        // Хвост — колеблющийся треугольник
        float sw = std::sin(f.phase) * H * 0.9f;
        Vec2 t0 = P(-L * 0.45f, 0), t1 = P(-L * 0.85f, H * 1.1f + sw), t2 = P(-L * 0.85f, -H * 1.1f + sw);
        V(t0, r * 0.8f, g * 0.8f, b * 0.8f, 0.95f); V(t1, r, g, b, 0.7f); V(t2, r, g, b, 0.7f);
        // Спинной плавник
        Vec2 d0 = P(L * 0.1f, H * 0.7f), d1 = P(-L * 0.2f, H * 1.6f), d2 = P(-L * 0.25f, H * 0.6f);
        V(d0, r, g, b, 0.8f); V(d1, r, g, b, 0.5f); V(d2, r, g, b, 0.8f);
        // Глаз
        Vec2 e = P(L * 0.3f, H * 0.25f);
        float es = 1.6f * f.size;
        V(e + Vec2{-es, -es}, 0.05f, 0.05f, 0.1f, 1.f); V(e + Vec2{es, -es}, 0.05f, 0.05f, 0.1f, 1.f); V(e + Vec2{0, es}, 0.05f, 0.05f, 0.1f, 1.f);
    }
    (void)time;
}

} // namespace ide
