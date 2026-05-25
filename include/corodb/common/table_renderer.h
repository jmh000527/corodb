// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file table_renderer.h @brief ASCII 表格渲染接口。 */

#pragma once

#include <string>
#include <vector>

namespace corodb {

    /** @brief ASCII 表格渲染配置选项。 */
    struct TableRenderOptions {
        std::string indent = "  ";      ///< 表格前的缩进字符串（默认两个空格）。
        std::size_t max_col_width = 60; ///< 列的最大宽度（默认 60 字符）。
        bool truncate = true;           ///< 超长内容是否截断并加省略号（默认 true）。
    };

    /**
     * @brief 将数据渲染为 ASCII 表格字符串。
     * @param headers 列标题向量
     * @param rows 数据行，每行是一个字符串向量
     * @param right_align 每列的对齐方式（true=右对齐，false=左对齐）
     * @param opt 渲染选项
     */
    std::string render_ascii_table(const std::vector<std::string>& headers,
                                   const std::vector<std::vector<std::string>>& rows,
                                   const std::vector<bool>& right_align, const TableRenderOptions& opt);

    /**
     * @brief 根据数据内容自动推断各列对齐方式。
     * @param rows 数据行向量
     * @param col_count 列数量
     */
    std::vector<bool> guess_alignment(const std::vector<std::vector<std::string>>& rows, std::size_t col_count);

} // namespace corodb
