// =============================================================================
//  Async.hpp — C++20 корутины и пул потоков на std::jthread
//  Task<T> — «ленивая» корутина; co_await switchToPool() переносит выполнение
//  в фоновый пул, co_await switchToMain() — обратно в UI-поток.
// =============================================================================
#pragma once
#include "core/JThread.hpp"
#include "core/EventQueue.hpp"

#include <algorithm>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ide {

// -----------------------------------------------------------------------------
//  ThreadPool — фиксированный пул рабочих потоков с кооперативной отменой
// -----------------------------------------------------------------------------
class ThreadPool {
public:
    explicit ThreadPool(unsigned threads = std::max(2u, std::thread::hardware_concurrency() / 2)) {
        workers_.reserve(threads);
        for (unsigned i = 0; i < threads; ++i) {
            workers_.emplace_back([this](ide::stop_token st) { workerLoop(st); });
        }
    }
    ~ThreadPool() {
        {
            std::lock_guard lk(mtx_);
            stopping_ = true;
        }
        for (auto& w : workers_) w.request_stop();
        cv_.notify_all();
        // std::jthread сам вызывает join() в деструкторе
    }

    static ThreadPool& global() {
        static ThreadPool pool;
        return pool;
    }

    void submit(std::function<void()> job) {
        {
            std::lock_guard lk(mtx_);
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

private:
    void workerLoop(ide::stop_token st) {
        while (!st.stop_requested()) {
            std::function<void()> job;
            {
                std::unique_lock lk(mtx_);
                cv_.wait(lk, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_ || st.stop_requested()) return;
                if (jobs_.empty()) continue;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            try { job(); } catch (...) { /* ошибки фоновых задач изолированы */ }
        }
    }

    std::mutex                        mtx_;
    std::condition_variable           cv_;
    bool                              stopping_ = false;
    std::deque<std::function<void()>> jobs_;
    std::vector<ide::jthread>         workers_;
};

// -----------------------------------------------------------------------------
//  Fire-and-forget корутина (используется для асинхронных сценариев UI)
// -----------------------------------------------------------------------------
struct FireAndForget {
    struct promise_type {
        FireAndForget       get_return_object() noexcept { return {}; }
        std::suspend_never  initial_suspend() noexcept { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept {}
    };
};

// Awaitable: продолжить выполнение корутины в пуле потоков
inline auto switchToPool() {
    struct Awaiter {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) const {
            ThreadPool::global().submit([h] { h.resume(); });
        }
        void await_resume() const noexcept {}
    };
    return Awaiter{};
}

// Awaitable: продолжить выполнение корутины в UI-потоке (на следующем кадре)
inline auto switchToMain() {
    struct Awaiter {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) const {
            postToMain([h] { h.resume(); });
        }
        void await_resume() const noexcept {}
    };
    return Awaiter{};
}

// Удобный хелпер: выполнить work() в фоне, затем done(result) в UI-потоке
template <typename Work, typename Done>
void runAsync(Work work, Done done) {
    ThreadPool::global().submit([work = std::move(work), done = std::move(done)]() mutable {
        auto result = work();
        postToMain([done = std::move(done), result = std::move(result)]() mutable {
            done(std::move(result));
        });
    });
}

} // namespace ide
