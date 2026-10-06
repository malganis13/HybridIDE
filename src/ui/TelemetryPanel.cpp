// ============================================================================
//  TelemetryPanel.cpp
// ============================================================================
#include "ui/TelemetryPanel.hpp"
#include "ui/Localization.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ide {

TelemetryPanel::TelemetryPanel()
    : thread_([this](std::stop_token st) { worker(st); }) {}

TelemetryPanel::~TelemetryPanel() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void TelemetryPanel::worker(std::stop_token st) {
    SystemInfo info;                               // NVML и т.п. живут только в этом потоке
    {
        std::scoped_lock lk(mtx_);
        gpuSource_ = info.gpuSource();
    }
    auto start = std::chrono::steady_clock::now();
    while (!st.stop_requested()) {
        SystemSample s = info.sample();
        float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        {
            std::scoped_lock lk(mtx_);
            last_ = s;
            t_ = t;
            cpu_.add(t, s.cpuPercent);
            ram_.add(t, s.ramTotalBytes ? 100.f * float(s.ramUsedBytes) / float(s.ramTotalBytes) : 0.f);
            gpu_.add(t, s.gpuPercent.value_or(0.f));
            rss_.add(t, float(s.processRssBytes) / (1024.f * 1024.f));
        }
        // Спим маленькими шагами, чтобы быстро реагировать на остановку.
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(sampleInterval * 1000));
        while (!st.stop_requested() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void TelemetryPanel::render(bool* open) {
    auto& L = Localization::instance();
    if (!ImGui::Begin(L.tr("telemetry.title"), open)) { ImGui::End(); return; }

    // Копируем данные под мьютексом — рисование может занимать время.
    ScrollingSeries cpu, ram, gpu, rss;
    SystemSample last; float t; std::string gpuSrc;
    {
        std::scoped_lock lk(mtx_);
        cpu = cpu_; ram = ram_; gpu = gpu_; rss = rss_; last = last_; t = t_; gpuSrc = gpuSource_;
    }

    ImGui::Text("CPU: %.1f%%  (%u)", last.cpuPercent, SystemInfo::cpuCount());
    ImGui::SameLine(0, 24);
    ImGui::Text("RAM: %.2f / %.2f GiB", last.ramUsedBytes / 1073741824.0, last.ramTotalBytes / 1073741824.0);
    ImGui::SameLine(0, 24);
    if (last.gpuPercent) ImGui::Text("GPU: %.0f%% [%s]", *last.gpuPercent, gpuSrc.c_str());
    else ImGui::TextDisabled("GPU: %s", L.tr("telemetry.no_gpu"));
    ImGui::SameLine(0, 24);
    ImGui::Text("%s: %.0f MiB", L.tr("telemetry.ide_memory"), last.processRssBytes / 1048576.0);

    ImGui::SetNextItemWidth(180);
    ImGui::SliderFloat(L.tr("telemetry.history"), &historySeconds, 10.f, 300.f, "%.0f s");

    auto plot = [&](const char* id, const char* label, ScrollingSeries& s, float yMax, const char* fmt) {
        if (ImPlot::BeginPlot(id, ImVec2(-1, 150), ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
            ImPlot::SetupAxisLimits(ImAxis_X1, t - historySeconds, t, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, yMax, yMax > 0 ? ImGuiCond_Always : ImGuiCond_Once);
            ImPlot::SetupAxisFormat(ImAxis_Y1, fmt);
            if (!s.xs.empty()) {
                // Кольцевой буфер: Offset указывает на самую старую точку.
                ImPlotSpec spec;
                spec.Offset = s.offset;
                spec.Stride = sizeof(float);
                spec.FillAlpha = 0.25f;
                ImPlot::PlotShaded(label, s.xs.data(), s.ys.data(), (int)s.xs.size(), 0.0, spec);
                spec.FillAlpha = 1.f;
                ImPlot::PlotLine(label, s.xs.data(), s.ys.data(), (int)s.xs.size(), spec);
            }
            ImPlot::EndPlot();
        }
    };
    plot("##cpu", "CPU %", cpu, 100.f, "%.0f%%");
    plot("##ram", "RAM %", ram, 100.f, "%.0f%%");
    if (last.gpuPercent) plot("##gpu", "GPU %", gpu, 100.f, "%.0f%%");
    float rssMax = 0; for (float v : rss.ys) rssMax = std::max(rssMax, v);
    plot("##rss", "IDE MiB", rss, rssMax * 1.25f + 16.f, "%.0f");
    ImGui::End();
}

void TelemetryPanel::renderStatusBarWidget() {
    SystemSample s = last();
    char buf[96];
    if (s.gpuPercent)
        std::snprintf(buf, sizeof buf, "CPU %2.0f%%  RAM %2.0f%%  GPU %2.0f%%", s.cpuPercent,
                      s.ramTotalBytes ? 100.0 * s.ramUsedBytes / s.ramTotalBytes : 0.0, *s.gpuPercent);
    else
        std::snprintf(buf, sizeof buf, "CPU %2.0f%%  RAM %2.0f%%", s.cpuPercent,
                      s.ramTotalBytes ? 100.0 * s.ramUsedBytes / s.ramTotalBytes : 0.0);
    ImGui::TextUnformatted(buf);
}

} // namespace ide
