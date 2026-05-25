// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file buffer_pool.h @brief BufferPool 类型定义。 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace corodb {

    /** @brief 页头元数据。 */
    struct PageHeader {
        uint64_t lsn{ 0 };
        uint32_t checksum{ 0 };
        uint16_t slot_count{ 0 };
        uint16_t free_start{ 0 };
        uint16_t free_end{ 0 };

        [[nodiscard]] constexpr std::size_t free_space() const noexcept {
            return (free_end > free_start) ? (free_end - free_start) : 0;
        }
    };

    /** @brief 页面标识符。 */
    struct PageId {
        std::string file;
        uint32_t page_idx{ 0 };

        [[nodiscard]] bool operator==(const PageId& other) const noexcept {
            return page_idx == other.page_idx && file == other.file;
        }

        [[nodiscard]] bool operator!=(const PageId& other) const noexcept {
            return !(*this == other);
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return !file.empty();
        }
    };

    /** @brief PageId 的哈希器。 */
    struct PageIdHash {
        [[nodiscard]] std::size_t operator()(const PageId& id) const noexcept {
            const std::size_t h1 = std::hash<std::string>{}(id.file);
            const std::size_t h2 = std::hash<uint32_t>{}(id.page_idx);
            return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
    };

    /** @brief 线程安全的页面缓存。 */
    class BufferPool {
    public:
        /// 帧索引类型
        using FrameId = std::size_t;

        /** @brief 缓存页帧，包含页数据、元数据及并发控制锁。 */
        struct Frame {
            std::vector<char> data;                 ///< 页面数据缓冲区
            PageId id;                              ///< 页面标识符
            std::atomic<bool> dirty{ false };       ///< 脏页标志，表示页面是否被修改
            std::atomic<uint32_t> pin_count{ 0 };   ///< 固定计数，表示有多少客户端正在使用此帧
            std::atomic<uint32_t> usage_count{ 0 }; ///< 使用计数，用于Clock置换算法
            std::atomic<uint64_t> last_used{ 0 };   ///< 最后使用时间戳，用于统计
            mutable std::shared_mutex rw_latch;     ///< 读写锁，保护页面内容的并发访问

            /// 默认构造函数
            Frame() = default;

            // 禁止拷贝和移动，确保Frame在内存中位置固定
            Frame(const Frame&) = delete;
            Frame& operator=(const Frame&) = delete;
            Frame(Frame&&) = delete;
            Frame& operator=(Frame&&) = delete;

            /// 检查帧是否被固定。
            [[nodiscard]] bool is_pinned() const noexcept {
                return pin_count.load(std::memory_order_relaxed) > 0;
            }

            /// 检查帧是否为脏页。
            [[nodiscard]] bool is_dirty() const noexcept {
                return dirty.load(std::memory_order_relaxed);
            }
        };

        /**
         * @brief 构造缓冲池实例。
         * @param page_size 页大小（字节，通常为 4096）
         * @param capacity_pages 缓冲池容量（页数）
         * @throws std::bad_alloc 若内存分配失败。
         */
        BufferPool(std::size_t page_size, std::size_t capacity_pages);

        /** @brief 析构时将所有脏页刷新到磁盘。 */
        ~BufferPool();

        // 禁止拷贝和移动
        BufferPool(const BufferPool&) = delete;
        BufferPool& operator=(const BufferPool&) = delete;
        BufferPool(BufferPool&&) = delete;
        BufferPool& operator=(BufferPool&&) = delete;

        /**
         * @name 页面操作
         * @{
         */

        /**
         * @brief 固定并返回页帧（必要时从磁盘加载）。
         * @throws std::runtime_error 若缓冲池已满或无法从磁盘加载。
         */
        [[nodiscard]] std::shared_ptr<Frame> pin(const PageId& id);

        /**
         * @brief 分配并返回一个零初始化的新页帧（已固定并标记为脏）。
         * @throws std::runtime_error 若缓冲池已满。
         */
        [[nodiscard]] std::shared_ptr<Frame> allocate(const PageId& id);

        /** @brief 标记页帧为脏页（需在置换前写回磁盘）。 */
        void mark_dirty(const std::shared_ptr<Frame>& frame) const;

        /** @brief 减少页帧的固定计数；计数归零后该帧可被置换。 */
        void unpin(const std::shared_ptr<Frame>& frame) const;

        /** @} */

        /**
         * @name 缓冲池管理
         * @{
         */

        /**
         * @brief 将所有脏页刷新到磁盘。
         * @throws std::runtime_error 若写入磁盘失败。
         */
        void flush_all();

        /**
         * @brief 刷新指定页面到磁盘。
         * @return true 若成功刷新，false 若页面不在缓冲池中或不是脏页。
         */
        bool flush_page(const PageId& id);

        /**
         * @brief 预热缓冲池，预加载指定页面以提升后续访问性能。
         */
        void warmup(const std::vector<PageId>& ids);

        /** @brief 返回当前缓存的页数。 */
        [[nodiscard]] std::size_t size() const;

        /** @brief 返回缓冲池容量（最大可缓存页数）。 */
        [[nodiscard]] std::size_t capacity() const noexcept {
            return capacity_;
        }

        /**
         * @brief 获取页大小
         *
         * @return std::size_t 页大小（字节）
         */
        [[nodiscard]] std::size_t page_size() const noexcept {
            return page_size_;
        }

        /**
         * @brief 获取缓冲池统计信息
         */
        struct Stats {
            std::size_t current_size;   ///< 当前缓存的页数
            std::size_t dirty_pages;    ///< 脏页数量
            std::size_t pinned_pages;   ///< 被固定的页数
            std::size_t hit_count;      ///< 缓存命中次数
            std::size_t miss_count;     ///< 缓存未命中次数
            double hit_ratio;           ///< 缓存命中率
            std::size_t flush_count;    ///< 刷新操作次数
            std::size_t eviction_count; ///< 页面淘汰次数
        };

        /** @brief 返回当前缓冲池统计快照。 */
        [[nodiscard]] Stats get_stats() const;

        /** @brief 重置统计计数器。 */
        void reset_stats();

        /** @} */

    private:
        std::size_t page_size_; ///< 页大小（字节）
        std::size_t capacity_;  ///< 缓冲池容量（页数）

        std::vector<std::shared_ptr<Frame>> frames_;                 ///< 页帧数组
        std::unordered_map<PageId, FrameId, PageIdHash> page_table_; ///< 页表：PageId -> FrameId
        std::list<FrameId> free_list_;                               ///< 空闲帧列表

        std::size_t clock_hand_{ 0 }; ///< Clock算法的指针位置

        // ====================================================================
        // 分片锁设计：减少锁争用
        // ====================================================================
        static constexpr std::size_t kNumShards = 16;                     ///< 分片数量
        mutable std::array<std::shared_mutex, kNumShards> shard_latches_; ///< 分片锁
        mutable std::shared_mutex global_latch_;                          ///< 全局锁（仅用于页表修改）

        // 统计信息
        struct {
            std::atomic<std::size_t> hit_count{ 0 };
            std::atomic<std::size_t> miss_count{ 0 };
            std::atomic<std::size_t> flush_count{ 0 };
            std::atomic<std::size_t> eviction_count{ 0 };
        } stats_;

        // 兼容性别名（保持旧代码可编译）
        std::shared_mutex& latch_ = global_latch_;

        /// 获取页面对应的分片索引。
        [[nodiscard]] std::size_t get_shard_index(const PageId& id) const noexcept {
            return PageIdHash{}(id) % kNumShards;
        }

        /// 从磁盘加载页面到指定帧。
        void load_to_frame(const PageId& id, FrameId frame_id);

        /// 将帧中的脏页写入磁盘。
        void flush_frame(Frame& frame);

        /// 用 Clock 置换算法寻找可用帧（必要时刷脏）。
        bool find_victim(FrameId* frame_id, std::unique_lock<std::shared_mutex>& lock);
    };

    /** @brief 全局缓冲池管理器，统一管理多个缓冲池实例。 */
    class GlobalBufferPoolManager {
    public:
        /** @brief 返回全局单例实例。 */
        static GlobalBufferPoolManager& instance() {
            static GlobalBufferPoolManager instance;
            return instance;
        }

        /**
         * @brief 创建命名缓冲池。
         * @param memory_quota 内存配额（字节，0 表示无限制）
         */
        void create_buffer_pool(const std::string& name, std::size_t page_size, std::size_t capacity_pages,
                                std::size_t memory_quota = 0);

        /**
         * @brief 获取命名缓冲池实例。
         * @return 不存在时返回 nullptr。
         */
        BufferPool* get_buffer_pool(const std::string& name);

        /** @brief 删除命名缓冲池，返回是否成功删除。 */
        bool remove_buffer_pool(const std::string& name);

        /** @brief 列出所有已创建的缓冲池名称。 */
        std::vector<std::string> list_buffer_pools() const;

        /**
         * @brief 获取全局内存使用情况
         */
        struct MemoryStats {
            std::size_t total_memory; ///< 总内存使用
            std::size_t used_memory;  ///< 已使用内存
            std::size_t peak_memory;  ///< 峰值内存使用
            std::size_t dirty_memory; ///< 脏页内存使用
        };

        /** @brief 返回全局内存使用统计。 */
        MemoryStats get_memory_stats() const;

    private:
        GlobalBufferPoolManager() = default;
        ~GlobalBufferPoolManager() = default;

        GlobalBufferPoolManager(const GlobalBufferPoolManager&) = delete;
        GlobalBufferPoolManager& operator=(const GlobalBufferPoolManager&) = delete;

        struct BufferPoolInfo {
            std::unique_ptr<BufferPool> pool;
            std::size_t memory_quota;
            std::chrono::steady_clock::time_point creation_time;
        };

        std::unordered_map<std::string, BufferPoolInfo> pools_;
        mutable std::shared_mutex mutex_;
    };

    /** @brief 线程安全的单调递增日志序列号（LSN）分配器。 */
    class LsnAllocator {
    public:
        /// 原子递增并返回下一个唯一 LSN。
        [[nodiscard]] uint64_t next() noexcept {
            return lsn_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        /// 读取当前 LSN（不递增）。
        [[nodiscard]] uint64_t current() const noexcept {
            return lsn_.load(std::memory_order_relaxed);
        }

        /// 设置 LSN 初始值（用于崩溃恢复）。
        void set(uint64_t value) noexcept {
            lsn_.store(value, std::memory_order_relaxed);
        }

    private:
        std::atomic<uint64_t> lsn_{ 0 }; ///< 当前LSN值
    };

} // namespace corodb
