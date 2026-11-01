// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "SourceResultHistory.hpp"
#include <utility>

namespace openswmm::twoD {
// Appenders only grow existing numeric columns. Keep their prior extents and
// checkpoint anchor until validation succeeds; rollback never allocates.
class ResultHistoryAppend {
public:
    ResultHistoryAppend(std::map<std::string, SourceResultTable>& tables,
                        std::string& boundary) : boundary_(boundary) {
        std::size_t count = 0;
        for (const auto& [name, table] : tables)
            count += table.columns.size();
        columns_.reserve(count);
        for (auto& [name, table] : tables)
            for (auto& column : table.columns)
                columns_.emplace_back(&column, column.size());
    }
    ResultHistoryAppend(const ResultHistoryAppend&) = delete;
    ResultHistoryAppend& operator=(const ResultHistoryAppend&) = delete;
    ~ResultHistoryAppend() {
        if (!committed_) {
            for (auto [column, size] : columns_)
                column->resize(size);
            if (replaced_)
                boundary_.swap(previous_boundary_);
        }
    }
    void replaceBoundary(std::string& next) noexcept {
        previous_boundary_.swap(boundary_);
        boundary_.swap(next);
        replaced_ = true;
    }
    void commit() noexcept { committed_ = true; }

private:
    std::vector<std::pair<std::vector<double>*, std::size_t>> columns_;
    std::string& boundary_;
    std::string previous_boundary_;
    bool replaced_ = false, committed_ = false;
};
}
