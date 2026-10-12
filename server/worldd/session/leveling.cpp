// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/leveling.cpp (see leveling.h)
// ============================================================================
#include "session/leveling.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::leveling {

std::optional<LevelCurve> LevelCurve::from(const content::LevelingType& type, std::string& error) {
    LevelCurve c;
    c.guid_ = type.guid;
    if (type.curve.empty() || type.curve.front().first != 1) {
        error = "the level curve of " + type.name + " does not start at level 1";
        return std::nullopt;
    }
    int expect = 1;
    int64_t last = -1;
    for (const auto& [level, xp] : type.curve) {
        if (level != expect || xp < last) {
            error = "the level curve of " + type.name + " skips a level or goes down at level " + std::to_string(level);
            return std::nullopt;
        }
        c.thresholds_.push_back(xp);
        last = xp;
        ++expect;
    }
    c.cap_ = type.level_cap > 0 ? type.level_cap : c.max_level();
    if (c.cap_ > c.max_level() || c.cap_ < 1) {
        error = "the level cap " + std::to_string(type.level_cap) + " of " + type.name + " is past its curve of " + std::to_string(c.max_level()) + " levels";
        return std::nullopt;
    }
    return c;
}

int64_t LevelCurve::xp_for(int level) const {
    if (level <= 1) return thresholds_.front();
    if (level > max_level()) return thresholds_.back();
    return thresholds_[static_cast<size_t>(level - 1)];
}

int LevelCurve::level_of(int64_t xp) const {
    int level = 1;
    for (size_t i = 0; i < thresholds_.size(); ++i) {
        if (xp >= thresholds_[i]) level = static_cast<int>(i + 1);
        else break;
    }
    return std::min(level, cap_);
}

int64_t LevelCurve::max_xp() const {
    if (cap_ < max_level()) return thresholds_[static_cast<size_t>(cap_)] - 1;   // just below the level past the cap
    return thresholds_.back();
}
// CoA x C Y B

int64_t LevelCurve::span_of(int level) const {
    const int l = std::clamp(level, 1, max_level());
    if (l < max_level()) return xp_for(l + 1) - xp_for(l);
    return l > 1 ? xp_for(l) - xp_for(l - 1) : 0;
}

std::optional<int64_t> scroll_xp(const LevelCurve& curve, int target_level, const Progress& p, double xp_add_mult, double margin) {
    if (target_level < 1 || target_level > curve.max_level()) return std::nullopt;
    if (p.level >= target_level) return 0;
    const double span = static_cast<double>(curve.span_of(p.level));
    const double ratio = span > 0 ? static_cast<double>(p.debt) / span : 0.0;
    const double xp = static_cast<double>(curve.xp_for(target_level)) + ratio * span;
    const double cur = static_cast<double>(curve.xp_for(p.level));
    const double mult = std::max(0.001, 1.0 + xp_add_mult);
    const double v = (xp - cur) / mult + margin;
    if (!std::isfinite(v) || v <= 0) return 0;
    if (v >= 9.0e18) return std::nullopt;
    return static_cast<int64_t>(std::floor(v));
}

Progress with_death_debt(const LevelCurve& curve, const Progress& p, double share) {
    Progress out = p;
    const int64_t span = curve.span_of(p.level);
    const int64_t add = static_cast<int64_t>(std::llround(static_cast<double>(span) * share));
    out.debt = std::min<int64_t>(span, std::max<int64_t>(0, p.debt) + std::max<int64_t>(0, add));
    return out;
}

std::vector<const content::AutoGrant*> granted_by(std::span<const content::AutoGrant> grants, int level) {
    std::vector<const content::AutoGrant*> out;
    for (const content::AutoGrant& g : grants) {
        if (g.level_requirement >= 1 && g.level_requirement <= level) out.push_back(&g);
    }
    std::stable_sort(out.begin(), out.end(), [](const content::AutoGrant* a, const content::AutoGrant* b) {
        return a->level_requirement != b->level_requirement ? a->level_requirement < b->level_requirement : a->node_guid < b->node_guid;
    });
    return out;
}

XpResult add_xp(const LevelCurve& curve, std::span<const content::AutoGrant> grants, const Progress& p, int64_t delta) {
    XpResult r;
    r.before = p;
    Progress n = p;
    if (delta > 0) {
        const int64_t paid = std::min(delta, n.debt);
        n.debt -= paid;
        const int64_t rest = delta - paid;
        n.xp = rest > curve.max_xp() - n.xp ? curve.max_xp() : n.xp + rest;   // held at the most the tracker holds, no overflow
    }
    n.level = std::max(n.level, curve.level_of(n.xp));   // a level never goes down (a record whose XP trails its level keeps it)
    n.level = std::min(n.level, curve.cap());
    r.after = n;
    if (n.level > p.level) {
        for (const content::AutoGrant* g : granted_by(grants, n.level)) {
            if (g->level_requirement > p.level) r.granted.push_back(g);
        }
    }
    return r;
}

Progress progress_of(const LevelCurve& curve, int level, std::optional<int64_t> xp, int64_t debt) {
    Progress p;
    p.level = std::clamp(level, 1, curve.cap());
    const int64_t threshold = curve.xp_for(p.level);
    p.xp = xp && *xp >= threshold ? std::min(*xp, curve.max_xp()) : threshold;
    p.debt = std::max<int64_t>(0, debt);
    return p;
}

}  // namespace aoc::worldd::leveling
