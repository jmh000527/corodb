// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file buffer_pool.cpp
// @brief 基于 Clock 替换算法的缓冲池管理器的实现。

#include "corodb/storage/buffer_pool.h"
#include "corodb/storage/storage_engine_common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace corodb {

    // ============================================================================
    //                          缓冲池初始化与销毁
    // ============================================================================

    /**
     * @brief 缓冲池构造函数
     * @param page_size 页面大小（字节），通常为4KB或8KB
     * @param capacity_pages 缓冲池容量（页面数），决定内存占用
     */
    BufferPool::BufferPool(std::size_t page_size, std::size_t capacity_pages)
        : page_size_(page_size), capacity_(capacity_pages) {
        // 预分配所有页面帧，避免运行时动态分配
        frames_.reserve(capacity_);

        for (size_t i = 0; i < capacity_; ++i) {
            // 创建帧对象
            frames_.push_back(std::make_shared<Frame>());
            // 为每个帧分配页面数据缓冲区
            frames_.back()->data.resize(page_size_);
            // 将帧索引添加到空闲列表（初始时所有帧都空闲）
            free_list_.push_back(i);
        }
    }

    /**
     * @brief 缓冲池析构函数
     *
     * 确保所有脏页在销毁前刷新到磁盘，防止数据丢失
     */
    BufferPool::~BufferPool() {
        flush_all();
    }

    /**
     * @brief 获取当前缓存的页面数量
     * @return 页表中的映射条目数
     */
    size_t BufferPool::size() const {
        std::shared_lock lock(latch_); // 共享锁，允许并发读取
        return page_table_.size();
    }

    // ============================================================================
    //                          磁盘I/O操作
    // ============================================================================

    /**
     * @brief 从磁盘加载页面到指定的内存帧
     * @param id 页面标识符，包含文件路径和页面索引
     * @param frame_id 目标帧在frames_数组中的索引
     *
     * 调用时机：当pin()发现页面不在缓冲池中时调用
     * 前置条件：调用者已获取该帧的独占访问权（通过pin_count或其他机制）
     */
    void BufferPool::load_to_frame(const PageId& id, FrameId frame_id) {
        auto& frame = *frames_[frame_id];

        // 以二进制模式打开页面文件
        std::ifstream ifs(id.file, std::ios::binary);
        if (!ifs) {
            // 文件不存在或无法打开
            // 对于已存在的页面读取，这是错误情况
            throw std::runtime_error("Failed to open page file: " + id.file);
        }

        // 计算页面在文件中的偏移量 = 页面索引 × 页面大小
        ifs.seekg(static_cast<std::streamoff>(id.page_idx) * static_cast<std::streamoff>(page_size_), std::ios::beg);
        if (!ifs) {
            throw std::runtime_error("Seek failed for page file: " + id.file);
        }

        // 读取整个页面到帧的数据缓冲区
        ifs.read(frame.data.data(), static_cast<std::streamsize>(page_size_));

        // 处理部分读取的情况（可能发生在文件末尾）
        if (ifs.gcount() != static_cast<std::streamsize>(page_size_)) {
            // 将未读取的部分填充为0（简化处理）
            std::fill(frame.data.begin() + ifs.gcount(), frame.data.end(), '\0');
        }

        // 注：LSM SSTable 数据页为原始 payload，不含 PageHeader/校验和（stamp_page_checksum
        // 从未在写路径调用）。此前此处按 hdr->checksum 校验，会把 payload 字节误判为校验和，
        // 对已刷盘的 SSTable 产生假阳性 "checksum mismatch"，导致已提交数据不可读。
        // 真正的页级校验和需在为数据页预留页头后重做（见 ROADMAP）。此处不做页级校验。

        // 更新帧元数据
        frame.id = id;       // 记录此帧对应的页面ID
        frame.dirty = false; // 刚从磁盘加载，未被修改
        // pin_count和usage_count由调用者（pin函数）管理
    }

    // ============================================================================
    //                          脏页刷新
    // ============================================================================

    /**
     * @brief 将脏页帧刷新到磁盘
     * @param frame 要刷新的页面帧
     *
     * 使用共享锁保护帧内容，确保刷新期间数据一致性
     * 即使其他线程在读取页面，刷新操作也是安全的
     */
    void BufferPool::flush_frame(Frame& frame) {
        // 获取帧的共享锁，防止写入时数据被修改导致"撕裂"
        std::shared_lock content_lock(frame.rw_latch);

        // 页面未被修改，无需刷新
        if (!frame.dirty)
            return;

        // 确保目标目录存在（首次写入时可能不存在）
        std::filesystem::create_directories(std::filesystem::path(frame.id.file).parent_path());

        // 尝试以读写模式打开文件（保留现有内容）
        std::fstream fs(frame.id.file, std::ios::binary | std::ios::in | std::ios::out);
        if (!fs) {
            // 文件不存在，创建新文件
            fs.open(frame.id.file, std::ios::binary | std::ios::out | std::ios::trunc);
        }

        // 定位到页面在文件中的偏移位置
        fs.seekp(static_cast<std::streamoff>(frame.id.page_idx) * static_cast<std::streamoff>(page_size_),
                 std::ios::beg);

        // 写入页面数据
        fs.write(frame.data.data(), static_cast<std::streamsize>(page_size_));

        if (!fs)
            throw std::runtime_error("Failed to write page to file: " + frame.id.file);

        // 写入成功，清除脏标志
        frame.dirty = false;

        // 更新统计信息
        stats_.flush_count.fetch_add(1);
    }

    // ============================================================================
    //                          Clock置换算法
    // ============================================================================

    // =========================================================================
    // Clock 替换算法（LRU 近似）
    // =========================================================================
    //
    // 核心思想：环形链表 + 引用位（usage_count）模拟"最近最少使用"。
    // 相比真正的 LRU（需维护链表并在每次访问时移动节点），Clock 算法
    // 只需在淘汰时扫描——访问时只需递增 usage_count，O(1) 开销。
    //
    // 算法流程：
    //   1. 优先从空闲列表（free_list_）分配——O(1)，无淘汰开销
    //   2. 空闲列表为空时，从 clock_hand_ 位置开始环形扫描：
    //      a) 若 usage_count > 0 → 递减（"再给一次机会"），指针前进
    //      b) 若 usage_count == 0 且 pin_count == 0 → 选中淘汰
    //      c) 若 pin_count > 0 → 正在被使用，跳过
    //   3. 最多扫描两轮——两轮后仍未找到，说明所有帧都被固定
    //
    // 与 Linux 内核的 page reclaim 和 PostgreSQL 的 buffer manager 原理相同。
    bool BufferPool::find_victim(FrameId* frame_id, std::unique_lock<std::shared_mutex>& lock) {
        // 前置条件：调用者已持有全局latch_（通过lock参数传入）

        // ==== 策略1：优先使用空闲列表中的帧 ====
        if (!free_list_.empty()) {
            *frame_id = free_list_.front();
            free_list_.pop_front();
            return true;
        }

        // ==== 策略2：Clock扫描寻找牺牲页 ====
        // 最多扫描两轮（第一轮减少usage_count，第二轮找到目标）
        size_t start_hand = clock_hand_;
        size_t passes = 0; // 已完成的扫描轮数

        while (passes < 2) {
            auto& frame = *frames_[clock_hand_];

            // 被固定的页面不能淘汰（正在被使用）
            if (frame.pin_count == 0) {
                if (frame.usage_count > 0) {
                    // 【二次机会】：减少使用计数，继续扫描
                    // 最近被访问过的页面获得"赦免"
                    frame.usage_count--;
                } else {
                    // 【找到牺牲页】：usage_count=0 且未固定

                    // 如果是脏页，需要先刷新到磁盘
                    if (frame.dirty) {
                        // 【并发优化】：刷新脏页时临时释放全局锁
                        // 这允许其他线程在刷新期间访问其他页面

                        // 步骤1：临时增加pin_count，防止其他线程同时选中此帧
                        frame.pin_count++;

                        // 步骤2：释放全局锁
                        lock.unlock();

                        // 步骤3：执行磁盘I/O（耗时操作）
                        flush_frame(frame);

                        // 步骤4：重新获取全局锁
                        lock.lock();

                        // 步骤5：恢复pin_count
                        frame.pin_count--;

                        // 步骤6：【竞态检查】
                        // 在刷新期间，其他线程可能通过pin()获取了此页面
                        if (frame.pin_count > 0) {
                            // 放弃此帧，继续搜索下一个
                            clock_hand_ = (clock_hand_ + 1) % capacity_;
                            continue;
                        }
                        // 此时页面已干净且仍未被固定，可以安全淘汰
                    }

                    // 记录牺牲帧索引
                    *frame_id = clock_hand_;

                    // 从页表中删除旧的页面映射
                    if (frame.id.is_valid()) {
                        page_table_.erase(frame.id);
                    }

                    // 推进时钟指针，供下次使用
                    clock_hand_ = (clock_hand_ + 1) % capacity_;

                    // 更新统计信息
                    stats_.eviction_count.fetch_add(1);
                    return true;
                }
            }

            // 推进时钟指针
            clock_hand_ = (clock_hand_ + 1) % capacity_;

            // 检查是否完成一轮完整扫描
            if (clock_hand_ == start_hand)
                passes++;
        }

        // 所有帧都被固定，无可用帧
        return false;
    }

    // ============================================================================
    //                          页面固定与解除固定
    // ============================================================================

    /**
     * @brief 固定页面到缓冲池
     * @param id 要固定的页面标识符
     * @return 指向页面帧的共享指针
     * @throws std::runtime_error 缓冲池耗尽（所有帧都被固定）
     *
     * 核心操作流程：
     * 1. 查找页面是否已在缓冲池中（页表查询）
     * 2. 命中：增加引用计数，返回帧指针
     * 3. 未命中：找牺牲帧 → 从磁盘加载页面 → 建立映射
     *
     * 【并发优化】：磁盘 I/O 操作在锁外执行，减少全局锁持有时间
     *
     * 调用者在使用完页面后必须调用unpin()释放
     */
    std::shared_ptr<BufferPool::Frame> BufferPool::pin(const PageId& id) {
        // 获取全局排它锁，保护页表和帧元数据的一致性
        std::unique_lock lock(latch_);

        // ==== 步骤1：查找页表 ====
        auto it = page_table_.find(id);
        if (it != page_table_.end()) {
            // 【缓存命中】：页面已在缓冲池中
            auto frame = frames_[it->second];
            frame->pin_count++;     // 增加引用计数
            frame->usage_count = 1; // 标记为最近使用（Clock算法用）
            frame->last_used = std::chrono::steady_clock::now().time_since_epoch().count();
            stats_.hit_count.fetch_add(1);
            return frame;
        }

        // ==== 步骤2：缓存未命中，需要从磁盘加载 ====
        FrameId fid;
        if (!find_victim(&fid, lock)) {
            // 所有帧都被固定，无法分配新帧
            throw std::runtime_error("Buffer pool exhausted: all pages pinned");
        }

        // 获取帧指针（此时帧已从页表中移除，属于"我们"）
        auto frame = frames_[fid];

        // ==== 步骤3：建立新的页面映射 ====
        page_table_[id] = fid;
        frame->pin_count = 1;   // 调用者持有一个引用
        frame->usage_count = 1; // 标记为最近使用
        frame->last_used = std::chrono::steady_clock::now().time_since_epoch().count();

        // ==== 步骤4：【并发优化】在锁外执行磁盘 I/O ====
        // 此时帧已被固定（pin_count=1），不会被其他线程淘汰
        // 释放锁让其他线程可以访问缓冲池
        lock.unlock();

        // 从磁盘加载页面数据（耗时 I/O 操作）
        load_to_frame(id, fid);

        // 更新统计信息
        stats_.miss_count.fetch_add(1);
        return frame;
    }

    /**
     * @brief 分配一个新的空白页面
     * @param id 新页面的标识符
     * @return 指向新分配页面帧的共享指针
     *
     * 与pin()的区别：
     * - pin()用于读取已存在的页面
     * - allocate()用于创建新页面，初始化为全零
     */
    std::shared_ptr<BufferPool::Frame> BufferPool::allocate(const PageId& id) {
        std::unique_lock lock(latch_);

        FrameId fid;

        // 检查页面是否已存在
        if (page_table_.count(id)) {
            // 页面已存在：复用它但清零内容
            // 这种情况不常见，可能是页面被重用
            auto frame = frames_[page_table_[id]];
            frame->pin_count++;
            frame->usage_count = 1;
            frame->last_used = std::chrono::steady_clock::now().time_since_epoch().count();
            std::ranges::fill(frame->data, '\0'); // 清零数据
            frame->dirty = true;                  // 标记为脏（需要写回磁盘）
            return frame;
        }

        // 分配新帧
        if (!find_victim(&fid, lock)) {
            throw std::runtime_error("Buffer pool exhausted");
        }

        // 建立映射
        page_table_[id] = fid;
        auto frame = frames_[fid];

        // 初始化帧
        frame->pin_count = 1;
        frame->usage_count = 1;
        frame->last_used = std::chrono::steady_clock::now().time_since_epoch().count();
        frame->id = id;

        // 将页面数据初始化为全零
        std::fill(frame->data.begin(), frame->data.end(), '\0');
        frame->dirty = true; // 新分配的页面需要写入磁盘

        return frame;
    }

    /**
     * @brief 将页面标记为脏（已修改）
     * @param frame 要标记的页面帧
     *
     * 调用者在修改页面内容后应调用此函数
     * 脏页会在unpin后或flush_all时写回磁盘
     */
    void BufferPool::mark_dirty(const std::shared_ptr<Frame>& frame) const {
        if (!frame)
            return;
        frame->dirty = true;
    }

    /**
     * @brief 取消固定页面（减少引用计数）
     * @param frame 要取消固定的页面帧
     *
     * 当pin_count降为0时，页面成为淘汰候选
     * 注意：不需要全局锁，pin_count是原子操作
     */
    void BufferPool::unpin(const std::shared_ptr<Frame>& frame) const {
        if (!frame)
            return;

        // 原子减少引用计数
        // 当计数归零时，页面可被Clock算法选中淘汰
        if (frame->pin_count > 0) {
            frame->pin_count--;
        }
    }

    // ============================================================================
    //                          批量刷新
    // ============================================================================

    /**
     * @brief 刷新缓冲池中所有脏页面到磁盘
     *
     * 优化策略：
     * 1. 持共享锁收集脏页列表（允许并发读取）
     * 2. 释放锁后逐个刷新（减少锁持有时间）
     *
     * 典型调用场景：
     * - 缓冲池销毁前
     * - 检查点(checkpoint)操作
     * - 事务提交时的强制刷新
     */
    void BufferPool::flush_all() {
        // ==== 阶段1：收集脏页列表 ====
        std::vector<std::shared_ptr<Frame>> dirty_frames;
        {
            std::shared_lock lock(latch_);            // 共享锁，允许并发pin操作
            dirty_frames.reserve(frames_.size() / 4); // 预估约25%页面是脏的

            for (const auto& frame: frames_) {
                if (frame->dirty) {
                    dirty_frames.push_back(frame);
                }
            }
        }
        // 共享锁在此释放

        // ==== 阶段2：批量刷新脏页 ====
        // 无需持有全局锁，flush_frame内部有页面级别的锁保护
        for (const auto& frame: dirty_frames) {
            flush_frame(*frame);
        }
    }

    /**
     * @brief 刷新指定页面到磁盘
     *
     * @param id 页面标识符
     * @return true 如果页面被成功刷新
     * @return false 如果页面不在缓冲池中或不是脏页
     */
    bool BufferPool::flush_page(const PageId& id) {
        std::shared_lock lock(latch_);
        auto it = page_table_.find(id);
        if (it == page_table_.end()) {
            return false;
        }

        auto frame = frames_[it->second];
        if (!frame->dirty) {
            return false;
        }

        lock.unlock();
        flush_frame(*frame);
        return true;
    }

    /**
     * @brief 预热缓冲池
     *
     * 预加载指定的页面到缓冲池，提高后续操作的性能。
     *
     * @param ids 要预加载的页面ID列表
     */
    void BufferPool::warmup(const std::vector<PageId>& ids) {
        for (const auto& id: ids) {
            try {
                auto frame = pin(id);
                unpin(frame);
            } catch (const std::exception&) {
                // 忽略加载错误，继续预热其他页面
            }
        }
    }

    /**
     * @brief 获取缓冲池统计信息
     *
     * @return Stats 统计信息
     */
    BufferPool::Stats BufferPool::get_stats() const {
        Stats stats;
        stats.current_size = size();

        // 计算脏页和被固定的页面数量
        std::shared_lock lock(latch_);
        stats.dirty_pages = 0;
        stats.pinned_pages = 0;

        for (const auto& frame: frames_) {
            if (frame->dirty) {
                stats.dirty_pages++;
            }
            if (frame->pin_count > 0) {
                stats.pinned_pages++;
            }
        }
        lock.unlock();

        stats.hit_count = stats_.hit_count;
        stats.miss_count = stats_.miss_count;
        stats.hit_ratio = (stats_.hit_count + stats_.miss_count) > 0
                                  ? static_cast<double>(stats_.hit_count) / (stats_.hit_count + stats_.miss_count)
                                  : 0.0;
        stats.flush_count = stats_.flush_count;
        stats.eviction_count = stats_.eviction_count;

        return stats;
    }

    /**
     * @brief 重置统计信息
     */
    void BufferPool::reset_stats() {
        stats_.hit_count = 0;
        stats_.miss_count = 0;
        stats_.flush_count = 0;
        stats_.eviction_count = 0;
    }

    // ============================================================================
    //                          全局缓冲池管理器实现
    // ============================================================================

    /**
     * @brief 创建缓冲池实例
     *
     * @param name 缓冲池名称
     * @param page_size 页大小
     * @param capacity_pages 容量（页数）
     * @param memory_quota 内存配额（字节，0表示无限制）
     */
    void GlobalBufferPoolManager::create_buffer_pool(const std::string& name, std::size_t page_size,
                                                     std::size_t capacity_pages, std::size_t memory_quota) {
        std::unique_lock lock(mutex_);

        if (pools_.find(name) != pools_.end()) {
            // 缓冲池已存在，更新配置
            auto& info = pools_[name];
            info.pool = std::make_unique<BufferPool>(page_size, capacity_pages);
            info.memory_quota = memory_quota;
            info.creation_time = std::chrono::steady_clock::now();
        } else {
            // 创建新缓冲池
            BufferPoolInfo info;
            info.pool = std::make_unique<BufferPool>(page_size, capacity_pages);
            info.memory_quota = memory_quota;
            info.creation_time = std::chrono::steady_clock::now();
            pools_[name] = std::move(info);
        }
    }

    /**
     * @brief 获取缓冲池实例
     *
     * @param name 缓冲池名称
     * @return BufferPool* 缓冲池指针，不存在返回nullptr
     */
    BufferPool* GlobalBufferPoolManager::get_buffer_pool(const std::string& name) {
        std::shared_lock lock(mutex_);
        auto it = pools_.find(name);
        if (it == pools_.end()) {
            return nullptr;
        }
        return it->second.pool.get();
    }

    /**
     * @brief 删除缓冲池实例
     *
     * @param name 缓冲池名称
     * @return true 如果成功删除
     */
    bool GlobalBufferPoolManager::remove_buffer_pool(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = pools_.find(name);
        if (it == pools_.end()) {
            return false;
        }
        pools_.erase(it);
        return true;
    }

    /**
     * @brief 获取所有缓冲池名称
     *
     * @return 缓冲池名称列表
     */
    std::vector<std::string> GlobalBufferPoolManager::list_buffer_pools() const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> names;
        for (const auto& [name, _]: pools_) {
            names.push_back(name);
        }
        return names;
    }

    /**
     * @brief 获取内存使用统计
     *
     * @return MemoryStats 内存统计信息
     */
    GlobalBufferPoolManager::MemoryStats GlobalBufferPoolManager::get_memory_stats() const {
        MemoryStats stats;
        stats.total_memory = 0;
        stats.used_memory = 0;
        stats.dirty_memory = 0;

        std::shared_lock lock(mutex_);
        for (const auto& [_, info]: pools_) {
            auto pool_stats = info.pool->get_stats();
            std::size_t page_size = info.pool->page_size();

            stats.used_memory += pool_stats.current_size * page_size;
            stats.dirty_memory += pool_stats.dirty_pages * page_size;
            stats.total_memory += info.pool->capacity() * page_size;
        }

        stats.peak_memory = stats.used_memory; // 简化实现，实际应跟踪峰值
        return stats;
    }

} // namespace corodb
