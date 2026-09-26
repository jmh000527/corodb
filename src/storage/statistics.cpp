// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file statistics.cpp
// @brief 列级统计信息的序列化与反序列化实现。
//
// 二进制格式：
//
//   TableStats:
//   +----------+------+------------------+------------------+
//   | TSTAT(5) | Ver(1)| table_name_len(2)| table_name(var)  |
//   +----------+------+------------------+------------------+
//   | total_rows(8) | stats_ts(8) | col_count(2)            |
//   +---------------+------------+--------------------------+
//   | ColumnStats[0] | ColumnStats[1] | ...                 |
//   +----------------+----------------+----------------------+
//
//   ColumnStats:
//   +----------------+------------+----------+-----------+----------+
//   | col_name_len(2)| col_name   | type(1)  | total(8)  | null(8)  |
//   +----------------+------------+----------+-----------+----------+
//   | ndistinct(8) | mcv_count(2) | [Value + freq(8)] × mcv_count  |
//   +--------------+--------------+----------------------------------+
//   | hist_count(2) | [Value] × hist_count | correlation(8) | stats_ts(8) |
//   +----------------+---------------------+----------------+------------+
//
//   Value 编码复用 storage_internal::write_value / read_value：
//     tag=0x00 NULL | tag=0x01 int64_t(8) | tag=0x02 string(len(4)+data) | tag=0x03 double(8)

#include "corodb/storage/statistics.h"

#include <cstring>
#include <istream>
#include <ostream>
#include <sstream>

#include "corodb/storage/storage_engine_common.h"

namespace corodb {

    namespace {

        // --- 魔数与版本 ---
        constexpr char kTableStatsMagic[5] = { 'T', 'S', 'T', 'A', 'T' };
        constexpr char kColumnStatsMagic[5] = { 'C', 'S', 'T', 'A', 'T' };
        constexpr uint8_t kFormatVersion = 1;

        // --- 小端写入辅助 ---
        void write_u8(std::ostream& os, uint8_t v) {
            os.put(static_cast<char>(v));
        }

        void write_u16(std::ostream& os, uint16_t v) {
            os.write(reinterpret_cast<const char*>(&v), sizeof(v));
        }

        void write_u64(std::ostream& os, uint64_t v) {
            os.write(reinterpret_cast<const char*>(&v), sizeof(v));
        }

        void write_f64(std::ostream& os, double v) {
            os.write(reinterpret_cast<const char*>(&v), sizeof(v));
        }

        void write_string(std::ostream& os, const std::string& s) {
            uint16_t len = static_cast<uint16_t>(s.size());
            write_u16(os, len);
            os.write(s.data(), static_cast<std::streamsize>(s.size()));
        }

        void write_value_wrapped(std::ostream& os, const Value& v) {
            storage_internal::write_value(os, v);
        }

        // --- 小端读取辅助 ---
        bool read_u8(std::istream& is, uint8_t& out) {
            is.read(reinterpret_cast<char*>(&out), sizeof(out));
            return static_cast<bool>(is);
        }

        bool read_u16(std::istream& is, uint16_t& out) {
            is.read(reinterpret_cast<char*>(&out), sizeof(out));
            return static_cast<bool>(is);
        }

        bool read_u64(std::istream& is, uint64_t& out) {
            is.read(reinterpret_cast<char*>(&out), sizeof(out));
            return static_cast<bool>(is);
        }

        bool read_f64(std::istream& is, double& out) {
            is.read(reinterpret_cast<char*>(&out), sizeof(out));
            return static_cast<bool>(is);
        }

        bool read_string(std::istream& is, std::string& out) {
            uint16_t len = 0;
            if (!read_u16(is, len))
                return false;
            out.resize(len);
            if (len > 0)
                is.read(out.data(), len);
            return static_cast<bool>(is);
        }

        bool read_value_wrapped(std::istream& is, Value& out) {
            try {
                out = storage_internal::read_value(is);
                return true;
            } catch (...) {
                return false;
            }
        }

        // --- 魔数校验 ---
        bool check_magic(std::istream& is, const char (&magic)[5]) {
            char buf[5];
            is.read(buf, 5);
            if (!is || std::memcmp(buf, magic, 5) != 0)
                return false;
            return true;
        }

    } // anonymous namespace

    // =========================================================================
    // ColumnStats
    // =========================================================================

    std::string ColumnStats::serialize() const {
        std::ostringstream oss(std::ios::binary);

        // 魔数 + 版本
        oss.write(kColumnStatsMagic, 5);
        write_u8(oss, kFormatVersion);

        // 列名 + 类型
        write_string(oss, column_name);
        write_u8(oss, static_cast<uint8_t>(type));

        // 基础统计
        write_u64(oss, static_cast<uint64_t>(total_rows));
        write_u64(oss, static_cast<uint64_t>(null_count));
        write_f64(oss, null_frac);
        write_u64(oss, static_cast<uint64_t>(ndistinct));

        // MCV
        uint16_t mcv_count = static_cast<uint16_t>(mcv_values.size());
        write_u16(oss, mcv_count);
        for (uint16_t i = 0; i < mcv_count; ++i) {
            write_value_wrapped(oss, mcv_values[i]);
            write_f64(oss, mcv_freqs[i]);
        }

        // 直方图
        uint16_t hist_count = static_cast<uint16_t>(histogram_bounds.size());
        write_u16(oss, hist_count);
        for (uint16_t i = 0; i < hist_count; ++i) {
            write_value_wrapped(oss, histogram_bounds[i]);
        }

        // 物理相关 + 时间戳
        write_f64(oss, correlation);
        write_u64(oss, stats_ts);

        return oss.str();
    }

    std::optional<ColumnStats> ColumnStats::deserialize(const std::string& data) {
        if (data.size() < 6) // 5 magic + 1 version
            return std::nullopt;

        std::istringstream iss(data, std::ios::binary);

        // 魔数
        if (!check_magic(iss, kColumnStatsMagic))
            return std::nullopt;

        // 版本
        uint8_t version = 0;
        if (!read_u8(iss, version) || version != kFormatVersion)
            return std::nullopt;

        ColumnStats stats;

        // 列名 + 类型
        if (!read_string(iss, stats.column_name))
            return std::nullopt;
        uint8_t type_byte = 0;
        if (!read_u8(iss, type_byte))
            return std::nullopt;
        stats.type = static_cast<TypeKind>(type_byte);

        // 基础统计
        uint64_t total = 0, nulls = 0, ndv = 0;
        if (!read_u64(iss, total) || !read_u64(iss, nulls) || !read_f64(iss, stats.null_frac) ||
            !read_u64(iss, ndv))
            return std::nullopt;
        stats.total_rows = static_cast<std::size_t>(total);
        stats.null_count = static_cast<std::size_t>(nulls);
        stats.ndistinct = static_cast<std::size_t>(ndv);

        // MCV
        uint16_t mcv_count = 0;
        if (!read_u16(iss, mcv_count))
            return std::nullopt;
        stats.mcv_values.reserve(mcv_count);
        stats.mcv_freqs.reserve(mcv_count);
        for (uint16_t i = 0; i < mcv_count; ++i) {
            Value v;
            double freq = 0.0;
            if (!read_value_wrapped(iss, v) || !read_f64(iss, freq))
                return std::nullopt;
            stats.mcv_values.push_back(std::move(v));
            stats.mcv_freqs.push_back(freq);
        }

        // 直方图
        uint16_t hist_count = 0;
        if (!read_u16(iss, hist_count))
            return std::nullopt;
        stats.histogram_bounds.reserve(hist_count);
        for (uint16_t i = 0; i < hist_count; ++i) {
            Value v;
            if (!read_value_wrapped(iss, v))
                return std::nullopt;
            stats.histogram_bounds.push_back(std::move(v));
        }

        // 物理相关 + 时间戳
        if (!read_f64(iss, stats.correlation) || !read_u64(iss, stats.stats_ts))
            return std::nullopt;

        return stats;
    }

    // =========================================================================
    // TableStats
    // =========================================================================

    std::string TableStats::serialize() const {
        std::ostringstream oss(std::ios::binary);

        // 魔数 + 版本
        oss.write(kTableStatsMagic, 5);
        write_u8(oss, kFormatVersion);

        // 表名
        write_string(oss, table_name);

        // 基础信息
        write_u64(oss, static_cast<uint64_t>(total_rows));
        write_u64(oss, stats_ts);

        // 列统计数
        uint16_t col_count = static_cast<uint16_t>(columns.size());
        write_u16(oss, col_count);

        // 各列统计（每条前置长度前缀，便于反序列化容错）
        for (const auto& [name, col_stats] : columns) {
            std::string col_data = col_stats.serialize();
            write_u16(oss, static_cast<uint16_t>(col_data.size())); // 长度（2 字节足够，单列统计 < 64KB）
            oss.write(col_data.data(), static_cast<std::streamsize>(col_data.size()));
        }

        return oss.str();
    }

    std::optional<TableStats> TableStats::deserialize(const std::string& data) {
        if (data.size() < 8) // 5 magic + 1 version + 至少 2 字节
            return std::nullopt;

        std::istringstream iss(data, std::ios::binary);

        // 魔数
        if (!check_magic(iss, kTableStatsMagic))
            return std::nullopt;

        // 版本
        uint8_t version = 0;
        if (!read_u8(iss, version) || version != kFormatVersion)
            return std::nullopt;

        TableStats stats;

        // 表名
        if (!read_string(iss, stats.table_name))
            return std::nullopt;

        // 基础信息
        uint64_t total = 0;
        if (!read_u64(iss, total) || !read_u64(iss, stats.stats_ts))
            return std::nullopt;
        stats.total_rows = static_cast<std::size_t>(total);

        // 列统计数
        uint16_t col_count = 0;
        if (!read_u16(iss, col_count))
            return std::nullopt;

        // 各列统计
        for (uint16_t i = 0; i < col_count; ++i) {
            uint16_t col_len = 0;
            if (!read_u16(iss, col_len))
                return std::nullopt;
            std::string col_data(col_len, '\0');
            if (col_len > 0) {
                iss.read(col_data.data(), col_len);
                if (!iss)
                    return std::nullopt;
            }
            auto col_stats = ColumnStats::deserialize(col_data);
            if (!col_stats)
                return std::nullopt;
            stats.columns[col_stats->column_name] = std::move(*col_stats);
        }

        return stats;
    }

} // namespace corodb
