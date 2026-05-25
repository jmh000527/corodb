// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file table_renderer.cpp
// @brief ASCII 表格渲染器的实现。

#include "corodb/common/table_renderer.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iomanip>
#include <sstream>

namespace corodb {

    namespace {
        std::string maybe_truncate(std::string s, std::size_t max_len) {
            if (max_len == 0)
                return {};
            if (s.size() <= max_len)
                return s;
            if (max_len <= 3)
                return std::string(max_len, '.');
            s.resize(max_len - 3);
            s += "...";
            return s;
        }

        std::string render_border(const std::vector<std::size_t>& widths, const std::string& indent) {
            std::ostringstream oss;
            oss << indent << '+';
            for (std::size_t i = 0; i < widths.size(); ++i) {
                oss << std::string(widths[i] + 2, '-');
                oss << '+';
            }
            oss << "\n";
            return oss.str();
        }

        std::string render_row(const std::vector<std::string>& row, const std::vector<std::size_t>& widths,
                               const std::vector<bool>& right_align, const std::string& indent) {
            std::ostringstream oss;
            oss << indent << '|';
            for (std::size_t i = 0; i < widths.size(); ++i) {
                const std::string& cell = (i < row.size()) ? row[i] : std::string{};
                oss << ' ';
                if (i < right_align.size() && right_align[i]) {
                    oss << std::format("{:>{}}", cell, widths[i]);
                } else {
                    oss << std::format("{:<{}}", cell, widths[i]);
                }
                oss << ' ';
                oss << '|';
            }
            oss << "\n";
            return oss.str();
        }
    } // namespace

    /**
     * @brief 渲染ASCII表格
     * @param headers 表头列名列表
     * @param rows 数据行列表
     * @param right_align 每列是否右对齐
     * @param opt 渲染选项
     * @return 渲染后的ASCII表格字符串
     */
    std::string render_ascii_table(const std::vector<std::string>& headers,
                                   const std::vector<std::vector<std::string>>& rows,
                                   const std::vector<bool>& right_align, const TableRenderOptions& opt) {
        if (headers.empty())
            return {}; // 无表头则返回空

        // 计算每列的宽度（初始为表头宽度）
        std::vector<std::size_t> widths(headers.size(), 0);
        for (std::size_t i = 0; i < headers.size(); ++i)
            widths[i] = headers[i].size();

        // 根据数据内容更新列宽度（取最大值）
        for (const auto& row: rows) {
            for (std::size_t i = 0; i < widths.size() && i < row.size(); ++i) {
                widths[i] = std::max(widths[i], row[i].size());
            }
        }

        // 应用最大列宽限制
        if (opt.truncate && opt.max_col_width > 0) {
            for (auto& w: widths)
                w = std::min<std::size_t>(w, opt.max_col_width);
        }

        // 处理表头截断
        std::vector<std::string> hdr = headers;
        if (opt.truncate && opt.max_col_width > 0) {
            for (auto& h: hdr)
                h = maybe_truncate(h, opt.max_col_width);
        }

        // 构建输出
        std::ostringstream oss;
        oss << render_border(widths, opt.indent);                        // 顶部边框
        oss << render_row(hdr, widths, std::vector<bool>{}, opt.indent); // 表头行
        oss << render_border(widths, opt.indent);                        // 表头分隔线

        // 渲染数据行
        for (const auto& row: rows) {
            std::vector<std::string> out_row;
            out_row.reserve(widths.size());
            for (std::size_t i = 0; i < widths.size(); ++i) {
                std::string cell = (i < row.size()) ? row[i] : std::string{};
                if (opt.truncate && opt.max_col_width > 0)
                    cell = maybe_truncate(cell, opt.max_col_width); // 截断超长内容
                out_row.push_back(std::move(cell));
            }
            oss << render_row(out_row, widths, right_align, opt.indent);
        }
        oss << render_border(widths, opt.indent); // 底部边框
        return oss.str();
    }

    /**
     * @brief 根据数据内容推断列对齐方式
     *
     * 如果列中所有非NULL值都是数字，则右对齐；否则左对齐
     * @param rows 数据行列表
     * @param col_count 列数量
     * @return 每列的对齐方式（true=右对齐，false=左对齐）
     */
    std::vector<bool> guess_alignment(const std::vector<std::vector<std::string>>& rows, std::size_t col_count) {
        std::vector<bool> right_align(col_count, true); // 默认右对齐
        for (size_t c = 0; c < col_count; ++c) {
            for (const auto& row: rows) {
                if (c >= row.size())
                    continue;
                const auto& cell = row[c];
                if (cell == "NULL")
                    continue; // NULL值不参与判断
                // 检查是否为数字：首字符为数字或负号
                if (cell.empty() || (!std::isdigit(cell[0]) && cell[0] != '-')) {
                    right_align[c] = false; // 非数字，改为左对齐
                    break;
                }
                // 检查剩余字符是否都是数字
                if (!std::all_of(cell.begin() + 1, cell.end(), ::isdigit)) {
                    right_align[c] = false;
                    break;
                }
            }
        }
        return right_align;
    }

} // namespace corodb
