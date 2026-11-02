// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/scan_order.h
//
//  The order of a view's scan.  Every 67 ms the view lists the actors round its player, each with its distance, and puts them nearest first (a tie goes by id).  Two lists 67 ms
//  apart are nearly the same order: the player walks 40 cm, a creature a little more, and a few actors change places.  sort_after_small_changes starts from the order of the last
//  list and puts it right: a walk through the list and a move for each pair that changed places, instead of a sort of all of it.
//
//  The result is the order std::sort gives: `less` is a strict total order (no two elements are equivalent: the ids differ), and the sorted order of such a set is unique, whatever
//  the way to it.  A list that is far from in order (the player jumped, or most of the actors are new) is given to std::sort when the moves pass a budget.
// ============================================================================
#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace aoc::worldd {

/// Puts `v` in the order of `less`, a strict total order.  The first `ordered` elements are expected nearly in order already (the actors of the last pass, in its order); the rest
/// are in no order (the actors the last pass did not have).  `spare` is a buffer that is used and handed back (its memory is kept for the next call).
template <class T, class Less>
void sort_after_small_changes(std::vector<T>& v, size_t ordered, Less less, std::vector<T>& spare) {
    const size_t n = v.size();
    const size_t m = ordered < n ? ordered : n;
    // 1. the first m elements: insertion sort, while it stays cheap.  Each element moves as far as the order changed round it; past the budget the list is far from in order and
    //    std::sort takes it (every step leaves a permutation, so it may take over at any point).
    const size_t budget = 6 * m + 64;
    size_t moves = 0;
    bool cheap = true;
    for (size_t i = 1; i < m; ++i) {
        if (!less(v[i], v[i - 1])) continue;
        T x = std::move(v[i]);
        size_t j = i;
        do {
            v[j] = std::move(v[j - 1]);
            --j;
        } while (j > 0 && less(x, v[j - 1]));
        v[j] = std::move(x);
        moves += i - j;
        if (moves > budget) {
            cheap = false;
            break;
        }
    }
    if (!cheap) std::sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), less);
    if (n == m) return;
    // 2. the rest: no order to start from
    if (m == 0) {
        std::sort(v.begin(), v.end(), less);
        return;
    }
    const size_t fresh = n - m;
    if (fresh <= 8) {   // a few: each goes to its place (a binary search, one rotation)
        for (size_t i = m; i < n; ++i) {
            const auto at = std::upper_bound(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v[i], less);
            std::rotate(at, v.begin() + static_cast<std::ptrdiff_t>(i), v.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        }
        return;
    }
    std::sort(v.begin() + static_cast<std::ptrdiff_t>(m), v.end(), less);   // many: sorted, then merged with the first part
    spare.clear();
    spare.reserve(n);
    std::merge(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.begin() + static_cast<std::ptrdiff_t>(m), v.end(), std::back_inserter(spare), less);
    v.swap(spare);
}

}  // namespace aoc::worldd
