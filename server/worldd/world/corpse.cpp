// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/corpse.cpp (see corpse.h)
// ============================================================================
#include "world/corpse.h"

#include "bootstrap/constants.h"
#include "protocol/repl/block_reader.h"
#include "protocol/schema/generated/index.h"
#include "spawns/corpse_exports.h"

#include <array>
#include <iterator>

namespace aoc::worldd::world {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace gen = ::aoc::protocol::schema::gen;
namespace cx = ::aoc::worldd::spawns::corpse_exports;
using protocol::emit::FIntrepidNetworkGUID;

namespace {

template <size_t N>
repl::ExportChain chain_of(const cx::Link (&links)[N]) {
    repl::ExportChain out;
    for (const cx::Link& l : links) {
        repl::ExportLink link;
        link.guid.ObjectId = l.id;
        link.guid.ServerId = l.server_id;
        link.guid.Randomizer = l.randomizer;
        link.name = l.name ? l.name : "";
        link.no_load = (l.flags & 2) != 0;
        link.has_checksum = (l.flags & 4) != 0;
        link.checksum = l.checksum;
        out.push_back(std::move(link));
    }
    return out;
}

repl::Subobject* find_sub(repl::Actor& a, const char* cls) {
    for (repl::Subobject& s : a.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

/// A dynamic subobject made of one of the retail blocks.
bool make(const char* cls, const cx::Payload& payload, repl::ExportChain chain, const FIntrepidNetworkGUID& guid, repl::Subobject& out, std::string& error) {
    const schema::ClassSchema* schema = gen::find_class_schema(cls);
    if (!schema) {
        error = std::string("no schema ") + cls;
        return false;
    }
    std::string why;
    auto state = repl::load_object_state_hex(*schema, payload.hex, payload.bits, payload.rep, &why);
    if (!state) {
        error = std::string("the retail ") + cls + " block does not load: " + why;
        return false;
    }
    out = {};
    out.guid = guid;
    out.obj = std::move(*state);
    out.dynamic_class = std::move(chain);
    return true;
}

/// An export id worldd mints for a native class's default object: a hash of its path with the static bit set (the form spawns/generic_npc.h gives;
/// the client resolves the export by its path and accepts checksum 0).
uint64_t export_id_of(const char* path) {
    uint64_t h = 1469598103934665603ull;
    for (const char* p = path; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211ull;
    }
    return h | 1ull;
}

/// The export chain of the class default object of the base loot pile, ALootPile (world/corpse.h: an item drop's pile).  The retail full-bag capture
/// exports `Default__LootPile` from `/Script/GameSystemsPlugin`, checksum 0.
repl::ExportChain loot_pile_archetype() {
    repl::ExportChain chain;
    repl::ExportLink obj, pkg;
    obj.guid = FIntrepidNetworkGUID::from_u64(export_id_of("/Script/GameSystemsPlugin.Default__LootPile"));
    obj.name = "Default__LootPile";
    obj.has_checksum = true;
    obj.checksum = 0;
    pkg.guid = FIntrepidNetworkGUID::from_u64(158953490572197689ull);   // the package id the retail chains of its components name
    pkg.name = "/Script/GameSystemsPlugin";
    pkg.has_checksum = true;
    pkg.checksum = 0;
    chain.push_back(std::move(obj));
    chain.push_back(std::move(pkg));
    return chain;
}

repl::Value guid_word(uint32_t w) { return repl::Value::i64(static_cast<int32_t>(w)); }
// built by C | Y | B with C O A

repl::Value guid_list(const std::vector<repl::GuidValue>& guids) {
    repl::ArrayValue list;
    for (const repl::GuidValue& g : guids) {
        repl::ElementState e;
        e.values[1] = guid_word(g.a);
        e.values[2] = guid_word(g.b);
        e.values[3] = guid_word(g.c);
        e.values[4] = guid_word(g.d);
        list.elements.push_back(std::move(e));
    }
    return repl::Value(std::move(list));
}

/// The first input's state of the interactable: `state` (EInteractionPointPermissionStates: 2 available, 0 none) with the players in StateGuids.
bool set_first_input(repl::Subobject& inter, int state, const std::vector<repl::GuidValue>& players) {
    const repl::Value* v = inter.obj.state.get(*inter.obj.schema, "CurrentInputStates");
    const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!arr || arr->elements.empty()) return false;
    repl::ArrayValue copy = *arr;
    repl::ElementState& e = copy.elements[0];
    e.values[1] = repl::Value::u64(static_cast<uint64_t>(state));
    e.values[2] = guid_list(players);
    e.values[3] = repl::Value::u64(static_cast<uint64_t>(state));
    e.values[4] = guid_list(players);
    // the other inputs are not offered: current state 0 (retail: 1 before the loot is taken, then current 0 and cached 1)
    return inter.obj.state.set(*inter.obj.schema, "CurrentInputStates", repl::Value(std::move(copy)));
}

/// The three components of a loot (a corpse's or a pile's), made of the retail blocks: `pile` takes the pile's own.
struct Components {
    repl::Subobject loot, container, inter;
    FIntrepidNetworkGUID loot_guid, container_guid, inter_guid;
};

/// PlayerInteractionDef FoliageLootPileInteraction: the interaction of input 0 of the pile a full bag's overflow makes (the retail full-bag open names it; it has no
/// channel, where the dead player's pile's PlayerLootPileInteraction has a 3 s one).
constexpr int64_t kFoliageLootPileInteraction = 6064632235501551616ll;

/// Puts `def` as the interaction of the first input of the interactable's InteractionInputData.
bool set_first_input_definition(repl::Subobject& inter, int64_t def) {
    const repl::Value* v = inter.obj.state.get(*inter.obj.schema, "InteractionInputData");
    const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!arr || arr->elements.empty()) return false;
    repl::ArrayValue copy = *arr;
    copy.elements[0].values[6] = repl::Value::i64(def);   // InteractionInputData.PlayerInteractDefinition.Guid
    return inter.obj.state.set(*inter.obj.schema, "InteractionInputData", repl::Value(std::move(copy)));
}

/// `item_drop` (with `pile`): the pile of the items that fell out of a full bag.  Its loot is not public (the retail open sends no bPublicLoot) and its interaction is the foliage pile's.
bool build_components(bool pile, bool item_drop, const Minter& mint, const repl::GuidValue& owner, const std::vector<repl::GuidValue>& may_loot, int top_rarity, bool has_equipment,
                      Components& c, std::string& error) {
    c.loot_guid = mint();
    c.container_guid = mint();
    c.inter_guid = mint();
    if (!make("ULootComponent", pile ? cx::kPileLootComponentPayload : cx::kLootComponentPayload, chain_of(cx::kLootComponent), c.loot_guid, c.loot, error) ||
        !make("UItemStorageComponent", pile ? cx::kPileItemStorageComponentPayload : cx::kItemStorageComponentPayload, chain_of(cx::kItemStorageComponent), c.container_guid,
              c.container, error) ||
        !make("UInteractableComponent", pile ? cx::kPileInteractableComponentPayload : cx::kInteractableComponentPayload, chain_of(cx::kInteractableComponent), c.inter_guid,
              c.inter, error)) {
        return false;
    }
    repl::ObjectState& lo = c.loot.obj;
    bool ok = lo.state.set(*lo.schema, "OwnerGuid.A", guid_word(owner.a)) && lo.state.set(*lo.schema, "OwnerGuid.B", guid_word(owner.b)) &&
              lo.state.set(*lo.schema, "OwnerGuid.C", guid_word(owner.c)) && lo.state.set(*lo.schema, "OwnerGuid.D", guid_word(owner.d)) &&
              lo.state.set(*lo.schema, "CosmeticData.TopRarity", repl::Value::u64(static_cast<uint64_t>(top_rarity))) &&
              lo.state.set(*lo.schema, "ServerLootContainer", repl::Value(c.container_guid));
    // the equipment's light: sent only for loot that holds equipment, as the retail blocks leave the field out when it is clear (the client reads an absent bool as clear)
    if (has_equipment) ok = ok && lo.state.set(*lo.schema, "CosmeticData.bHasEquipment", repl::Value::boolean(true));
    if (pile && !item_drop) ok = ok && lo.state.set(*lo.schema, "bPublicLoot", repl::Value::boolean(true));
    if (item_drop) {   // the pile block's own LootComponent is a death's (public): the drop's is the owner's, and sends no bPublicLoot
        if (const schema::RepCommand* pub = lo.schema->command("bPublicLoot")) lo.state.erase(pub->handle);
        ok = ok && set_first_input_definition(c.inter, kFoliageLootPileInteraction);
    }
    // the client offers an interaction only for an input the server allows (world/interaction_state.h): the four inputs are
    repl::ArrayValue allowed;
    for (int k = 0; k < 4; ++k) {
        repl::ElementState e;
        e.values[1] = repl::Value::boolean(true);
        allowed.elements.push_back(std::move(e));
    }
    ok = ok && c.inter.obj.state.set(*c.inter.obj.schema, "CanInputActivate", repl::Value(std::move(allowed))) && set_first_input(c.inter, 2, may_loot);
    if (!ok) error = "a value of the loot components is refused";
    return ok;
}

}  // namespace

std::optional<CorpseLoot> add_loot(ActorRegistry& registry, uint64_t npc, const Minter& mint, const repl::GuidValue& owner,
                                   const std::vector<repl::GuidValue>& may_loot, int top_rarity, bool has_equipment, std::string& error) {
    WorldActor* w = registry.find(npc);
    if (!w || w->kind != ActorKind::Npc) {
        error = "the NPC is not held";
        return std::nullopt;
    }
    if (find_sub(w->actor, "ULootComponent")) {
        error = "the NPC has its loot components already";
        return std::nullopt;
    }
    repl::Subobject* secondary = find_sub(w->actor, "UNPCSecondaryInfo");
    if (!secondary) {
        error = "the NPC has no InteractInfo to point at its loot";
        return std::nullopt;
    }
    CorpseLoot out;
    Components c;
    if (!build_components(false, false, mint, owner, may_loot, top_rarity, has_equipment, c, error)) return std::nullopt;
    const FIntrepidNetworkGUID loot_guid = c.loot_guid, container_guid = c.container_guid, inter_guid = c.inter_guid;
    repl::Subobject loot = std::move(c.loot), container = std::move(c.container), inter = std::move(c.inter);
    bool ok = false;
    ok = registry.modify(npc, Change::State, [&](WorldActor& a) {
        repl::Subobject* sec = find_sub(a.actor, "UNPCSecondaryInfo");
        if (!sec) {
            ok = false;
            return;
        }
        ok = sec->obj.state.set(*sec->obj.schema, "LootComponent", repl::Value(loot_guid)) &&
             sec->obj.state.set(*sec->obj.schema, "LootContainerComponent", repl::Value(container_guid));
        if (!ok) return;
        a.actor.subobjects.push_back(std::move(loot));
        a.actor.subobjects.push_back(std::move(container));
        a.actor.subobjects.push_back(std::move(inter));
    }) && ok;
    if (!ok) {
        error = "the NPC's InteractInfo refuses the loot pointers";
        return std::nullopt;
    }
    out.loot_component = loot_guid.ObjectId;
    out.container = container_guid.ObjectId;
    out.interactable = inter_guid.ObjectId;
    out.container_guid = container_guid;
    (void)secondary;
    if (!registry.set_interact_component(npc, out.interactable)) {
        error = "the interactable's component id is taken";
        return std::nullopt;
    }
    return out;
}
// [C.O.A + C_Y_B]

std::optional<Pile> add_pile(ActorRegistry& registry, const Minter& mint, double x, double y, double z, const repl::GuidValue& player, const std::string& name,
                             const std::vector<repl::GuidValue>& may_loot, int top_rarity, bool has_equipment, std::string& error, bool item_drop) {
    const schema::ClassSchema* schema = gen::find_class_schema(item_drop ? "ALootPile" : "APlayerLootPile");
    if (!schema) {
        error = item_drop ? "no schema ALootPile" : "no schema APlayerLootPile";
        return std::nullopt;
    }
    repl::ObjectState loaded;
    if (item_drop) {
        loaded.schema = schema;   // (the class's own values; the fields the client needs are set below)
    } else {
        std::string why;
        auto state = repl::load_object_state_hex(*schema, cx::kPileActorPayload.hex, cx::kPileActorPayload.bits, cx::kPileActorPayload.rep, &why);
        if (!state) {
            error = "the retail pile block does not load: " + why;
            return std::nullopt;
        }
        loaded = std::move(*state);
    }
    Components c;
    if (!build_components(true, item_drop, mint, player, may_loot, top_rarity, has_equipment, c, error)) return std::nullopt;
    WorldActor w;
    w.kind = ActorKind::Pile;
    repl::Actor& a = w.actor;
    a.guid = mint();
    a.obj = std::move(loaded);
    repl::ExportChain archetype;
    if (item_drop) {
        archetype = loot_pile_archetype();
    } else {
        for (const cx::Link& l : cx::kPileArchetype) {
            repl::ExportLink link;
            link.guid.ObjectId = l.id;
            link.guid.ServerId = l.server_id;
            link.guid.Randomizer = l.randomizer;
            link.name = l.name ? l.name : "";
            link.no_load = (l.flags & 2) != 0;
            link.has_checksum = (l.flags & 4) != 0;
            link.checksum = l.checksum;
            archetype.push_back(std::move(link));
        }
    }
    a.open.archetype = std::move(archetype);
    a.open.level = bootstrap::persistent_level_export();
    a.open.spawn.location = repl::quantize_vector(10, x, y, z);
    w.id = a.guid.ObjectId;
    w.x = x;
    w.y = y;
    w.z = z;
    repl::ObjectState& o = a.obj;
    uint64_t seed = a.guid.ObjectId * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    const auto next = [&seed]() {
        seed += 0x9E3779B97F4A7C15ull;
        uint64_t zz = seed;
        zz = (zz ^ (zz >> 30)) * 0xBF58476D1CE4E5B9ull;
        zz = (zz ^ (zz >> 27)) * 0x94D049BB133111EBull;
        return zz ^ (zz >> 31);
    };
    bool ok = o.state.set(*o.schema, "LootContainer", repl::Value(c.container_guid)) && o.state.set(*o.schema, "LootComponent", repl::Value(c.loot_guid)) &&
              o.state.set(*o.schema, "LootPileGuid.A", guid_word(static_cast<uint32_t>(next()))) && o.state.set(*o.schema, "LootPileGuid.B", guid_word(static_cast<uint32_t>(next()))) &&
              o.state.set(*o.schema, "LootPileGuid.C", guid_word(static_cast<uint32_t>(next()))) && o.state.set(*o.schema, "LootPileGuid.D", guid_word(static_cast<uint32_t>(next())));
    if (item_drop) {
        // what the retail full-bag open sets besides these: the roles (SimulatedProxy for every client, and the Authority the client receives), and no bHasLoot
        ok = ok && o.state.set(*o.schema, "RemoteRole", repl::Value::u64(1)) && o.state.set(*o.schema, "Role", repl::Value::u64(4));
    } else {   // a dead player's pile names the player and the kind of pile (and sends bHasLoot); a drop's class has none of these
        ok = ok && o.state.set(*o.schema, "bHasLoot", repl::Value::boolean(true)) && o.state.set(*o.schema, "PlayerName", repl::Value::str(name)) &&
             o.state.set(*o.schema, "PlayerGuid.A", guid_word(player.a)) && o.state.set(*o.schema, "PlayerGuid.B", guid_word(player.b)) &&
             o.state.set(*o.schema, "PlayerGuid.C", guid_word(player.c)) && o.state.set(*o.schema, "PlayerGuid.D", guid_word(player.d)) &&
             o.state.set(*o.schema, "LootPileType", repl::Value::u64(2)) && o.state.set(*o.schema, "bIsOwnerCorrupted", repl::Value::boolean(false));
    }
    if (!ok) {
        error = "a value of the loot pile is refused";
        return std::nullopt;
    }
    a.subobjects.push_back(std::move(c.loot));
    a.subobjects.push_back(std::move(c.container));
    a.subobjects.push_back(std::move(c.inter));
    Pile out;
    out.actor = w.id;
    out.parts.loot_component = c.loot_guid.ObjectId;
    out.parts.container = c.container_guid.ObjectId;
    out.parts.interactable = c.inter_guid.ObjectId;
    out.parts.container_guid = c.container_guid;
    w.npc.interact_component = out.parts.interactable;
    if (!registry.add(std::move(w))) {
        error = "the registry refuses the pile";
        return std::nullopt;
    }
    return out;
}

bool empty_loot(ActorRegistry& registry, uint64_t npc) {
    bool done = false;
    registry.modify(npc, Change::State, [&](WorldActor& a) {
        repl::Subobject* loot = find_sub(a.actor, "ULootComponent");
        repl::Subobject* inter = nullptr;
        for (repl::Subobject& s : a.actor.subobjects) {
            if (s.obj.schema && std::string(s.obj.schema->name) == "UInteractableComponent" && s.dynamic_class) inter = &s;
        }
        if (!loot || !inter) return;
        const bool a1 = loot->obj.state.set(*loot->obj.schema, "CosmeticData.TopRarity", repl::Value::u64(static_cast<uint64_t>(kLootEmptied)));
        const bool a2 = set_first_input(*inter, 0, {});
        done = a1 && a2;
    });
    return done;
}

bool set_loot_light(ActorRegistry& registry, uint64_t npc, int top_rarity, bool has_equipment) {
    bool done = false;
    registry.modify(npc, Change::State, [&](WorldActor& a) {
        repl::Subobject* loot = find_sub(a.actor, "ULootComponent");
        if (!loot) return;
        bool ok = loot->obj.state.set(*loot->obj.schema, "CosmeticData.TopRarity", repl::Value::u64(static_cast<uint64_t>(top_rarity)));
        // the equipment's light: set while the loot holds equipment; when the last of it is taken the field the client holds must be told false (an absent field
        // would keep the light it already has), and loot that never had any keeps the retail block's absent field
        if (has_equipment) {
            ok = ok && loot->obj.state.set(*loot->obj.schema, "CosmeticData.bHasEquipment", repl::Value::boolean(true));
        } else if (loot->obj.state.get(*loot->obj.schema, "CosmeticData.bHasEquipment")) {
            ok = ok && loot->obj.state.set(*loot->obj.schema, "CosmeticData.bHasEquipment", repl::Value::boolean(false));
        }
        done = ok;
    });
    return done;
}

}  // namespace aoc::worldd::world
