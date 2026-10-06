#pragma once
// ============================================================================
//  TelemetryPanel — окно «Телеметрия»: графики загрузки CPU / RAM / GPU
//  в реальном времени на ImPlot.
//
//  Сбор данных идёт в отдельном потоке (std::jthread), чтобы чтение /proc
//  или вызовы NVML никогда не задерживали кадр. Результаты складываются в
//  кольцевые буферы под мьютексом; render() только копирует и рисует.
// ============================================================================
#include "platform/SystemInfo.hpp"

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ide {

// Кольцевой буфер точек (t, value) для ImPlot::PlotLine со смещением.
struct ScrollingSeries {
    explicit ScrollingSeries(int capacity = 600) : max(capacity) { xs.reserve(max); ys.reserve(max); }
    void add(float x, float y) {
        if ((int)xs.size() < max) { xs.push_back(x); ys.push_back(y); }
        else { xs[offset] = x; ys[offset] = y; offset = (offset + 1) % max; }
    }
    int max, offset = 0;
    std::vector<float> xs, ys;
};

class TelemetryPanel {
public:
    TelemetryPanel();
    ~TelemetryPanel();

    void render(bool* open);                       // окно с графиками
    void renderStatusBarWidget();                  // компактный индикатор в статус-строке

    float historySeconds = 60.f;                   // ширина окна графика
    float sampleInterval = 0.5f;                   // период опроса, сек
    [[nodiscard]] SystemSample last() const { std::scoped_lock lk(mtx_); return last_; }

private:
    void worker(std::stop_token st);

    mutable std::mutex mtx_;
    SystemSample       last_;
    ScrollingSeries    cpu_, ram_, gpu_, rss_;
    float              t_ = 0.f;
    std::string        gpuSource_;
    std::jthread       thread_;                    // объявлен последним — стартует после инициализации полей
};

} // namespace ide
