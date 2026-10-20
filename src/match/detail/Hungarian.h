// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtGlobal>

#include <utility>
#include <vector>

// Internal: the assignment problem for AlbumMatcher's track mapping (note §8 step 3; beets lapjv, Lidarr Munkres).
namespace selecta::match::detail {

// A minimum-cost assignment of min(rows, cols) pairs for a rows × cols cost matrix (row-major, finite costs): the
// (row, column) pairs, by row. Kuhn–Munkres with potentials, O(n² m) for n = min(rows, cols); ties go to the lower
// column, so the result is deterministic. Pure; any thread.
std::vector<std::pair<qsizetype, qsizetype>> minimumCostAssignment(const std::vector<double> &cost, qsizetype rows,
                                                                   qsizetype cols);

} // namespace selecta::match::detail
