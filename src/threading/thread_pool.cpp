// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file thread_pool.cpp
// @brief 固定大小线程池的实现。

#include "corodb/threading/thread_pool.h"

namespace corodb {

    // ============================================================================
    //                          线程池构造与析构
    // ============================================================================

    /**
     * @brief 构造线程池
     * @param num_threads 工作线程数量，0表示自动检测CPU核心数
     */
    ThreadPool::ThreadPool(std::size_t num_threads) {
        // 如果未指定线程数，使用硬件并发数
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            // 某些系统可能返回0，使用安全的默认值
            if (num_threads == 0) {
                num_threads = 4;
            }
        }

        // 预分配线程容器空间，避免动态扩容
        workers_.reserve(num_threads);

        // 创建工作线程，每个线程运行worker_loop函数
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back(&ThreadPool::worker_loop, this);
        }
    }

    /**
     * @brief 析构线程池，等待所有任务完成
     */
    ThreadPool::~ThreadPool() {
        // 设置停止标志，使用release语义确保之前的写入对其他线程可见
        stop_.store(true, std::memory_order_release);

        // 唤醒所有正在等待的工作线程，让它们检查停止标志
        cv_.notify_all();

        // 等待所有工作线程结束（优雅关闭）
        for (auto& worker: workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    // ============================================================================
    //                          任务管理
    // ============================================================================

    /**
     * @brief 获取待处理任务数量
     * @return 队列中等待执行的任务数
     */
    std::size_t ThreadPool::pending_tasks() const {
        std::unique_lock lock(mutex_); // 加锁保护任务队列
        return tasks_.size();
    }

    void ThreadPool::post(std::function<void()> f) {
        {
            std::unique_lock lock(mutex_);
            if (stop_.load(std::memory_order_acquire)) {
                throw std::runtime_error("ThreadPool: post on stopped pool");
            }
            // Block if the queue is full (backpressure).
            if (max_queue_size_ > 0) {
                cv_producer_.wait(lock, [this] {
                    return stop_.load(std::memory_order_acquire) || tasks_.size() < max_queue_size_;
                });
                if (stop_.load(std::memory_order_acquire)) {
                    throw std::runtime_error("ThreadPool: post on stopped pool");
                }
            }
            tasks_.emplace(std::move(f));
        }
        cv_.notify_one();
    }

    // ============================================================================
    //                          工作线程循环
    // ============================================================================

    /**
     * @brief 工作线程主循环
     *
     * 每个工作线程不断执行以下步骤：
     * 1. 等待任务到达或停止信号
     * 2. 从队列取出任务
     * 3. 执行任务
     * 4. 重复以上步骤直到线程池关闭
     */
    void ThreadPool::worker_loop() {
        for (;;) {                      // 无限循环，直到线程池关闭
            std::function<void()> task; // 待执行的任务

            {
                std::unique_lock lock(mutex_);

                // 条件变量等待：
                // - 收到停止信号，或
                // - 任务队列非空
                // 这避免了忙等待，节省CPU资源
                cv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire) || !tasks_.empty(); });

                // 检查退出条件：停止标志已设置且队列为空
                // 注意：即使收到停止信号，也要先完成队列中剩余的任务
                if (stop_.load(std::memory_order_acquire) && tasks_.empty()) {
                    return; // 退出工作线程
                }

                // 从队列头部取出任务（FIFO顺序）
                task = std::move(tasks_.front());
                tasks_.pop();
                // Notify producers that queue space is available.
                if (max_queue_size_ > 0) {
                    cv_producer_.notify_one();
                }
            }
            // 锁已释放，其他线程可以继续提交或获取任务

            // 执行任务（在锁外执行，允许最大并发）
            task();
        }
    }

} // namespace corodb
