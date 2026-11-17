// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "spawns/hand_placed.h"

#include <utility>

namespace aoc::worldd::spawns {

const std::vector<HandPlaced>& hand_placed() {
    static const std::vector<HandPlaced> points = {
        // The Hole's boss room (PopSet_TheHole_Bosses), 3 m east of the Rotting Mass, at the Misbegotten's level 20
        {"AR_HandPlaced_RVR_TheHole_TheMisbegotten", 6064632269219037194, 0, 6064632156627861519, -973586.0, 529962.0, 15284.0, 0.0, 20},
        // Varnok Forge's bosses (ANV_Pocket_Dungeon_Varnok_Forge_BOSSES), 3 m east of Coalgrim the Fire-Fettered, at level 10 (Magnus's own instance has no level and a
        // 3-minute respawn: Coalgrim's settings are taken instead)
        {"AR_HandPlaced_ANV_VarnokForge_MagnusTheCinderboundColossus", 6064633667890774026, 0, 6064633787211317251, -815411.0, -657957.0, 19870.0, 42.0, 10},
        // The Wreckage of Carphin's residential district (RVR_WoC_Residential_District), 3 m east of the Wailing Widow, at Carrig's level 21
        {"AR_HandPlaced_RVR_Carphin_FinancierCarrig", 6064630911082103055, 0, 6064630270761779576, -481999.0, 448681.0, 16127.0, 0.0, 21},
        // The Bloomed Maw (Jundark), 3 m east of the Guardner, at the Lore Warden's level 39
        {"AR_HandPlaced_JUN_BloomedMaw_LoreWarden", 0, 6064633458826870786, 6064633430216933377, -1071918.0, 1180535.0, 11525.0, 0.0, 39},
        // Azmaran's wilderness, 3 m east of one of the world's only three 3-star mummy Berserkers (the rare's Blueprint, Minotaur_Berserker_Mummy2_RareSpawn, is their
        // Minotaur_Berserker_Mummy2's rare variant), with Bartok the Heartless's instance settings (the Vault of the Emigrant's mummy rare: level 15, a 15-minute respawn)
        {"AR_HandPlaced_SAN_Azmaran_MummyBerserkerRare", 0, 6064632942955266052, 6064632953500467208, -338842.0, 874593.0, 8957.0, 0.0, 15,
         6064632991428771946},   // (no named table of its own: the mummy Berserkers' Minotaur_Berserker_Mummy, not Bartok's)
        // Three named mobs the data places nowhere, with their own Blueprints and named drop tables (modelled homes, chosen by the user on 17 November): Tarnished Sevro, a
        // Minotaur Berserker, 3 m east of Kavrok the Taxed with Gallowsgate's other minotaur named mobs (level 16, Kavrok's settings)
        {"AR_HandPlaced_ANV_Gallowsgate_TarnishedSevro", 0, 6064631883872141339, 6064633787934113796, -1366616.0, -376931.0, 10545.0, 0.0, 16, 6064632283100217413},
        // the Windswept Watcher, a raven, among Gravepeak's level 21-22 ravens (Corvus's settings: a 20-minute respawn)
        {"AR_HandPlaced_RVR_Gravepeak_WindsweptWatcher", 0, 6064631884117508126, 6064630270761775551, -836245.0, 229669.0, 26694.0, 0.0, 22, 6064632283100282950},
        // Briarpaw the Gluttonous Bear at the Verdant Span Bear Dens, with its level 19-20 bears (Little Brother's settings, the Steelbloom bear rare); its paw is the reagent of
        // Leatherworking_T3_Armor_Briarpaw_Hands
        {"AR_HandPlaced_ANV_VerdantSpan_BriarpawGluttonousBear", 0, 6064631883861393433, 6064631754211655686, -1063317.0, -248011.0, 17451.0, 0.0, 20, 6064632283100020804},
    };
    return points;
}

int add_hand_placed(content::PopulationData& data) {
    int added = 0;
    int64_t base = kHandPlacedGuid;
    for (const HandPlaced& h : hand_placed()) {
        const int64_t g = base;
        base += 8;
        const auto like = data.instances.find(h.like_instance);
        if (like == data.instances.end()) continue;
        int64_t asset_set = h.asset_set;
        if (asset_set == 0) {
            if (data.assets.count(h.asset) == 0) continue;
            asset_set = g + 1;
            data.asset_sets[asset_set] = {{h.asset, 1.0}};
        } else if (data.asset_sets.count(asset_set) == 0) {
            continue;
        }

        content::NpcInstanceRow inst = like->second;
        inst.guid = g + 2;
        inst.name = h.name;
        inst.asset_sets = {asset_set};
        inst.patrol_route = 0;   // it stands at its own place: the sibling's walk is the sibling's
        if (h.level > 0) inst.level_min = inst.level_max = h.level;
        if (h.loot != 0) inst.loot_tables = {h.loot};
        data.instances[inst.guid] = std::move(inst);

        content::NpcSetRow set;
        set.guid = g + 3;
        set.name = h.name;
        content::NpcSetMember member;
        member.key = 1;
        member.instance = g + 2;
        set.members.push_back(member);
        data.sets[set.guid] = std::move(set);

        content::NpcWorldSpawn ws;
        ws.guid = g + 4;
        ws.name = h.name;
        ws.culture = "cCount";   // as npc_world_spawn holds it (the siblings' WorldSpawns name no culture either)
        ws.map_name = "Verra_World_Master";
        data.world_spawns[ws.guid] = std::move(ws);

        content::NpcPlacement p;
        p.spawn = g + 4;
        p.key = 1;
        p.set = g + 3;
        p.x = h.x;
        p.y = h.y;
        p.z = h.z;
        p.yaw = h.yaw;
        p.min_level = p.max_level = h.level;
        data.placements.push_back(p);
        ++added;
    }
    return added;
}

}  // namespace aoc::worldd::spawns
