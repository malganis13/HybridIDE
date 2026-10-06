#pragma once
// ============================================================================
//  SystemInfo — кроссплатформенный сбор телеметрии: загрузка CPU, RAM, GPU.
//
//  Источники данных:
//    * Windows : GetSystemTimes (CPU), GlobalMemoryStatusEx (RAM),
//                NVML (nvml.dll, если установлен драйвер NVIDIA) для GPU.
//    * Linux   : /proc/stat, /proc/meminfo, NVML (libnvidia-ml.so.1) либо
//                /sys/class/drm/card*/device/gpu_busy_percent (AMD/Intel).
//    * macOS   : host_statistics (CPU), host_statistics64 + sysctl (RAM);
//                GPU недоступен без приватных API — помечается как «нет данных».
//
//  NVML загружается динамически (dlopen/LoadLibrary), поэтому сборка не
//  зависит от CUDA SDK, а отсутствие драйвера не ломает приложение.
//  Все методы дешёвые; sample() рассчитан на вызов ~2–4 раза в секунду.
// ============================================================================
#include <cstdint>
#include <optional>
#include <string>

namespace ide {

struct SystemSample {
    float                cpuPercent = 0.f;            // 0..100, все ядра
    std::uint64_t        ramUsedBytes = 0, ramTotalBytes = 0;
    std::optional<float> gpuPercent;                  // nullopt — нет источника
    std::optional<float> gpuMemPercent;
    std::uint64_t        processRssBytes = 0;          // память самой IDE
};

class SystemInfo {
public:
    SystemInfo();
    ~SystemInfo();
    SystemInfo(const SystemInfo&) = delete;
    SystemInfo& operator=(const SystemInfo&) = delete;

    // Снять новый замер. CPU считается как дельта между двумя вызовами,
    // поэтому первый вызов всегда возвращает 0 %.
    SystemSample sample();

    [[nodiscard]] const std::string& gpuSource() const { return gpuSource_; }  // "NVML", "sysfs", ""
    [[nodiscard]] static unsigned cpuCount();

private:
    std::optional<float> readGpu(std::optional<float>* memPercent);

    std::uint64_t prevIdle_ = 0, prevTotal_ = 0;
    std::string   gpuSource_;
    void*         nvmlLib_ = nullptr;    // дескриптор библиотеки NVML
    void*         nvmlDevice_ = nullptr; // nvmlDevice_t первой видеокарты
    void*         fnUtil_ = nullptr;     // nvmlDeviceGetUtilizationRates
    void*         fnShutdown_ = nullptr; // nvmlShutdown
    std::string   sysfsPath_;            // путь к gpu_busy_percent
};

} // namespace ide
