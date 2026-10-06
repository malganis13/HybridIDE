// =============================================================================
//  FishSimulation.hpp — косяк рыб на алгоритме Boids (К. Рейнольдс, 1987):
//  разделение, выравнивание, сплочённость + границы аквариума, блуждание и
//  реакция испуга: клик в редакторе создаёт «точку опасности», от которой
//  рыбы получают импульс отталкивания и ускоряются. Пространственная сетка
//  (uniform grid) даёт O(n) поиск соседей. Модуль не зависит от OpenGL —
//  генерирует вершины треугольников для ShaderEngine.
// =============================================================================
#pragma once
#include <cstdint>
#include <vector>

namespace ide {

struct Vec2 {
    float x = 0, y = 0;
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    [[nodiscard]] float len() const;
    [[nodiscard]] Vec2 normalized() const;
    [[nodiscard]] Vec2 limited(float m) const;
};

struct Fish {
    Vec2  pos, vel;
    float size   = 1.f;      // масштаб особи
    float phase  = 0.f;      // фаза взмаха хвоста
    float fear   = 0.f;      // 0..1, затухает со временем
    float hue    = 0.f;      // цвет особи
    int   species = 0;       // 0 — тропическая, 1 — неон, 2 — скалярия
};

struct FishVertex { float x, y; float r, g, b, a; };

struct BoidParams {
    float separation = 1.6f, alignment = 1.0f, cohesion = 0.8f;
    float neighborRadius = 70.f, separationRadius = 26.f;
    float maxSpeed = 70.f, maxForce = 120.f, panicSpeed = 260.f;
    float scareRadius = 220.f, scareStrength = 1.0f;   // «коэффициент испуга» из настроек темы
    float wander = 0.4f;
};

class FishSimulation {
public:
    void reset(int count, float width, float height, std::uint32_t seed = 7);
    void resize(float width, float height) { w_ = width; h_ = height; }
    void scare(Vec2 at, float strength = 1.f);
    void update(float dt);
    // Генерация геометрии (треугольники) в пикселях окна
    void buildVertices(std::vector<FishVertex>& out, float time, float alpha = 0.85f) const;

    [[nodiscard]] const std::vector<Fish>& fish() const { return fish_; }
    BoidParams params;

private:
    struct Threat { Vec2 pos; float strength, age; };
    void rebuildGrid();
    std::vector<Fish>   fish_;
    std::vector<Threat> threats_;
    float w_ = 1280, h_ = 720;
    // Пространственная сетка
    std::vector<std::vector<int>> grid_;
    int gw_ = 1, gh_ = 1;
    float cell_ = 70.f;
    std::uint32_t rng_ = 1;
    float rand01();
};

} // namespace ide
