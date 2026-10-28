// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  respawn/make_shrines.h  (Embersprings)
//
//  Makes the Emberspring actors of an area from the data's placements: each respawn point the population places with an attuned respawn definition (the instance's RespawnDataId
//  -> a respawn_point of kind 2 -> its attune_point) becomes an actor of the captured Emberspring open, standing where the data puts it, and a Shrine of the book the session
//  asks (discovery, the home, the death window).  Header-only (see respawn/embersprings.h).
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "respawn/embersprings.h"
#include "spawns/captured_spawns.h"
#include "spawns/population.h"
#include "world/actor_registry.h"
#include "world/interaction_state.h"

#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::respawn {

/// The archetype of an Emberspring (the Blueprint the captured template is keyed by).
inline constexpr const char* kShrineArchetype = "/Game/GameBlueprints/AttuneableCharacterRespawnBP.Default__AttuneableCharacterRespawnBP_C";

/// What the making did.
struct MadeShrines {
    ShrineBook book;
    size_t     made = 0;
    size_t     no_record = 0;      // a point whose instance names no attuned respawn definition or no place
    size_t     no_number = 0;      // a made shrine whose place has no number in the master list (it can be seen and used, not discovered)
    size_t     duplicates = 0;     // a respawn definition placed twice (the first stands)
    size_t     refused = 0;        // a shrine the open refused, or the registry
};

template <class Note>
MadeShrines make_shrines(world::ActorRegistry& registry, const spawns::CapturedData& captured, const content::RespawnData& data, const std::vector<spawns::RespawnPoint>& points,
                         const spawns::Minter& mint, uint32_t server_id, double world_time, Note&& note) {
    MadeShrines out;
    const spawns::OpenTemplate* tpl = captured.template_for(kShrineArchetype);
    if (!tpl) {
        note("the Emberspring has no captured open: no Emberspring is made");
        return out;
    }
    std::set<int64_t> seen;
    for (const spawns::RespawnPoint& p : points) {
        if (!p.attuneable) continue;
        const auto link = data.instance_respawn.find(p.instance);
        if (link == data.instance_respawn.end()) {
            ++out.no_record;
            continue;
        }
        const auto def = data.respawns.find(link->second);
        if (def == data.respawns.end() || def->second.kind != kAttunedRespawnKind) {
            ++out.no_record;
            continue;
        }
        const auto place = data.places.find(def->second.attune);
        if (place == data.places.end()) {
            ++out.no_record;
            continue;
        }
        if (!seen.insert(def->first).second) {
            ++out.duplicates;
            continue;
        }
        spawns::InstanceParams ip;
        ip.x = p.x;
        ip.y = p.y;
        ip.z = p.z;
        ip.yaw_degrees = p.yaw;
        ip.server_id = server_id;
        ip.world_time = world_time;
        ip.seed = mix64(static_cast<uint64_t>(def->first) ^ 0xE3B3E5ull);
        ip.attune_record = place->first;
        std::string why;
        auto w = spawns::instantiate(*tpl, mint, ip, why);
        if (!w) {
            ++out.refused;
            note("the Emberspring " + place->second.descriptor + " was refused: " + why);
            continue;
        }
        // the client offers the interaction only for an input the server allows (CanInputActivate), as for a vendor's
        if (!world::allow_inputs(w->actor)) {
            ++out.refused;
            note("the Emberspring " + place->second.descriptor + " has no interactable component to allow its input on");
            continue;
        }
        w->shrine.name = place->second.descriptor;
        w->shrine.respawn = def->first;
        w->shrine.number = place->second.number;
        w->npc.name = "Emberspring " + place->second.descriptor;
        w->npc.home_x = p.x;
        w->npc.home_y = p.y;
        w->npc.home_z = p.z;
        Shrine s;
        s.actor = w->id;
        s.place = place->first;
        s.respawn = def->first;
        s.number = place->second.number;
        s.x = p.x;
        s.y = p.y;
        s.z = p.z;
        s.yaw = p.yaw;
        s.radius = place->second.radius;
        s.box_x = def->second.box_x;
        s.box_y = def->second.box_y;
        s.box_z = def->second.box_z;
        s.exclusion = def->second.exclusion;
        s.name = place->second.descriptor;
        if (!registry.add(std::move(*w))) {
            ++out.refused;
            note("the Emberspring " + place->second.descriptor + " could not be added to the registry");
            continue;
        }
        if (s.number <= 0) ++out.no_number;
        out.book.add(std::move(s));
        ++out.made;
    }
    return out;
}

}  // namespace aoc::worldd::respawn
