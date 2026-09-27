// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file config.h @brief 全局配置参数定义（单例，INI 文件加载）。 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace corodb {

    class Config {
    public:
        // =========================================================================
        // 默认常量
        // =========================================================================
        static constexpr std::size_t kDefaultPageSize = 8192;
        static constexpr std::size_t kDefaultBufferPages = 256;
        static constexpr std::size_t kDefaultMemTableSize = 1 * 1024 * 1024; // 1 MB
        static constexpr uint32_t  kDefaultGroupCommitDelayUs = 1000;
        static constexpr uint32_t  kDefaultGroupCommitBatchSize = 64;
        static constexpr uint16_t  kDefaultServerPort = 4000;
        static constexpr std::size_t kDefaultMaxConnections = 10000;
        static constexpr std::size_t kDefaultMaxBufferSize = 64 * 1024 * 1024; // 64 MB
        static constexpr uint32_t  kDefaultLockTimeoutMs = 5000;
        static constexpr uint32_t  kDefaultNetTimeoutMs = 30000;
        static constexpr std::size_t kDefaultL0CompactionThreshold = 64 * 1024; // 64 KB
        static constexpr int       kDefaultLsmMaxLevel = 3;
        static constexpr std::size_t kDefaultSstCacheEntries = 256;
        static constexpr std::size_t kDefaultPlanCacheEntries = 128;
        static constexpr uint64_t  kDefaultStatementTimeoutMs = 0; // disabled
        static constexpr std::size_t kDefaultThreadPoolMaxQueue = 0; // unlimited
        // [statistics]（ANALYZE 采集，Phase 1）
        static constexpr std::size_t kDefaultStatisticsSampleTarget = 300;
        static constexpr std::size_t kDefaultStatisticsMaxMcv = 20;
        static constexpr std::size_t kDefaultStatisticsHistogramBuckets = 32;
        static constexpr double kDefaultAutoAnalyzeThreshold = 0.1;
        // [optimizer]（代价模型，Phase 2；默认值对齐 PostgreSQL）
        static constexpr double kDefaultSeqPageCost = 1.0;
        static constexpr double kDefaultRandomPageCost = 4.0;
        static constexpr double kDefaultCpuTupleCost = 0.01;
        static constexpr double kDefaultCpuIndexTupleCost = 0.005;
        static constexpr double kDefaultCpuOperatorCost = 0.0025;
        // [metrics]（观测性，Phase P2）
        static constexpr bool    kDefaultMetricsEnabled = true;
        static constexpr uint16_t kDefaultMetricsPort = 4100;
        static constexpr uint64_t kDefaultSlowQueryMs = 1000;

        static Config& instance() {
            static Config config;
            return config;
        }

        // =========================================================================
        // 文件 I/O
        // =========================================================================
        bool load_from_file(const std::string& path, std::string* error = nullptr);
        static bool write_default_file(const std::string& path);

        [[nodiscard]] const std::string& config_path() const noexcept { return config_path_; }

        // =========================================================================
        // [storage]
        // =========================================================================
        [[nodiscard]] std::size_t page_size() const noexcept { return page_size_; }
        [[nodiscard]] std::size_t buffer_pages() const noexcept { return buffer_pages_; }
        [[nodiscard]] std::size_t memtable_size_bytes() const noexcept { return memtable_size_bytes_; }
        void set_page_size(std::size_t v) noexcept { page_size_ = v; }
        void set_buffer_pages(std::size_t v) noexcept { buffer_pages_ = v; }
        void set_memtable_size_bytes(std::size_t v) noexcept { memtable_size_bytes_ = v; }

        // =========================================================================
        // [wal]
        // =========================================================================
        [[nodiscard]] uint32_t group_commit_delay_us() const noexcept { return group_commit_delay_us_; }
        [[nodiscard]] uint32_t group_commit_batch_size() const noexcept { return group_commit_batch_size_; }
        [[nodiscard]] const std::string& wal_sync_mode() const noexcept { return wal_sync_mode_; }
        void set_group_commit_delay_us(uint32_t v) noexcept { group_commit_delay_us_ = v; }
        void set_group_commit_batch_size(uint32_t v) noexcept { group_commit_batch_size_ = v; }
        void set_wal_sync_mode(std::string v) noexcept { wal_sync_mode_ = std::move(v); }

        // =========================================================================
        // [server]
        // =========================================================================
        [[nodiscard]] uint16_t server_port() const noexcept { return server_port_; }
        [[nodiscard]] const std::string& data_dir() const noexcept { return data_dir_; }
        [[nodiscard]] std::size_t max_connections() const noexcept { return max_connections_; }
        [[nodiscard]] std::size_t io_threads() const noexcept { return io_threads_; }
        [[nodiscard]] std::size_t worker_threads() const noexcept { return worker_threads_; }
        [[nodiscard]] bool reuse_port() const noexcept { return reuse_port_; }
        [[nodiscard]] uint64_t idle_timeout_sec() const noexcept { return idle_timeout_sec_; }
        [[nodiscard]] uint64_t statement_timeout_ms() const noexcept { return statement_timeout_ms_; }
        void set_server_port(uint16_t v) noexcept { server_port_ = v; }
        void set_data_dir(std::string v) noexcept { data_dir_ = std::move(v); }
        void set_max_connections(std::size_t v) noexcept { max_connections_ = v; }
        void set_io_threads(std::size_t v) noexcept { io_threads_ = v; }
        void set_worker_threads(std::size_t v) noexcept { worker_threads_ = v; }
        void set_reuse_port(bool v) noexcept { reuse_port_ = v; }
        void set_idle_timeout_sec(uint64_t v) noexcept { idle_timeout_sec_ = v; }
        void set_statement_timeout_ms(uint64_t v) noexcept { statement_timeout_ms_ = v; }

        // =========================================================================
        // [connection]
        // =========================================================================
        [[nodiscard]] std::size_t max_buffer_size() const noexcept { return max_buffer_size_; }
        void set_max_buffer_size(std::size_t v) noexcept { max_buffer_size_ = v; }

        // =========================================================================
        // [network]
        // =========================================================================
        [[nodiscard]] uint32_t net_send_timeout_ms() const noexcept { return net_send_timeout_ms_; }
        [[nodiscard]] uint32_t net_read_timeout_ms() const noexcept { return net_read_timeout_ms_; }
        void set_net_send_timeout_ms(uint32_t v) noexcept { net_send_timeout_ms_ = v; }
        void set_net_read_timeout_ms(uint32_t v) noexcept { net_read_timeout_ms_ = v; }

        // =========================================================================
        // [lock_manager]
        // =========================================================================
        [[nodiscard]] uint32_t lock_timeout_ms() const noexcept { return lock_timeout_ms_; }
        void set_lock_timeout_ms(uint32_t v) noexcept { lock_timeout_ms_ = v; }

        // =========================================================================
        // [lsm]
        // =========================================================================
        [[nodiscard]] std::size_t l0_compaction_threshold_bytes() const noexcept { return l0_threshold_; }
        [[nodiscard]] int lsm_max_level() const noexcept { return lsm_max_level_; }
        [[nodiscard]] std::size_t sst_cache_entries() const noexcept { return sst_cache_entries_; }
        void set_l0_compaction_threshold_bytes(std::size_t v) noexcept { l0_threshold_ = v; }
        void set_lsm_max_level(int v) noexcept { lsm_max_level_ = v; }
        void set_sst_cache_entries(std::size_t v) noexcept { sst_cache_entries_ = v; }

        // =========================================================================
        // [plan_cache]
        // =========================================================================
        [[nodiscard]] std::size_t plan_cache_entries() const noexcept { return plan_cache_entries_; }
        void set_plan_cache_entries(std::size_t v) noexcept { plan_cache_entries_ = v; }

        // =========================================================================
        // [thread_pool]
        // =========================================================================
        [[nodiscard]] std::size_t thread_pool_max_queue() const noexcept { return thread_pool_max_queue_; }
        void set_thread_pool_max_queue(std::size_t v) noexcept { thread_pool_max_queue_ = v; }

        // =========================================================================
        // [auth]
        // =========================================================================
        [[nodiscard]] const std::string& auth_salt() const noexcept { return auth_salt_; }
        void set_auth_salt(std::string v) noexcept { auth_salt_ = std::move(v); }

        // =========================================================================
        // [statistics]
        // =========================================================================
        [[nodiscard]] std::size_t statistics_sample_target() const noexcept { return statistics_sample_target_; }
        [[nodiscard]] std::size_t statistics_max_mcv() const noexcept { return statistics_max_mcv_; }
        [[nodiscard]] std::size_t statistics_histogram_buckets() const noexcept {
            return statistics_histogram_buckets_;
        }
        [[nodiscard]] double statistics_auto_analyze_threshold() const noexcept {
            return statistics_auto_analyze_threshold_;
        }
        void set_statistics_sample_target(std::size_t v) noexcept { statistics_sample_target_ = v; }
        void set_statistics_max_mcv(std::size_t v) noexcept { statistics_max_mcv_ = v; }
        void set_statistics_histogram_buckets(std::size_t v) noexcept {
            statistics_histogram_buckets_ = v;
        }
        void set_statistics_auto_analyze_threshold(double v) noexcept {
            statistics_auto_analyze_threshold_ = v;
        }

        // =========================================================================
        // [optimizer]
        // =========================================================================
        [[nodiscard]] bool cost_model_enabled() const noexcept { return cost_model_enabled_; }
        [[nodiscard]] double seq_page_cost() const noexcept { return seq_page_cost_; }
        [[nodiscard]] double random_page_cost() const noexcept { return random_page_cost_; }
        [[nodiscard]] double cpu_tuple_cost() const noexcept { return cpu_tuple_cost_; }
        [[nodiscard]] double cpu_index_tuple_cost() const noexcept { return cpu_index_tuple_cost_; }
        [[nodiscard]] double cpu_operator_cost() const noexcept { return cpu_operator_cost_; }
        void set_cost_model_enabled(bool v) noexcept { cost_model_enabled_ = v; }
        void set_seq_page_cost(double v) noexcept { seq_page_cost_ = v; }
        void set_random_page_cost(double v) noexcept { random_page_cost_ = v; }
        void set_cpu_tuple_cost(double v) noexcept { cpu_tuple_cost_ = v; }
        void set_cpu_index_tuple_cost(double v) noexcept { cpu_index_tuple_cost_ = v; }
        void set_cpu_operator_cost(double v) noexcept { cpu_operator_cost_ = v; }

        // =========================================================================
        // [metrics]
        // =========================================================================
        [[nodiscard]] bool metrics_enabled() const noexcept { return metrics_enabled_; }
        [[nodiscard]] uint16_t metrics_port() const noexcept { return metrics_port_; }
        [[nodiscard]] uint64_t metrics_slow_query_ms() const noexcept { return metrics_slow_query_ms_; }
        void set_metrics_enabled(bool v) noexcept { metrics_enabled_ = v; }
        void set_metrics_port(uint16_t v) noexcept { metrics_port_ = v; }
        void set_metrics_slow_query_ms(uint64_t v) noexcept { metrics_slow_query_ms_ = v; }

    private:
        Config() = default;
        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;

        void apply_kv(const std::string& key, const std::string& value);

        std::string config_path_;

        // [storage]
        std::size_t page_size_ = kDefaultPageSize;
        std::size_t buffer_pages_ = kDefaultBufferPages;
        std::size_t memtable_size_bytes_ = kDefaultMemTableSize;

        // [wal]
        uint32_t group_commit_delay_us_ = kDefaultGroupCommitDelayUs;
        uint32_t group_commit_batch_size_ = kDefaultGroupCommitBatchSize;
        // 默认 durable：保证提交后的数据即使断电也不丢失（生产安全）。
        // 追求极致写吞吐且可容忍断电丢数据时，可改为 fast。
        std::string wal_sync_mode_ = "durable";

        // [server]
        uint16_t server_port_ = kDefaultServerPort;
        std::string data_dir_ = "data";
        std::size_t max_connections_ = kDefaultMaxConnections;
        std::size_t io_threads_ = 0;
        std::size_t worker_threads_ = 0;
        bool reuse_port_ = true;
        uint64_t idle_timeout_sec_ = 0;
        uint64_t statement_timeout_ms_ = 0;

        // [connection]
        std::size_t max_buffer_size_ = kDefaultMaxBufferSize;

        // [network]
        uint32_t net_send_timeout_ms_ = kDefaultNetTimeoutMs;
        uint32_t net_read_timeout_ms_ = kDefaultNetTimeoutMs;

        // [lock_manager]
        uint32_t lock_timeout_ms_ = kDefaultLockTimeoutMs;

        // [lsm]
        std::size_t l0_threshold_ = kDefaultL0CompactionThreshold;
        int lsm_max_level_ = kDefaultLsmMaxLevel;
        std::size_t sst_cache_entries_ = kDefaultSstCacheEntries;

        // [plan_cache]
        std::size_t plan_cache_entries_ = kDefaultPlanCacheEntries;

        // [thread_pool]
        std::size_t thread_pool_max_queue_ = kDefaultThreadPoolMaxQueue;

        // [auth]
        std::string auth_salt_ = "corodb_salt_v1";

        // [statistics]
        std::size_t statistics_sample_target_ = kDefaultStatisticsSampleTarget;
        std::size_t statistics_max_mcv_ = kDefaultStatisticsMaxMcv;
        std::size_t statistics_histogram_buckets_ = kDefaultStatisticsHistogramBuckets;
        double statistics_auto_analyze_threshold_ = kDefaultAutoAnalyzeThreshold;

        // [optimizer]
        bool cost_model_enabled_ = true;
        double seq_page_cost_ = kDefaultSeqPageCost;
        double random_page_cost_ = kDefaultRandomPageCost;
        double cpu_tuple_cost_ = kDefaultCpuTupleCost;
        double cpu_index_tuple_cost_ = kDefaultCpuIndexTupleCost;
        double cpu_operator_cost_ = kDefaultCpuOperatorCost;

        // [metrics]
        bool metrics_enabled_ = kDefaultMetricsEnabled;
        uint16_t metrics_port_ = kDefaultMetricsPort;
        uint64_t metrics_slow_query_ms_ = kDefaultSlowQueryMs;
    };

} // namespace corodb
