// Sandboxie-OSS — SbieCore/Util/TablePrinter.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 人类可读表格输出（04-modules.md §7.1）：
//   列间两空格、左对齐（数字列右对齐）、表头仅当结果 ≥1 行且非 --quiet。

#pragma once

#include <string>
#include <vector>

namespace sbie::util {

class TablePrinter {
public:
    struct Column {
        std::wstring header;
        bool rightAlign = false;
    };

    void AddColumn(const std::wstring& header, bool rightAlign = false);
    void AddRow(const std::vector<std::wstring>& cells);
    bool Empty() const { return rows_.empty(); }
    size_t RowCount() const { return rows_.size(); }

    // withHeader=false 对应 --quiet（只输出数据行）
    std::wstring Render(bool withHeader) const;

private:
    std::vector<Column> cols_;
    std::vector<std::vector<std::wstring>> rows_;
};

} // namespace sbie::util
