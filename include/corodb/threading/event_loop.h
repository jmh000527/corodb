// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file event_loop.h @brief Reactor 事件循环接口。 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "corodb/net/port.h"

namespace corodb {

    // 前向声明
    class Channel;

    /** @brief 事件类型。 */
    enum class EventType : uint32_t {
        None = 0,
        Read = 1 << 0,  ///< 可读事件
        Write = 1 << 1, ///< 可写事件
        Error = 1 << 2, ///< 错误事件
        Close = 1 << 3, ///< 关闭事件
    };

    // 位运算支持
    inline EventType operator|(EventType a, EventType b) {
        return static_cast<EventType>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline EventType operator&(EventType a, EventType b) {
        return static_cast<EventType>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    inline bool has_event(EventType events, EventType test) {
        return (static_cast<uint32_t>(events) & static_cast<uint32_t>(test)) != 0;
    }

    /** @brief I/O 通道，封装文件描述符及其事件回调。 */
    class Channel {
    public:
        using EventCallback = std::function<void()>;

        /** @brief 构造 Channel。 @param loop 所属的事件循环。 @param fd 文件描述符。 */
        Channel(class EventLoop* loop, socket_t fd);

        ~Channel();

        /** @brief 禁止复制。 */
        Channel(const Channel&) = delete;
        Channel& operator=(const Channel&) = delete;

        /** @brief 获取文件描述符。 */
        [[nodiscard]] socket_t fd() const noexcept {
            return fd_;
        }

        /** @brief 获取所属事件循环。 */
        [[nodiscard]] EventLoop* owner_loop() const noexcept {
            return loop_;
        }

        /** @brief 设置可读回调。 */
        void set_read_callback(EventCallback cb) {
            read_callback_ = std::move(cb);
        }

        /** @brief 设置可写回调。 */
        void set_write_callback(EventCallback cb) {
            write_callback_ = std::move(cb);
        }

        /** @brief 设置错误回调。 */
        void set_error_callback(EventCallback cb) {
            error_callback_ = std::move(cb);
        }

        /** @brief 设置关闭回调。 */
        void set_close_callback(EventCallback cb) {
            close_callback_ = std::move(cb);
        }

        /** @brief 启用读事件监听。 */
        void enable_reading();

        /** @brief 禁用读事件监听。 */
        void disable_reading();

        /** @brief 启用写事件监听。 */
        void enable_writing();

        /** @brief 禁用写事件监听。 */
        void disable_writing();

        /** @brief 禁用所有事件。 */
        void disable_all();

        /** @brief 从事件循环中移除。 */
        void remove();

        /** @brief 是否在监听写事件。 */
        [[nodiscard]] bool is_writing() const noexcept {
            return has_event(events_, EventType::Write);
        }

        /** @brief 是否在监听读事件。 */
        [[nodiscard]] bool is_reading() const noexcept {
            return has_event(events_, EventType::Read);
        }

        /** @brief 获取当前监听的事件。 */
        [[nodiscard]] EventType events() const noexcept {
            return events_;
        }

        /** @brief 设置活动事件（由 EventLoop 调用）。 */
        void set_revents(EventType revents) {
            revents_ = revents;
        }

        /** @brief 处理事件（由 EventLoop 调用）。 */
        void handle_event();

    private:
        void update();

        EventLoop* loop_;                      ///< 所属事件循环
        socket_t fd_;                          ///< 文件描述符
        EventType events_{ EventType::None };  ///< 监听的事件
        EventType revents_{ EventType::None }; ///< 实际发生的事件
        bool added_to_loop_{ false };          ///< 是否已添加到循环

        EventCallback read_callback_;
        EventCallback write_callback_;
        EventCallback error_callback_;
        EventCallback close_callback_;
    };

    /** @brief 事件循环（Sub-Reactor），封装 epoll/poll 提供 I/O 多路复用。 */
    class EventLoop {
    public:
        using Functor = std::function<void()>;

        EventLoop();
        ~EventLoop();

        // 禁止复制和移动
        EventLoop(const EventLoop&) = delete;
        EventLoop& operator=(const EventLoop&) = delete;

        /** @brief 启动事件循环，阻塞直到 quit() 被调用。 */
        void loop();

        /** @brief 停止事件循环，线程安全。 */
        void quit();

        /** @brief 在事件循环线程中执行回调，若已在循环线程则立即执行。 @param cb 要执行的回调。 */
        void run_in_loop(Functor cb);

        /** @brief 将回调加入待执行队列。 @param cb 要执行的回调。 */
        void queue_in_loop(Functor cb);

        /** @brief 唤醒事件循环（跨线程通知）。 */
        void wakeup();

        /** @brief 更新 Channel 的事件监听。 @param channel 要更新的 Channel。 */
        void update_channel(Channel* channel);

        /** @brief 移除 Channel。 @param channel 要移除的 Channel。 */
        void remove_channel(Channel* channel);

        /** @brief 检查是否在事件循环线程中。 */
        [[nodiscard]] bool is_in_loop_thread() const noexcept {
            return thread_id_ == std::this_thread::get_id();
        }

        /** @brief 检查事件循环是否正在运行。 */
        [[nodiscard]] bool is_looping() const noexcept {
            return looping_.load();
        }

    private:
        void do_pending_functors();
        void handle_wakeup_read();

        std::atomic<bool> looping_{ false };
        std::atomic<bool> quit_{ false };
        std::thread::id thread_id_;

#ifndef _WIN32
        int epfd_{ -1 }; ///< epoll 文件描述符
#endif

        // 唤醒机制
#ifdef _WIN32
        socket_t wakeup_fds_[2]{ INVALID_SOCKET_VAL, INVALID_SOCKET_VAL };
#else
        int wakeup_fd_{ -1 }; ///< eventfd
#endif
        std::unique_ptr<Channel> wakeup_channel_;

        // Channel 管理
        std::unordered_map<socket_t, Channel*> channels_;

        // 跨线程回调队列
        std::mutex mutex_;
        std::vector<Functor> pending_functors_;
        std::atomic<bool> calling_pending_functors_{ false };
    };

    /** @brief 在独立线程中运行 EventLoop 的包装类。 */
    class EventLoopThread {
    public:
        EventLoopThread();
        ~EventLoopThread();

        // 禁止复制
        EventLoopThread(const EventLoopThread&) = delete;
        EventLoopThread& operator=(const EventLoopThread&) = delete;

        /** @brief 启动线程并返回 EventLoop 指针。 @return EventLoop 指针。 */
        EventLoop* start();

        /** @brief 停止线程。 */
        void stop();

    private:
        void thread_func();

        EventLoop* loop_{ nullptr };
        std::thread thread_;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool started_{ false };
    };

    /** @brief 管理多个 EventLoopThread 的线程池（Sub-Reactor Pool）。 */
    class EventLoopThreadPool {
    public:
        /** @brief 构造线程池。 @param base_loop 主事件循环。 @param num_threads I/O 线程数量，0 表示使用主循环。 */
        explicit EventLoopThreadPool(EventLoop* base_loop, std::size_t num_threads = 0);

        ~EventLoopThreadPool();

        // 禁止复制
        EventLoopThreadPool(const EventLoopThreadPool&) = delete;
        EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

        /** @brief 启动所有 I/O 线程。 */
        void start();

        /** @brief 停止所有 I/O 线程。 */
        void stop();

        /** @brief 获取下一个事件循环（Round-Robin）。 @return EventLoop 指针。 */
        EventLoop* get_next_loop();

        /** @brief 获取线程数量。 */
        [[nodiscard]] std::size_t size() const noexcept {
            return num_threads_;
        }

    private:
        EventLoop* base_loop_;
        std::size_t num_threads_;
        std::vector<std::unique_ptr<EventLoopThread>> threads_;
        std::vector<EventLoop*> loops_;
        std::atomic<std::size_t> next_{ 0 };
        bool started_{ false };
    };

} // namespace corodb
