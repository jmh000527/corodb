// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file thread_pool.h @brief 通用线程池接口。 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace corodb {

    /** @brief 线程安全的工作线程池，支持任意可调用对象并返回 std::future。 */
    class ThreadPool {
    public:
        /** @brief 构造线程池。 @param num_threads 工作线程数量，0 表示使用 CPU 核心数。 */
        explicit ThreadPool(std::size_t num_threads = 0);

        /** @brief 析构函数，停止所有工作线程。 */
        ~ThreadPool();

        // 禁止复制和移动
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;
        ThreadPool(ThreadPool&&) = delete;
        ThreadPool& operator=(ThreadPool&&) = delete;

        /** @brief 提交任务到线程池。 @return std::future 用于获取任务返回值。 @throw std::runtime_error
         * 如果线程池已停止。 */
        template<typename F, typename... Args>
        auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

        /** @brief 提交无返回值任务，避免 packaged_task/future 开销。 @throw std::runtime_error 如果线程池已停止。 */
        void post(std::function<void()> f);

        /** @brief 设置最大队列长度（0 = 无限制，默认）。超过后提交任务会阻塞。 */
        void set_max_queue_size(std::size_t sz) {
            std::unique_lock lock(mutex_);
            max_queue_size_ = sz;
        }

        /** @brief 获取工作线程数量。 */
        [[nodiscard]] std::size_t size() const noexcept {
            return workers_.size();
        }

        /** @brief 获取待处理任务数量。 */
        [[nodiscard]] std::size_t pending_tasks() const;

        /** @brief 检查线程池是否已停止。 */
        [[nodiscard]] bool stopped() const noexcept {
            return stop_.load(std::memory_order_acquire);
        }

    private:
        /// 工作线程主循环。
        void worker_loop();

        std::vector<std::thread> workers_;        ///< 工作线程
        std::queue<std::function<void()>> tasks_; ///< 任务队列
        mutable std::mutex mutex_;                ///< 互斥锁
        std::condition_variable cv_;              ///< 条件变量（有新任务或停止）
        std::condition_variable cv_producer_;     ///< 条件变量（队列有空位）
        std::atomic<bool> stop_{ false };         ///< 停止标志
        std::size_t max_queue_size_{ 0 };         ///< 最大队列长度（0 = 无限制）
    };

    // ============================================================================
    // 模板实现
    // ============================================================================

    template<typename F, typename... Args>
    auto ThreadPool::submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using ReturnType = std::invoke_result_t<F, Args...>;

        // 将任务包装成 packaged_task
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<ReturnType> future = task->get_future();

        {
            std::unique_lock lock(mutex_);

            if (stop_.load(std::memory_order_acquire)) {
                throw std::runtime_error("ThreadPool: submit on stopped pool");
            }

            // 队列满时阻塞（背压）。
            if (max_queue_size_ > 0) {
                cv_producer_.wait(lock, [this] {
                    return stop_.load(std::memory_order_acquire) || tasks_.size() < max_queue_size_;
                });
                if (stop_.load(std::memory_order_acquire)) {
                    throw std::runtime_error("ThreadPool: submit on stopped pool");
                }
            }

            tasks_.emplace([task]() { (*task)(); });
        }

        cv_.notify_one();
        return future;
    }

} // namespace corodb
