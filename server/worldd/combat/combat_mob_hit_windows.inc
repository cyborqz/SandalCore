// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_mob_hit_windows.inc  (the lane's milestone 2)
//
//  A creature's attack whose montage lands its hit from a hit-window notify state: the state is open for a stretch of the montage and the game lands the hit on every tick of it, on
//  the players the hit's targeting record reaches, once for each and not again while the window is open (`hit_windows` in npc_montages.tsv); a state that allows several hits lands the
//  hit on a player who stays in reach again after its interval.  A player who comes in during the window is hit then; one who leaves before a tick reaches him is not.
//
//  A montage an ability enters on a branch (`branches` in npc_abilities.tsv): the conditional branch of the Mycelions' Launch is bound to the movement event RootMotionObstructed, which
//  happens when a body in the ability's freeze box holds the creature its montage carries.  The client plays the branch's montage by itself from the ability record; the world watches the
//  creature's root motion, and from the moment it is obstructed the branch montage's hit notifies land the ability's hits at their own times.  A launch that nothing obstructs plays out and
//  lands no impact.
//
//  Included by combat_world.cpp (its own file: combat_world.cpp stays in its own functions).
// ============================================================================

void CombatWorld::plan_windows(const abilities::AbilityDef& def, const MontageData& montage, CastPlan& out) {
    if (!anim_) return;
    const double rate = montage.rate_scale > 0 ? montage.rate_scale : 1.0;
    for (const MontageData::Window& w : montage.windows) {
        if (!(w.end >= w.start)) continue;
        const BiteArea* area = anim_->area_of(def.row.guid, w.key);
        if (!area) continue;
        for (const content::AbilityHitRow& r : def.hits) {
            if (r.source != "hit" || r.key != w.key) continue;
            out.windows.push_back({w.start / rate, w.end / rate, r.hit_guid, static_cast<int>(r.key), w.interval, *area});
            break;
        }
    }
    std::stable_sort(out.windows.begin(), out.windows.end(), [](const CastPlan::PlannedWindow& a, const CastPlan::PlannedWindow& b) { return a.start < b.start; });
}

void CombatWorld::plan_branch_hits(const abilities::AbilityDef& def, const MontageData& montage, CastPlan& out) {
    if (!anim_ || montage.root_motion.empty()) return;   // (only a montage that carries the creature can be obstructed)
    const std::vector<AbilityBranch>* branches = anim_->branches_of(def.row.guid);
    if (!branches) return;
    for (const AbilityBranch& branch : *branches) {
        if (branch.event != "obstructed") continue;
        const MontageData* played = anim_->montage(branch.montage);
        if (!played || played == &montage || played->rate_scale <= 0) continue;
        for (const MontageData::Hit& h : played->hits) {
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source != "hit" || r.key != h.key) continue;
                out.branch_hits.push_back({h.time / played->rate_scale, r.hit_guid, static_cast<int>(r.key)});
                break;
            }
        }
    }
    std::stable_sort(out.branch_hits.begin(), out.branch_hits.end(), [](const PlannedHit& a, const PlannedHit& b) { return a.delay < b.delay; });
}

void CombatWorld::queue_windows(const CastPlan& plan, Pending& p, double now) {
    for (const CastPlan::PlannedWindow& w : plan.windows) {
        Pending::PendingWindow pending;
        pending.start = now + std::max(w.start, kMinHitDelay);   // (the same floor the point notifies use: not on the tick the cast began)
        pending.end = std::min(now + w.end, p.end_at);            // (a window cannot outlive the montage that holds it)
        if (pending.start > pending.end) continue;
        pending.hit = w.hit;
        pending.area = w.area;
        pending.interval = w.interval;
        p.windows.push_back(std::move(pending));
    }
}

void CombatWorld::queue_branch(const CastPlan& plan, Pending& p) {
    for (const PlannedHit& h : plan.branch_hits) {
        Pending::PendingBranch b;
        b.delay = std::max(h.delay, 0.0);
        b.hit = h.hit;
        const BiteArea* area = anim_ ? anim_->area_of(p.ability->row.guid, h.key) : nullptr;
        b.area = area ? *area : BiteArea{};
        p.branch.push_back(std::move(b));
    }
    p.branch_watch = !p.branch.empty();
}

bool CombatWorld::window_open(const Pending& p, double now) const {
    for (const Pending::PendingWindow& w : p.windows) {
        if (now >= w.start && now <= w.end) return true;
    }
    return false;
}

bool CombatWorld::looks_every_tick(const Pending& p, double now) const {
    return p.branch_watch || window_open(p, now);
}

void CombatWorld::watch_branch(Npc& n, Pending& p, double now) {
    if (!p.branch_watch) return;
    // carried no more: the montage's travel is over and nothing obstructed it, so the branch's event never happened (the launch lands no impact)
    if (!npcs_world_ || !npcs_world_->root_motion_active(n.actor_id)) {
        p.branch_watch = false;
        return;
    }
    if (!npcs_world_->root_motion_obstructed(n.actor_id)) return;
    p.branch_watch = false;
    // the branch montage's hits fall due after their own delays, in time order with the hits that are still to come
    struct Due {
        double   at;
        int64_t  hit;
        BiteArea area;
    };
    std::vector<Due> rest;
    for (size_t i = p.next_hit; i < p.timed_hits.size(); ++i) rest.push_back({p.timed_hits[i].first, p.timed_hits[i].second, p.timed_areas[i]});
    for (const Pending::PendingBranch& b : p.branch) rest.push_back({now + b.delay, b.hit, b.area});
    std::stable_sort(rest.begin(), rest.end(), [](const Due& a, const Due& b) { return a.at < b.at; });
    p.timed_hits.resize(p.next_hit);
    p.timed_areas.resize(p.next_hit);
    for (Due& d : rest) {
        p.timed_hits.emplace_back(d.at, d.hit);
        p.timed_areas.push_back(std::move(d.area));
    }
}

void CombatWorld::sweep_windows(Npc& n, Pending& p, double now) {
    for (Pending::PendingWindow& w : p.windows) {
        if (now < w.start || now > w.end || w.hit == 0) continue;
        // the target part of the record finds the creature's target or the one the cast was aimed at; its volumes reach every player inside them
        const uint64_t aimed = w.area.has_target_part() ? aimed_at(n, w.area, p.target, p.by_pick) : 0;
        const std::vector<PlayerPort*> candidates = players_;
        for (PlayerPort* q : candidates) {
            if (!q->fighter().alive()) continue;
            // a player the window has hit is not hit again, unless the state allows several hits and its interval has passed
            const auto seen = w.struck.find(q->actor_id());
            if (seen != w.struck.end() && !(w.interval > 0 && now - seen->second >= w.interval - 1e-6)) continue;
            if (!hit_reaches(n, q->fighter(), w.area, aimed != 0 && q->actor_id() == aimed)) continue;
            // the hit's own effects on the creature itself (a rage, a tracker) land once for the window, with the first player it reaches
            if (!w.self_landed) {
                if (const abilities::HitDef* hit = catalog_.hit(w.hit); hit && (hit->row.applicable_targets & 1) != 0) land_self_effects(n, *hit, now);
                w.self_landed = true;
            }
            deliver_hit(n, *q, *p.ability, w.hit, now);
            w.struck[q->actor_id()] = now;
        }
    }
}
