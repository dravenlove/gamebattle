#pragma once

// A fixed-size thread pool: one mutex-protected task queue, N worker threads,
// and std::future results. Deliberately simple so every line can be explained
// in lesson 17.

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace practice {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count) {
        if (thread_count == 0) {
            throw std::invalid_argument("thread pool needs at least one thread");
        }
        workers_.reserve(thread_count);
        for (std::size_t index = 0; index < thread_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        // std::jthread joins in its destructor: queued tasks finish first.
    }

    template <typename Function>
    auto submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        // packaged_task is move-only but std::function needs a copyable target,
        // so the task lives behind a shared_ptr.
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
        auto future = task->get_future();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("submit on a stopping thread pool");
            }
            tasks_.emplace([task] { (*task)(); });
        }
        ready_.notify_one();
        return future;
    }

    std::size_t size() const noexcept { return workers_.size(); }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();  // run outside the lock so other workers can take tasks
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_{false};
    std::vector<std::jthread> workers_;  // last member: threads start after everything above exists
};

} // namespace practice
