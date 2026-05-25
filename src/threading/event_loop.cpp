// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file event_loop.cpp
// @brief I/O 事件循环（epoll/IOCP）的实现。

#include "corodb/threading/event_loop.h"

#include <algorithm>
#include <cstring>
#include <print>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace corodb {

    // ============================================================================
    //                          Channel 实现
    // ============================================================================
    // Channel 封装了文件描述符上的事件（读/写/错误/关闭）
    // 每个 Channel 对象负责监听一个 fd 的事件并分发到对应的回调函数

    /**
     * @brief Channel 构造函数
     * @param loop 所属的事件循环
     * @param fd 要监听的文件描述符
     */
    Channel::Channel(EventLoop* loop, socket_t fd) : loop_(loop), fd_(fd) {
    }

    /**
     * @brief Channel 析构函数
     *
     * 注意：不在此关闭 fd，fd 的生命周期由 Connection 管理
     */
    Channel::~Channel() {
    }

    /**
     * @brief 启用读事件监听
     *
     * 注册到 epoll/select，当 fd 有数据可读时触发回调
     */
    void Channel::enable_reading() {
        events_ = events_ | EventType::Read; // 添加读事件标志
        update();                            // 更新到事件循环
    }

    /**
     * @brief 禁用读事件监听
     */
    void Channel::disable_reading() {
        // 清除读事件标志（位操作：AND NOT）
        events_ = static_cast<EventType>(static_cast<uint32_t>(events_) & ~static_cast<uint32_t>(EventType::Read));
        update();
    }

    /**
     * @brief 启用写事件监听
     *
     * 当 fd 可写时触发回调，通常用于发送缓冲区有数据等待发送时
     */
    void Channel::enable_writing() {
        events_ = events_ | EventType::Write;
        update();
    }

    /**
     * @brief 禁用写事件监听
     */
    void Channel::disable_writing() {
        events_ = static_cast<EventType>(static_cast<uint32_t>(events_) & ~static_cast<uint32_t>(EventType::Write));
        update();
    }

    /**
     * @brief 禁用所有事件监听
     */
    void Channel::disable_all() {
        events_ = EventType::None;
        update();
    }

    /**
     * @brief 从事件循环中移除
     *
     * 连接关闭时调用，从 epoll/channels 中注销
     */
    void Channel::remove() {
        loop_->remove_channel(this);
        added_to_loop_ = false;
    }

    /**
     * @brief 更新到事件循环
     *
     * 内部函数，将 Channel 的事件状态同步到 epoll/select
     */
    void Channel::update() {
        added_to_loop_ = true;
        loop_->update_channel(this);
    }

    /**
     * @brief 处理已触发的事件
     *
     * 根据 revents_ 中的事件类型，调用对应的回调函数
     * 处理顺序：错误 → 关闭 → 读 → 写
     */
    void Channel::handle_event() {
        // 1. 处理错误事件（如 socket 错误）
        if (has_event(revents_, EventType::Error)) {
            if (error_callback_)
                error_callback_();
        }

        // 2. 处理关闭事件（对端关闭连接）
        if (has_event(revents_, EventType::Close)) {
            if (close_callback_)
                close_callback_();
            return; // 关闭后不再处理其他事件
        }

        // 3. 处理读事件（有数据可读）
        if (has_event(revents_, EventType::Read)) {
            if (read_callback_)
                read_callback_();
        }

        // 4. 处理写事件（可以发送数据）
        if (has_event(revents_, EventType::Write)) {
            if (write_callback_)
                write_callback_();
        }
    }

    // ============================================================================
    //                          EventLoop 实现
    // ============================================================================
    // EventLoop 是 Reactor 模式的核心，负责：
    // - I/O 多路复用（epoll/select/poll）
    // - 事件分发到各个 Channel
    // - 跨线程任务队列的执行

    /**
     * @brief EventLoop 构造函数
     *
     * 初始化 I/O 多路复用机制和唤醒机制
     * - Linux：使用 epoll + eventfd
     * - Windows：使用 WSAPoll + UDP socket pair
     */
    EventLoop::EventLoop() {
        // 记录创建线程的 ID，用于检查是否在 I/O 线程中执行
        thread_id_ = std::this_thread::get_id();

#ifndef _WIN32
        // ==== Linux: 使用 epoll ====
        // 创建 epoll 实例，EPOLL_CLOEXEC 程序 fork 后自动关闭
        epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epfd_ < 0) {
            throw std::runtime_error("epoll_create1 failed");
        }

        // 创建 eventfd 用于跨线程唤醒
        // 当其他线程向队列添加任务时，写入 eventfd 唤醒 epoll_wait
        wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0) {
            ::close(epfd_);
            throw std::runtime_error("eventfd failed");
        }

        // 为 wakeup_fd 创建 Channel，监听可读事件
        wakeup_channel_ = std::make_unique<Channel>(this, wakeup_fd_);
        wakeup_channel_->set_read_callback([this] { handle_wakeup_read(); });
        wakeup_channel_->enable_reading();
#else
        // ==== Windows: 使用 socket pair 模拟 eventfd ====
        // Windows 没有 eventfd，使用 UDP socket pair 实现唤醒机制

        // 创建 UDP socket 作为读端
        socket_t listener = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (listener == INVALID_SOCKET) {
            throw std::runtime_error("Failed to create wakeup socket");
        }

        // 绑定到本地地址，端口由系统分配
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
        addr.sin_port = 0;                             // 系统自动分配端口

        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(listener);
            throw std::runtime_error("Failed to bind wakeup socket");
        }

        // 获取系统分配的端口号
        socklen_t len = sizeof(addr);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
            close_socket(listener);
            throw std::runtime_error("Failed to get socket name");
        }

        wakeup_fds_[0] = listener; // 读端：用于接收唤醒消息

        // 创建写端 socket
        wakeup_fds_[1] = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (wakeup_fds_[1] == INVALID_SOCKET) {
            close_socket(listener);
            throw std::runtime_error("Failed to create wakeup write socket");
        }

        // 连接写端到读端，之后可以直接 send()
        if (::connect(wakeup_fds_[1], reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(wakeup_fds_[0]);
            close_socket(wakeup_fds_[1]);
            throw std::runtime_error("Failed to connect wakeup sockets");
        }

        // 设置读端为非阻塞
        set_nonblocking(wakeup_fds_[0]);

        // 为 wakeup socket 创建 Channel
        wakeup_channel_ = std::make_unique<Channel>(this, wakeup_fds_[0]);
        wakeup_channel_->set_read_callback([this] { handle_wakeup_read(); });
        wakeup_channel_->enable_reading();
#endif
    }

    /**
     * @brief EventLoop 析构函数
     *
     * 关闭唤醒机制和 I/O 多路复用资源
     */
    EventLoop::~EventLoop() {
        wakeup_channel_->disable_all(); // 停止监听唤醒事件
        wakeup_channel_->remove();      // 从 channels_ 中移除

#ifndef _WIN32
        ::close(wakeup_fd_); // 关闭 eventfd
        ::close(epfd_);      // 关闭 epoll
#else
        if (wakeup_fds_[0] != INVALID_SOCKET_VAL)
            close_socket(wakeup_fds_[0]);
        if (wakeup_fds_[1] != INVALID_SOCKET_VAL)
            close_socket(wakeup_fds_[1]);
#endif
    }

    // ============================================================================
    //                          事件循环主体
    // ============================================================================

    /**
     * @brief 运行事件循环
     *
     * 这是 EventLoop 的核心函数，执行以下循环：
     * 1. 等待 I/O 事件（epoll_wait / WSAPoll）
     * 2. 处理已触发的事件（调用 Channel::handle_event）
     * 3. 执行挂起的跨线程任务
     *
     * 调用 quit() 后退出循环
     */
    void EventLoop::loop() {
        looping_.store(true);                    // 标记进入循环状态
        quit_.store(false);                      // 重置退出标志
        thread_id_ = std::this_thread::get_id(); // 更新线程 ID

#ifdef _WIN32
        std::vector<WSAPOLLFD> pollfds; // Windows poll 数组
#else
        std::vector<epoll_event> events(64); // Linux epoll 事件数组
#endif

        // =========================================================================
        // 事件循环主循环（Reactor 模式核心）
        // =========================================================================
        //
        // Reactor 模式：一个线程 + 一个 I/O 多路复用器（epoll/WSAPoll）
        // 管理多个 socket，所有 I/O 事件在此线程内串行处理。
        //
        // 每轮循环执行三个步骤：
        //   1. poll：阻塞等待 I/O 事件（超时 10ms，防止饿死 pending functor）
        //   2. dispatch：将就绪事件分发给对应 Channel 的回调
        //   3. functor：执行其他线程投递的跨线程任务
        //
        // 跨线程安全：非 I/O 线程通过 run_in_loop() 将任务放入 pending_functors_
        // 队列，由事件循环线程在每轮 poll 后取出执行。
        //
        // 为什么超时 10ms 而非无限等待？
        //   其他线程可能投递了 pending functor，若不设超时，这些任务永远得不到
        //   执行机会（因为 poll 在没有 I/O 事件时永久阻塞）。
        // ==== 主循环 ====
        while (!quit_.load()) {
#ifdef _WIN32
            // ==== Windows: 使用 WSAPoll ====
            // 重建 poll 数组（每次循环都需要，因为 channels_ 可能变化）
            pollfds.clear();
            for (const auto& [fd, channel]: channels_) {
                WSAPOLLFD pfd{};
                pfd.fd = static_cast<SOCKET>(fd);
                pfd.events = 0;
                // 根据 Channel 的事件标志设置 poll 事件
                if (has_event(channel->events(), EventType::Read)) {
                    pfd.events |= POLLRDNORM; // 正常数据可读
                }
                if (has_event(channel->events(), EventType::Write)) {
                    pfd.events |= POLLWRNORM; // 可写
                }
                pollfds.push_back(pfd);
            }

            // 没有 Channel 时短暂睡眠，避免忩等待
            if (pollfds.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                do_pending_functors(); // 执行挂起任务
                continue;
            }

            // 调用 WSAPoll，超时时间 10ms
            int n = ::WSAPoll(pollfds.data(), static_cast<ULONG>(pollfds.size()), 10);
            if (n > 0) {
                // 处理已触发的事件
                for (const auto& pfd: pollfds) {
                    if (pfd.revents == 0)
                        continue; // 无事件

                    auto it = channels_.find(static_cast<socket_t>(pfd.fd));
                    if (it == channels_.end())
                        continue; // Channel 已移除

                    // 转换 poll 事件为内部事件类型
                    EventType revents = EventType::None;
                    if (pfd.revents & (POLLRDNORM | POLLRDBAND)) {
                        revents = revents | EventType::Read; // 可读
                    }
                    if (pfd.revents & POLLWRNORM) {
                        revents = revents | EventType::Write; // 可写
                    }
                    if (pfd.revents & POLLERR) {
                        revents = revents | EventType::Error; // 错误
                    }
                    if (pfd.revents & (POLLHUP | POLLNVAL)) {
                        revents = revents | EventType::Close; // 挂断/无效
                    }

                    // 设置已触发事件并处理
                    it->second->set_revents(revents);
                    it->second->handle_event();
                }
            }
#else
            // ==== Linux: 使用 epoll ====
            // epoll_wait 阻塞等待事件，超时时间 10ms
            int n = ::epoll_wait(epfd_, events.data(), static_cast<int>(events.size()), 10);
            if (n > 0) {
                // 处理已触发的事件
                for (int i = 0; i < n; ++i) {
                    // epoll 直接返回 Channel 指针（在 update_channel 时存储）
                    auto* channel = static_cast<Channel*>(events[i].data.ptr);

                    // 转换 epoll 事件为内部事件类型
                    EventType revents = EventType::None;
                    if (events[i].events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
                        revents = revents | EventType::Read; // 可读
                    }
                    if (events[i].events & EPOLLOUT) {
                        revents = revents | EventType::Write; // 可写
                    }
                    if (events[i].events & EPOLLERR) {
                        revents = revents | EventType::Error; // 错误
                    }
                    if (events[i].events & EPOLLHUP) {
                        revents = revents | EventType::Close; // 挂断
                    }

                    channel->set_revents(revents);
                    channel->handle_event();
                }

                // 动态扩容：如果返回的事件数等于数组大小，可能有更多事件
                if (static_cast<std::size_t>(n) == events.size()) {
                    events.resize(events.size() * 2);
                }
            }
#endif

            // 执行跨线程投递的任务
            do_pending_functors();
        }

        looping_.store(false); // 标记已退出循环
    }

    // ============================================================================
    //                          事件循环控制
    // ============================================================================

    /**
     * @brief 退出事件循环
     *
     * 设置退出标志，如果不在 I/O 线程中则唤醒循环
     */
    void EventLoop::quit() {
        quit_.store(true);
        // 如果不在 I/O 线程，需要唤醒 epoll_wait 以便立即检查退出标志
        if (!is_in_loop_thread()) {
            wakeup();
        }
    }

    // ============================================================================
    //                          跨线程任务投递
    // ============================================================================

    /**
     * @brief 在 I/O 线程中执行回调
     * @param cb 要执行的回调函数
     *
     * 如果已在 I/O 线程，直接执行
     * 否则将任务添加到队列，等待 I/O 线程执行
     */
    void EventLoop::run_in_loop(Functor cb) {
        if (is_in_loop_thread()) {
            cb(); // 已在 I/O 线程，直接执行
        } else {
            queue_in_loop(std::move(cb)); // 跨线程，加入队列
        }
    }

    /**
     * @brief 将任务加入执行队列
     * @param cb 要执行的回调函数
     *
     * 线程安全，使用互斥锁保护队列
     */
    void EventLoop::queue_in_loop(Functor cb) {
        {
            std::lock_guard lock(mutex_); // 加锁保护任务队列
            pending_functors_.push_back(std::move(cb));
        }

        // 唤醒条件：
        // 1. 不在 I/O 线程（需唤醒 epoll_wait）
        // 2. 正在执行 pending functors（下次循环才会检查队列）
        if (!is_in_loop_thread() || calling_pending_functors_.load()) {
            wakeup();
        }
    }

    /**
     * @brief 唤醒事件循环
     *
     * 向 wakeup fd 写入数据，触发 epoll_wait 返回
     */
    void EventLoop::wakeup() {
#ifdef _WIN32
        char buf = 'w';                     // 唤醒标记
        ::send(wakeup_fds_[1], &buf, 1, 0); // 写入 UDP socket
#else
        uint64_t one = 1;
        [[maybe_unused]] auto n = ::write(wakeup_fd_, &one, sizeof(one)); // 写入 eventfd
#endif
    }

    /**
     * @brief 处理唤醒事件
     *
     * 读取并丢弃 wakeup fd 中的数据，清空缓冲区
     */
    void EventLoop::handle_wakeup_read() {
#ifdef _WIN32
        // 读取并丢弃所有唤醒消息
        char buf[64];
        while (::recv(wakeup_fds_[0], buf, sizeof(buf), 0) > 0) {
        }
#else
        // 读取 eventfd 计数器
        uint64_t one;
        [[maybe_unused]] auto n = ::read(wakeup_fd_, &one, sizeof(one));
#endif
    }

    // ============================================================================
    //                          Channel 管理
    // ============================================================================

    /**
     * @brief 更新 Channel 的事件注册
     * @param channel 要更新的 Channel
     *
     * 将 Channel 的事件标志同步到底层 I/O 多路复用机制
     */
    void EventLoop::update_channel(Channel* channel) {
#ifdef _WIN32
        // Windows: WSAPoll 每次调用时重新构建数组，只需更新 channels_ map
        channels_[channel->fd()] = channel;
#else
        // Linux: 更新 epoll 注册
        epoll_event ev{};
        ev.data.ptr = channel; // 存储 Channel 指针，事件触发时直接使用

        // 根据 Channel 的事件标志设置 epoll 事件
        if (has_event(channel->events(), EventType::Read)) {
            ev.events |= EPOLLIN | EPOLLPRI; // 数据可读 + 紧急数据
        }
        if (has_event(channel->events(), EventType::Write)) {
            ev.events |= EPOLLOUT; // 可写
        }
        ev.events |= EPOLLET; // 边缘触发模式（高性能）

        auto it = channels_.find(channel->fd());
        if (it == channels_.end()) {
            // 新增 Channel
            if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, channel->fd(), &ev) < 0) {
                throw std::runtime_error("epoll_ctl ADD failed");
            }
            channels_[channel->fd()] = channel;
        } else {
            // 修改已有 Channel
            if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, channel->fd(), &ev) < 0) {
                throw std::runtime_error("epoll_ctl MOD failed");
            }
        }
#endif
    }

    /**
     * @brief 从事件循环中移除 Channel
     * @param channel 要移除的 Channel
     */
    void EventLoop::remove_channel(Channel* channel) {
#ifndef _WIN32
        // Linux: 从 epoll 中注销
        if (channels_.count(channel->fd())) {
            ::epoll_ctl(epfd_, EPOLL_CTL_DEL, channel->fd(), nullptr);
        }
#endif
        // 从 channels_ map 中移除
        channels_.erase(channel->fd());
    }

    /**
     * @brief 执行挂起的任务
     *
     * 使用 swap 技巧减少锁持有时间：
     * 1. 加锁并交换出任务列表
     * 2. 释放锁后执行任务
     */
    void EventLoop::do_pending_functors() {
        std::vector<Functor> functors;
        calling_pending_functors_.store(true); // 标记正在执行

        {
            std::lock_guard lock(mutex_);
            functors.swap(pending_functors_); // 交换（原子操作，快速）
        }
        // 锁已释放，其他线程可以继续添加任务

        // 执行所有任务
        for (const auto& f: functors) {
            f();
        }

        calling_pending_functors_.store(false); // 清除标记
    }

    // ============================================================================
    //                          EventLoopThread 实现
    // ============================================================================
    // EventLoopThread 封装了一个专门运行 EventLoop 的线程
    // 用于创建 Sub-Reactor

    EventLoopThread::EventLoopThread() = default;

    EventLoopThread::~EventLoopThread() {
        stop();
    }

    /**
     * @brief 启动 EventLoop 线程
     * @return 新创建的 EventLoop 指针
     *
     * 创建新线程并在其中运行 EventLoop
     * 使用条件变量等待 EventLoop 创建完成
     */
    EventLoop* EventLoopThread::start() {
        // 创建并启动线程
        thread_ = std::thread(&EventLoopThread::thread_func, this);

        // 等待 EventLoop 创建完成
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return started_; }); // 等待 started_ 为 true

        return loop_;
    }

    /**
     * @brief 停止 EventLoop 线程
     */
    void EventLoopThread::stop() {
        if (loop_) {
            loop_->quit(); // 通知 EventLoop 退出
        }
        if (thread_.joinable()) {
            thread_.join(); // 等待线程结束
        }
    }

    /**
     * @brief 线程入口函数
     *
     * 在新线程中创建 EventLoop 并运行
     */
    void EventLoopThread::thread_func() {
        EventLoop loop; // 在堆栈上创建 EventLoop

        {
            std::lock_guard lock(mutex_);
            loop_ = &loop;   // 保存指针
            started_ = true; // 标记已启动
        }
        cv_.notify_one(); // 通知等待的 start() 函数

        loop.loop(); // 运行事件循环（阻塞直到 quit()）

        // EventLoop 退出后清理
        std::lock_guard lock(mutex_);
        loop_ = nullptr;
    }

    // ============================================================================
    //                          EventLoopThreadPool 实现
    // ============================================================================
    // I/O 线程池，管理多个 Sub-Reactor

    /**
     * @brief 构造函数
     * @param base_loop Main Reactor 的 EventLoop
     * @param num_threads I/O 线程数量
     */
    EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, std::size_t num_threads)
        : base_loop_(base_loop), num_threads_(num_threads) {
    }

    EventLoopThreadPool::~EventLoopThreadPool() {
        stop();
    }

    /**
     * @brief 启动线程池
     *
     * 创建指定数量的 I/O 线程，每个线程运行一个 EventLoop
     */
    void EventLoopThreadPool::start() {
        started_ = true;

        for (std::size_t i = 0; i < num_threads_; ++i) {
            // 创建 EventLoopThread
            auto thread = std::make_unique<EventLoopThread>();
            // 启动并获取 EventLoop 指针
            loops_.push_back(thread->start());
            threads_.push_back(std::move(thread));
        }
    }

    /**
     * @brief 停止线程池
     */
    void EventLoopThreadPool::stop() {
        for (auto& thread: threads_) {
            thread->stop();
        }
        threads_.clear();
        loops_.clear();
        started_ = false;
    }

    /**
     * @brief 获取下一个 EventLoop（Round-Robin 负载均衡）
     * @return EventLoop 指针
     *
     * 使用原子计数器实现无锁的轮询调度
     */
    EventLoop* EventLoopThreadPool::get_next_loop() {
        if (loops_.empty()) {
            // 没有 I/O 线程，使用 Main Reactor
            return base_loop_;
        }

        // Round-Robin：依次返回每个 EventLoop
        std::size_t idx = next_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
        return loops_[idx];
    }

} // namespace corodb
