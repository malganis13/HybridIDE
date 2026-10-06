// ============================================================================
//  SystemInfo.cpp — реализация сбора телеметрии (см. заголовок).
// ============================================================================
#include "platform/SystemInfo.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#elif defined(__APPLE__)
#  include <dlfcn.h>
#  include <mach/mach.h>
#  include <sys/sysctl.h>
#else
#  include <dlfcn.h>
#  include <unistd.h>
#endif

namespace ide {

// ---------------------------------------------------------------------------
//  Минимальное подмножество NVML ABI. Объявляем сами, чтобы не тянуть nvml.h:
//  сигнатуры стабильны с 2012 года.
// ---------------------------------------------------------------------------
namespace {
struct NvmlUtilization { unsigned int gpu; unsigned int memory; };
using NvmlInitFn     = int (*)();
using NvmlShutdownFn = int (*)();
using NvmlGetHandleFn = int (*)(unsigned int, void**);
using NvmlGetUtilFn  = int (*)(void*, NvmlUtilization*);

void* openLibrary(const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(name));
#else
    return dlopen(name, RTLD_LAZY | RTLD_LOCAL);
#endif
}
void* symbol(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}
void closeLibrary(void* lib) {
    if (!lib) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}
} // namespace

SystemInfo::SystemInfo() {
    // --- 1. Пытаемся NVML (NVIDIA) -----------------------------------------
#if defined(_WIN32)
    const char* names[] = {"nvml.dll", "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll"};
#elif defined(__APPLE__)
    const char* names[] = {"libnvidia-ml.dylib"};
#else
    const char* names[] = {"libnvidia-ml.so.1", "libnvidia-ml.so"};
#endif
    for (const char* n : names) {
        if ((nvmlLib_ = openLibrary(n))) break;
    }
    if (nvmlLib_) {
        auto init = reinterpret_cast<NvmlInitFn>(symbol(nvmlLib_, "nvmlInit_v2"));
        auto getHandle = reinterpret_cast<NvmlGetHandleFn>(symbol(nvmlLib_, "nvmlDeviceGetHandleByIndex_v2"));
        fnUtil_ = symbol(nvmlLib_, "nvmlDeviceGetUtilizationRates");
        fnShutdown_ = symbol(nvmlLib_, "nvmlShutdown");
        if (init && getHandle && fnUtil_ && init() == 0 && getHandle(0, &nvmlDevice_) == 0) {
            gpuSource_ = "NVML";
        } else {
            closeLibrary(nvmlLib_);
            nvmlLib_ = nullptr; nvmlDevice_ = nullptr; fnUtil_ = nullptr; fnShutdown_ = nullptr;
        }
    }
#if defined(__linux__)
    // --- 2. Фолбэк для AMD/Intel: amdgpu/i915 экспортируют загрузку в sysfs ---
    if (gpuSource_.empty()) {
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
            auto p = e.path() / "device" / "gpu_busy_percent";
            if (std::filesystem::exists(p, ec)) { sysfsPath_ = p.string(); gpuSource_ = "sysfs"; break; }
        }
    }
#endif
}

SystemInfo::~SystemInfo() {
    if (nvmlLib_) {
        if (fnShutdown_) reinterpret_cast<NvmlShutdownFn>(fnShutdown_)();
        closeLibrary(nvmlLib_);
    }
}

unsigned SystemInfo::cpuCount() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

std::optional<float> SystemInfo::readGpu(std::optional<float>* memPercent) {
    if (nvmlDevice_ && fnUtil_) {
        NvmlUtilization u{};
        if (reinterpret_cast<NvmlGetUtilFn>(fnUtil_)(nvmlDevice_, &u) == 0) {
            if (memPercent) *memPercent = static_cast<float>(u.memory);
            return static_cast<float>(u.gpu);
        }
        return std::nullopt;
    }
    if (!sysfsPath_.empty()) {
        std::ifstream f(sysfsPath_);
        int v = 0;
        if (f >> v) return static_cast<float>(v);
    }
    return std::nullopt;
}

SystemSample SystemInfo::sample() {
    SystemSample s;
    std::uint64_t idle = 0, total = 0;

#if defined(_WIN32)
    // ---------------- Windows ----------------
    FILETIME fIdle, fKernel, fUser;
    if (GetSystemTimes(&fIdle, &fKernel, &fUser)) {
        auto toU64 = [](FILETIME f) { return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        idle = toU64(fIdle);
        total = toU64(fKernel) + toU64(fUser);   // kernel уже включает idle
    }
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.ramTotalBytes = ms.ullTotalPhys;
        s.ramUsedBytes = ms.ullTotalPhys - ms.ullAvailPhys;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) s.processRssBytes = pmc.WorkingSetSize;
#elif defined(__APPLE__)
    // ---------------- macOS ----------------
    host_cpu_load_info_data_t cpu{};
    mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &cnt) == KERN_SUCCESS) {
        for (int i = 0; i < CPU_STATE_MAX; ++i) total += cpu.cpu_ticks[i];
        idle = cpu.cpu_ticks[CPU_STATE_IDLE];
    }
    std::uint64_t memTotal = 0; size_t len = sizeof(memTotal);
    sysctlbyname("hw.memsize", &memTotal, &len, nullptr, 0);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t vcnt = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &vcnt) == KERN_SUCCESS) {
        std::uint64_t page = static_cast<std::uint64_t>(vm_kernel_page_size);
        std::uint64_t freeB = (vm.free_count + vm.inactive_count) * page;
        s.ramTotalBytes = memTotal;
        s.ramUsedBytes = memTotal > freeB ? memTotal - freeB : 0;
    }
    mach_task_basic_info_data_t ti{};
    mach_msg_type_number_t tcnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&ti), &tcnt) == KERN_SUCCESS)
        s.processRssBytes = ti.resident_size;
#else
    // ---------------- Linux ----------------
    {
        std::ifstream f("/proc/stat");
        std::string cpu;
        std::uint64_t v[10]{};
        if (f >> cpu) {
            for (auto& x : v) f >> x;
            // user nice system idle iowait irq softirq steal guest guest_nice
            idle = v[3] + v[4];
            for (int i = 0; i < 8; ++i) total += v[i];   // guest уже учтён в user
        }
    }
    {
        std::ifstream f("/proc/meminfo");
        std::string key, unit;
        std::uint64_t val = 0, memTotal = 0, memAvail = 0;
        while (f >> key >> val >> unit) {
            if (key == "MemTotal:") memTotal = val * 1024;
            else if (key == "MemAvailable:") memAvail = val * 1024;
        }
        s.ramTotalBytes = memTotal;
        s.ramUsedBytes = memTotal > memAvail ? memTotal - memAvail : 0;
    }
    {
        std::ifstream f("/proc/self/statm");
        std::uint64_t size = 0, rss = 0;
        if (f >> size >> rss) s.processRssBytes = rss * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    }
#endif

    if (prevTotal_ != 0 && total > prevTotal_) {
        const double dt = double(total - prevTotal_);
        const double di = double(idle - prevIdle_);
        s.cpuPercent = static_cast<float>(100.0 * (1.0 - di / dt));
        if (s.cpuPercent < 0) s.cpuPercent = 0;
        if (s.cpuPercent > 100) s.cpuPercent = 100;
    }
    prevIdle_ = idle; prevTotal_ = total;

    s.gpuPercent = readGpu(&s.gpuMemPercent);
    return s;
}

} // namespace ide
