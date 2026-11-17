// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/ability_pipeline_resurrect.inc: the Resurrection's cast (Social, seeing other players: their death)
//  Included at the end of session/ability_pipeline.cpp.  The hit's own part (the life states it reaches, the dead player it raises, the request it leaves for
//  the session) is in ability_pipeline.cpp's trigger() and apply_hit_on_targets(); this file only answers whether a running cast is the one a
//  ServerResurrect confirms.
// ============================================================================

int64_t AbilityPipeline::resurrect_cast_for(uint64_t target) const {
    if (!catalog_) return 0;
    for (const Cast& cast : casts_) {
        if (!cast.def) continue;
        const HitDef* hit = abilities::resurrect_hit_of(*catalog_, *cast.def);
        if (!hit) continue;
        if (std::find(cast.target_actors.begin(), cast.target_actors.end(), target) == cast.target_actors.end()) continue;
        return hit->resurrect;
    }
    return 0;
}
