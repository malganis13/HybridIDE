// =============================================================================
//  EventQueue.hpp — lock-free очереди и межпоточная шина событий
//  Используется всеми асинхронными подсистемами (LSP, DAP, Git, GitHub, PTY)
//  для передачи результатов в UI-поток без блокировок.
// =============================================================================
#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace ide {

// -----------------------------------------------------------------------------
//  MpscQueue — неблокирующая очередь «много производителей / один потребитель»
//  (алгоритм Дмитрия Вьюкова). Производители — фоновые потоки, потребитель —
//  UI-поток, который вычитывает события один раз за кадр.
// -----------------------------------------------------------------------------
template <typename T>
class MpscQueue {
    struct Node {
        std::atomic<Node*> next{nullptr};
        std::optional<T>   value;
    };

public:
    MpscQueue() {
        auto* stub = new Node();
        head_.store(stub, std::memory_order_relaxed);
        tail_ = stub;
    }
    ~MpscQueue() {
        while (pop()) {}
        delete tail_;
    }
    MpscQueue(const MpscQueue&)            = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // Потокобезопасная вставка: один атомарный exchange, без мьютексов
    void push(T value) {
        auto* node = new Node();
        node->value.emplace(std::move(value));
        Node* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    // Извлечение — вызывается ТОЛЬКО из потока-потребителя
    std::optional<T> pop() {
        Node* tail = tail_;
        Node* next = tail->next.load(std::memory_order_acquire);
        if (!next) return std::nullopt;
        std::optional<T> out = std::move(next->value);
        next->value.reset();
        tail_ = next;
        delete tail;
        return out;
    }

    [[nodiscard]] bool empty() const {
        return tail_->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<Node*> head_;
    Node*              tail_;   // принадлежит потребителю
};

// -----------------------------------------------------------------------------
//  MainThreadDispatcher — очередь замыканий, выполняемых в UI-потоке.
//  Фоновая задача делает post([=]{ ...изменить состояние UI... }).
// -----------------------------------------------------------------------------
class MainThreadDispatcher {
public:
    using Task = std::function<void()>;

    static MainThreadDispatcher& instance() {
        static MainThreadDispatcher d;
        return d;
    }

    void post(Task t) { queue_.push(std::move(t)); }

    // Выполнить не более maxTasks задач за кадр, чтобы не «подвесить» рендер
    std::size_t drain(std::size_t maxTasks = 512) {
        std::size_t n = 0;
        while (n < maxTasks) {
            auto t = queue_.pop();
            if (!t) break;
            try {
                (*t)();
            } catch (...) {
                // Исключение из задачи не должно ронять главный цикл
            }
            ++n;
        }
        return n;
    }

private:
    MpscQueue<Task> queue_;
};

inline void postToMain(std::function<void()> fn) {
    MainThreadDispatcher::instance().post(std::move(fn));
}

} // namespace ide
