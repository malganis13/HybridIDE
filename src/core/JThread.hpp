#pragma once
// ============================================================================
//  JThread.hpp — std::jthread / std::stop_token с запасной реализацией.
//
//  libc++ от Apple (Xcode 15–16) до сих пор не включает std::jthread без
//  -fexperimental-library. Если стандартная библиотека его предоставляет
//  (__cpp_lib_jthread), используем std-версии; иначе — минимальный, но
//  совместимый по используемому интерфейсу полифил:
//    stop_token::stop_requested(), jthread(F) где F принимает stop_token
//    первым аргументом (или ничего), request_stop(), join(), joinable(),
//    автоматический request_stop()+join() в деструкторе.
// ============================================================================
#include <version>
#include <thread>

#if defined(__cpp_lib_jthread) && !defined(IDE_FORCE_JTHREAD_POLYFILL)
#include <stop_token>
namespace ide {
using std::jthread;
using std::stop_token;
} // namespace ide
#else
#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

namespace ide {

class stop_token {
public:
    stop_token() = default;
    explicit stop_token(std::shared_ptr<std::atomic<bool>> s) : s_(std::move(s)) {}
    [[nodiscard]] bool stop_requested() const noexcept { return s_ && s_->load(std::memory_order_acquire); }
    [[nodiscard]] bool stop_possible() const noexcept { return s_ != nullptr; }
private:
    std::shared_ptr<std::atomic<bool>> s_;
};

class jthread {
public:
    jthread() noexcept = default;

    template <class F, class... Args,
              class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, jthread>>>
    explicit jthread(F&& f, Args&&... args) : stop_(std::make_shared<std::atomic<bool>>(false)) {
        stop_token tok(stop_);
        if constexpr (std::is_invocable_v<std::decay_t<F>, stop_token, std::decay_t<Args>...>)
            t_ = std::thread(std::forward<F>(f), tok, std::forward<Args>(args)...);
        else
            t_ = std::thread(std::forward<F>(f), std::forward<Args>(args)...);
    }

    jthread(jthread&&) noexcept = default;
    jthread& operator=(jthread&& o) noexcept {
        if (this != &o) { reset(); t_ = std::move(o.t_); stop_ = std::move(o.stop_); }
        return *this;
    }
    jthread(const jthread&) = delete;
    jthread& operator=(const jthread&) = delete;
    ~jthread() { reset(); }

    bool request_stop() noexcept {
        if (!stop_) return false;
        return !stop_->exchange(true, std::memory_order_acq_rel);
    }
    [[nodiscard]] bool joinable() const noexcept { return t_.joinable(); }
    void join() { t_.join(); }
    void detach() { t_.detach(); }
    [[nodiscard]] stop_token get_stop_token() const noexcept { return stop_token(stop_); }

private:
    void reset() {
        if (t_.joinable()) { request_stop(); t_.join(); }
    }
    std::thread                        t_;
    std::shared_ptr<std::atomic<bool>> stop_;
};

} // namespace ide
#endif
