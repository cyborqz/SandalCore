// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "world/town_floor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aoc::worldd::world {

void TownFloor::set(int64_t node, const std::vector<Person>& people) {
    // the people in a bowl and their heights over the baked ground; the town's lift is the (upper) median
    std::vector<double> overs;
    for (const Person& p : people) {
        const double over = p.z - p.ground;
        if (std::isfinite(over) && std::isfinite(p.x) && std::isfinite(p.y) && over > kBowlCm) overs.push_back(over);
    }
    if (overs.empty()) {
        towns_.erase(node);
        return;
    }
    std::sort(overs.begin(), overs.end());
    Town town;
    town.lift = overs[overs.size() / 2];
    town.x0 = town.y0 = std::numeric_limits<double>::infinity();
    town.x1 = town.y1 = -std::numeric_limits<double>::infinity();
    for (const Person& p : people) {
        const double over = p.z - p.ground;
        if (!std::isfinite(over) || !std::isfinite(p.x) || !std::isfinite(p.y) || std::fabs(over - town.lift) > kSameCm) continue;
        town.persons.push_back(p);
        town.x0 = std::min(town.x0, p.x);
        town.y0 = std::min(town.y0, p.y);
        town.x1 = std::max(town.x1, p.x);
        town.y1 = std::max(town.y1, p.y);
    }
    towns_[node] = std::move(town);
}
// C Y B : C | O | A

std::optional<TownFloor::Floor> TownFloor::at(double x, double y, double ground) const {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(ground)) return std::nullopt;
    std::optional<Floor> best;
    double best_d2 = kReachCm * kReachCm;
    for (const auto& [node, town] : towns_) {
        if (x < town.x0 - kReachCm || x > town.x1 + kReachCm || y < town.y0 - kReachCm || y > town.y1 + kReachCm) continue;
        for (const Person& p : town.persons) {
            const double dx = p.x - x, dy = p.y - y;
            const double d2 = dx * dx + dy * dy;
            if (d2 > best_d2 || std::fabs(ground - p.ground) > kReliefCm) continue;
            best_d2 = d2;
            best = Floor{node, town.lift};
        }
    }
    return best;
}

size_t TownFloor::floor_persons(int64_t node) const {
    const auto it = towns_.find(node);
    return it == towns_.end() ? 0 : it->second.persons.size();
}

std::optional<double> TownFloor::lift_of(int64_t node) const {
    const auto it = towns_.find(node);
    if (it == towns_.end()) return std::nullopt;
    return it->second.lift;
}

}  // namespace aoc::worldd::world
