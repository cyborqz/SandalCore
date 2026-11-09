// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/ability_pipeline_chain.inc: a hit that names itself in its RecursiveHit jumps to the enemies near the one it hit (Chain Lightning).
//  Included at the end of ability_pipeline.cpp so that the chain has its own file; the rest of the pipeline does not change.
// ============================================================================

namespace {

// The hits that chain to the next enemy when they land: the hit names itself in its RecursiveHit and the recursive targeting's sphere says how far a jump reaches.
// Chain Lightning: the primary and up to five jumps, the same hit each time and never the same enemy twice [D: MaxHitsGlobally 6, MaxHitsPerTarget 1, MaxTargetsPerHit 1;
// R: the game's two confirmations of Mage_ChainLightning_Hit at the same millisecond, one target each]. The Earth passive's Aftershock
// (Mage_ChainLightning_Secondary_2) is not listed: its hit is self-recursive too, but the passive is not modelled here.
struct ChainRule {
    const char* hit;
    int         max_hits;   // the primary and every jump, in all
};
constexpr ChainRule kChains[] = {
    {"Mage_ChainLightning_Hit", 6},
};

}  // namespace

void AbilityPipeline::chain_from(AbilityChange& c, const HitDef& hit, const std::vector<uint64_t>& first, double t, const HitSource& src, const AbilityDef* ability) {
    if (!combat_ || chaining_ || first.empty() || src.type == content::kSourceTypeProjectile) return;
    const ChainRule* rule = nullptr;
    for (const ChainRule& r : kChains) {
        if (hit.row.name == r.hit) rule = &r;
    }
    if (!rule) return;
    double radius = 0;
    for (const content::RecursiveHitRow& r : hit.recursive) {
        if (r.child_hit == hit.row.guid) radius = r.radius_cm;
    }
    if (!(radius > 0)) return;
    // the enemies this cast has hit: the primary and every jump; the next one is the nearest of the others within the sphere of the one just hit
    std::set<uint64_t> reached(first.begin(), first.end());
    uint64_t from = first.front();
    chaining_ = true;
    while (static_cast<int>(reached.size()) < rule->max_hits) {
        const combat::TargetInfo at = combat_->inspect(from);
        if (!at.exists) break;
        uint64_t next = 0;
        double best = 0;
        for (const combat::GatheredTarget& e : combat_->enemies_around(at.position, radius, kMaxGathered)) {
            if (reached.count(e.actor)) continue;
            if (next == 0 || e.distance_cm < best) {
                next = e.actor;
                best = e.distance_cm;
            }
        }
        if (next == 0) break;
        reached.insert(next);
        HitSource fired = src;
        fired.autonomous = false;   // [R: the jump's confirmation is predicted on neither side, pa 0 ps 0]
        fired.simulated = false;
        fired.response = false;
        fired.reach_free = true;    // (the jump reaches the enemy near the one just hit, wherever the caster stands by now)
        apply_hit_on_targets(c, hit, {next}, t, fired, ability);
        from = next;
    }
    chaining_ = false;
}
