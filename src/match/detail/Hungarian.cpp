// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/detail/Hungarian.h"

#include <algorithm>
#include <limits>

namespace selecta::match::detail {

namespace {

// Rows ≤ cols. Potentials u (rows) and v (columns), 1-based with a virtual column 0; owner[j] is the row holding
// column j. Returns the column of every row.
std::vector<qsizetype> assignRows(const std::vector<double> &cost, qsizetype rows, qsizetype cols, bool transposed)
{
    const double infinity = std::numeric_limits<double>::infinity();
    const auto at = [&cost, rows, cols, transposed](qsizetype row, qsizetype col) {
        return transposed ? cost[static_cast<std::size_t>((col * rows) + row)]
                          : cost[static_cast<std::size_t>((row * cols) + col)];
    };
    const auto n = static_cast<std::size_t>(rows);
    const auto m = static_cast<std::size_t>(cols);
    std::vector<double> u(n + 1, 0.0);
    std::vector<double> v(m + 1, 0.0);
    std::vector<std::size_t> owner(m + 1, 0);
    std::vector<std::size_t> way(m + 1, 0);
    std::vector<double> slack(m + 1);
    std::vector<char> used(m + 1);
    for (std::size_t row = 1; row <= n; ++row) {
        owner[0] = row;
        std::size_t col0 = 0;
        std::ranges::fill(slack, infinity);
        std::ranges::fill(used, char{0});
        do {
            used[col0] = 1;
            const std::size_t row0 = owner[col0];
            double delta = infinity;
            std::size_t col1 = 0;
            for (std::size_t col = 1; col <= m; ++col) {
                if (used[col] != 0) {
                    continue;
                }
                const double reduced =
                    at(static_cast<qsizetype>(row0 - 1), static_cast<qsizetype>(col - 1)) - u[row0] - v[col];
                if (reduced < slack[col]) {
                    slack[col] = reduced;
                    way[col] = col0;
                }
                if (slack[col] < delta) {
                    delta = slack[col];
                    col1 = col;
                }
            }
            for (std::size_t col = 0; col <= m; ++col) {
                if (used[col] != 0) {
                    u[owner[col]] += delta;
                    v[col] -= delta;
                } else {
                    slack[col] -= delta;
                }
            }
            col0 = col1;
        } while (owner[col0] != 0);
        do {
            const std::size_t previous = way[col0];
            owner[col0] = owner[previous];
            col0 = previous;
        } while (col0 != 0);
    }
    std::vector<qsizetype> columnOf(n, -1);
    for (std::size_t col = 1; col <= m; ++col) {
        if (owner[col] != 0) {
            columnOf[owner[col] - 1] = static_cast<qsizetype>(col - 1);
        }
    }
    return columnOf;
}

} // namespace

std::vector<std::pair<qsizetype, qsizetype>> minimumCostAssignment(const std::vector<double> &cost, qsizetype rows,
                                                                   qsizetype cols)
{
    std::vector<std::pair<qsizetype, qsizetype>> pairs;
    if (rows <= 0 || cols <= 0 || static_cast<qsizetype>(cost.size()) < rows * cols) {
        return pairs;
    }
    const bool transposed = rows > cols; // the algorithm wants no more rows than columns
    const qsizetype small = transposed ? cols : rows;
    const qsizetype large = transposed ? rows : cols;
    const std::vector<qsizetype> columnOf = assignRows(cost, small, large, transposed);
    for (qsizetype i = 0; i < small; ++i) {
        const qsizetype j = columnOf[static_cast<std::size_t>(i)];
        if (j >= 0) {
            pairs.emplace_back(transposed ? j : i, transposed ? i : j);
        }
    }
    std::ranges::sort(pairs);
    return pairs;
}

} // namespace selecta::match::detail
