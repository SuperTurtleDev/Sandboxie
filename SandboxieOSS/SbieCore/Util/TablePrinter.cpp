// Sandboxie-OSS — SbieCore/Util/TablePrinter.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors

#include "TablePrinter.h"

namespace sbie::util {

void TablePrinter::AddColumn(const std::wstring& header, bool rightAlign)
{
    cols_.push_back(Column{ header, rightAlign });
}

void TablePrinter::AddRow(const std::vector<std::wstring>& cells)
{
    rows_.push_back(cells);
    // 行长不足补空单元格，保证列对齐
    if (rows_.back().size() < cols_.size())
        rows_.back().resize(cols_.size(), std::wstring());
}

std::wstring TablePrinter::Render(bool withHeader) const
{
    std::vector<size_t> width(cols_.size(), 0);
    if (withHeader) {
        for (size_t c = 0; c < cols_.size(); ++c)
            width[c] = cols_[c].header.size();
    }
    for (const auto& row : rows_) {
        for (size_t c = 0; c < row.size() && c < width.size(); ++c)
            if (row[c].size() > width[c])
                width[c] = row[c].size();
    }

    auto pad = [](const std::wstring& s, size_t w, bool right) -> std::wstring {
        if (s.size() >= w)
            return s;
        return right ? std::wstring(w - s.size(), L' ') + s
                     : s + std::wstring(w - s.size(), L' ');
    };

    std::wstring out;
    if (withHeader) {
        for (size_t c = 0; c < cols_.size(); ++c) {
            if (c) out += L"  ";
            out += pad(cols_[c].header, width[c], false);
        }
        out += L'\n';
    }
    for (const auto& row : rows_) {
        for (size_t c = 0; c < cols_.size(); ++c) {
            if (c) out += L"  ";
            out += pad(c < row.size() ? row[c] : std::wstring(), width[c],
                       cols_[c].rightAlign);
        }
        out += L'\n';
    }
    return out;
}

} // namespace sbie::util
