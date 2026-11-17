// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tailoring_fight_tests.inc: a crafted Tailoring piece worn and its mitigation in a fight (the Tailoring milestone)
//
//  A level-25 Bard on the real content.db crafts the AcademicAegis Chest at a Tailoring - Novice station, wears it, and
//  the worn stats follow (Constitution +13 and MagicalMitigationRating +69, the engine's value for the chest's 59 rating;
//  the chest's own durability is the product's, 13).  The damage multiplier the profile reads (Magical.Mitigation) falls
//  and the mitigation factor stats::DamageProfile::calculate_damage_mitigation returns rises, so the piece's mitigation is
//  the one the fight's damage is worked out with.  Then the Bard swings at a Wolf and the Wolf comes for him and bites:
//  the piece is worn through a real fight, the Wolf loses Health and the Bard's Health falls with the chest still on.
//  Included by test_worldd_ability_session.cpp after weaponsmithing_fight_tests.inc; main() calls it.
// ============================================================================
#include "spawns/captured_spawns.h"
#include "stats/damage.h"
#include "world/actor_registry.h"
#include "world/city_nodes.h"
#include "world/node_wire.h"
#include "protocol/repl/craft_rpc.h"

namespace tlf {
constexpr int64_t kT1 = 6064630554067599401ll;
constexpr int64_t kHalcyon = 108646610851452ll;   // a Village node (level 3, Riverlands)
constexpr int64_t kChestRecipe = 6064629387023482920ll, kChestItem = 109229507958396ll;
constexpr int64_t kThreadDaffodil = 6064631879482736645ll, kSkinGrem = 6064630417661821030ll, kLightChest = 6064634087927906604ll;
constexpr const char* kTax = "NodeTax.ArtisanTax.TextileMillBaseTax.ServiceTax.TailorStorefrontServiceTax";
constexpr int64_t kHealth = 108986352686712ll;
}  // namespace tlf

/// The item record of a name in the content's stat bundle (0 when the bundle has no such stat).
static int64_t tlf_stat_named(const Run& r, const char* name) {
    const stats::StatBundle* bundle = world_stats::of(*r.h.fx.content).bundle;
    if (!bundle) return 0;
    for (size_t i = 0; i < bundle->stats.size(); ++i) {
        if (bundle->stat_name(i) == name) return bundle->stats[i].guid;
    }
    return 0;
}

/// The stats a hit reads, over the character's current stats (stats::StatView).
struct TlfStats final : stats::StatView {
    const stats::CharacterStats* cs = nullptr;
    double stat_value(int64_t guid) const override {
        const stats::StatValue* v = cs ? cs->find(guid) : nullptr;
        return v ? v->value : 0.0;
    }
};

/// One stack of a bag for the craft.
static svc::InventoryItem tlf_item(int slot, int64_t record, int32_t quantity = 1) {
    static uint32_t n = 0;
    return stack_of(record, slot, quantity, ++n + 0x9700);
}

/// ServerInteractWithInteractableComponent on the station's component, the way the client sends it.
static BunchWriter tlf_interact(uint64_t component) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest req;
    req.interactable.ObjectId = component;
    req.interactable.ServerId = 60;
    req.interactable.Randomizer = 0x6ee87ee4u;
    req.counter = 1;
    const auto params = rpc ? repl::interact_params(*rpc, req) : std::nullopt;
    return params ? wsf_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

/// ServerStartCraft of the chest with the stacks the client picked (the Light reagent is not named).
static BunchWriter tlf_start_craft(const emit::FIntrepidNetworkGUID& station, std::vector<repl::CraftResource> general) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerStartCraft");
    repl::StartCraft s;
    s.station = station;
    s.transaction = {21, 22, 23, 24};
    s.recipe = tlf::kChestRecipe;
    s.quantity = 1;
    s.general = std::move(general);
    const auto params = rpc ? repl::start_craft_params(*rpc, s) : std::nullopt;
    return params ? wsf_rpc(pc, "ServerStartCraft", *params) : BunchWriter();
}

/// ServerItemUsed on a bag slot: the chest is worn where its record lists it.
static BunchWriter tlf_item_used(int slot, const std::string& guid) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed");
    repl::RpcParams p;
    const auto g = items::guid_of(guid);
    const repl::GuidValue gv = g ? *g : repl::GuidValue{};
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(3));   // EItemLocation::Backpack
    ok = ok && repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
    ok = ok && repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    ok = ok && repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    ok = ok && repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    ok = ok && repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    ok = ok && repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    return ok ? wsf_rpc(pawn, "ServerItemUsed", p) : BunchWriter();
}

/// A world with ground, a Wolf and a Tailoring - Novice station behind the player.
struct TlfWorld {
    uint64_t wolf = 0;
    uint64_t station_id = 0;
    uint64_t station_component = 0;
    emit::FIntrepidNetworkGUID station_guid;
    uint64_t next_actor_id = 6600000;

    void prepare(PlayerWorld& w) {
        std::string err;
        if (!w.load_blueprints(NPC_MOTION_PATH, err)) {
            std::printf("  [FAIL] the Blueprint table: %s\n", err.c_str());
            return;
        }
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        if (!nav || !w.adopt_ground(std::move(nav), err)) {
            std::printf("  [FAIL] the ground: %s\n", err.c_str());
            return;
        }
        auto nodes = world::CityNodes::load(ability_rig::real_db_path(), err);
        std::unique_ptr<world::NodeWire> wire;
        if (nodes) {
            if (const world::CityNodeInfo* halcyon = nodes->by_name("Halcyon")) nodes->apply_snapshot({{halcyon->guid, 3, 0, 0}});
            wire = world::NodeWire::load(ability_rig::real_db_path(), *nodes, err);
        }
        if (!nodes || !wire) {
            std::printf("  [FAIL] the city nodes or the node wire: %s\n", err.c_str());
            return;
        }
        w.set_city_nodes(nodes.get());
        w.set_node_wire(wire.get());
        w.load_crafting_data();
        auto captured = spawns::CapturedData::load(err);
        if (!captured) {
            std::printf("  [FAIL] the captured data: %s\n", err.c_str());
            return;
        }
        // the node and wire are kept by the world: the test's copies live in the Run's fixture for the whole test
        kept_nodes = std::move(nodes);
        kept_wire = std::move(wire);
        std::string path;
        for (const auto& [p, row] : w.blueprints().rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) path = p;
        }
        wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + 120, kY, kZ, 8, 800);
        const spawns::OpenTemplate* t = nullptr;
        for (const spawns::OpenTemplate& o : captured->opens()) {
            if (o.category != "station") continue;
            if (!t) t = &o;
            if (o.archetype_path.find("Tailoring") != std::string::npos) {
                t = &o;
                break;
            }
        }
        if (!t) return;
        spawns::InstanceParams p;
        p.x = kX - 400;
        p.y = kY;
        p.z = kZ;
        p.server_id = 60;
        p.seed = 0x6263u;
        const spawns::Minter mint = [&]() { return w.mint_guid(); };
        auto actor = spawns::instantiate(*t, mint, p, err);
        if (!actor) return;
        actor->station.def = tlf::kT1;
        actor->station.name = "CraftingStation_Tailoring_T1";
        actor->station.tax_tag = tlf::kTax;
        actor->station.node = tlf::kHalcyon;
        actor->station.processing = false;
        station_guid = actor->actor.guid;
        station_component = actor->npc.interact_component;
        station_id = actor->id;
        if (!w.registry().add(std::move(*actor))) station_id = 0;
    }
    std::unique_ptr<world::CityNodes> kept_nodes;
    std::unique_ptr<world::NodeWire> kept_wire;
};

/// The fight: craft the chest, wear it, read the worn stats and the mitigation a hit reads, then swing at the Wolf and take its bite.
static void test_tlf_fight() {
    std::printf("a crafted Tailoring chest, worn and its mitigation in a fight\n");
    svc::Character rec = bard25();
    rec.items.push_back(tlf_item(0, tlf::kThreadDaffodil, 4));
    rec.items.push_back(tlf_item(1, tlf::kSkinGrem, 1));
    rec.items.push_back(tlf_item(2, tlf::kLightChest, 1));
    rec.wallet.push_back({items::kCopperCurrency, 100000});
    TlfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    CHECK(r.h.s != nullptr && ww.wolf != 0 && ww.station_id != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf and a Tailoring - Novice station");
    if (!r.h.s || ww.wolf == 0 || ww.station_id == 0 || !r.h.fx.world->combat()) return;
    r.h.client_sends({r.h.client.actor(r.h.pawn_ch, wsf_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", repl::RpcParams{}), true)});
    r.h.settle();
    r.play(0.5, false);
    r.h.client_sends({r.h.client.actor(r.h.pawn_ch, tlf_interact(ww.station_component), true)});
    r.h.settle();
    CHECK(r.h.s->station_opens() == 1, "the station's window opens");
    r.h.client_sends({r.h.client.actor(r.h.pc_ch, tlf_start_craft(ww.station_guid, {wsf_resource(tlf::kThreadDaffodil, 4), wsf_resource(tlf::kSkinGrem, 1)}), true)});
    r.h.settle();
    CHECK(r.h.s->quick_active(), "the craft begins");
    r.play(2.4, false);
    r.h.settle();
    CHECK(r.h.s->quick_made() == 1, "the chest is made");
    std::optional<svc::InventoryItem> chest;
    for (const svc::InventoryItem& it : r.h.s->inventory().items()) {
        if (it.item_record_id == tlf::kChestItem) chest = it;
    }
    CHECK(chest.has_value() && chest->container == svc::ItemContainer::Backpack && chest->quality == 1 && std::fabs(chest->durability - 13.0) < 0.01,
          "Gear_Armor_Light_AcademicAegis_Chest is in the bag at quality 1 and durability 13");
    if (!chest) return;

    // the item's own stat block, then the character's stats before the chest is worn
    const stats::CharacterStats* cs = r.h.s->character_stats();
    const int64_t constitution = tlf_stat_named(r, "Constitution");
    const int64_t rating = tlf_stat_named(r, "MagicalMitigationRating");
    CHECK(cs && constitution != 0 && rating != 0, "the worn piece's stats are in the stat bundle");
    {
        std::string gear_error;
        const auto gear = r.h.fx.content->item_gear(tlf::kChestItem, gear_error);
        bool raw = false;
        if (gear) {
            for (const stats::BlockStat& s : gear->stats) {
                if (s.stat == rating) raw = std::fabs(s.min[0] - 59.0) < 0.01;
            }
        }
        CHECK(gear && raw, "the chest's stat block gives 59 MagicalMitigationRating at Common (its worn rating is the engine's, +69 through the rating curve)");
    }
    const auto value = [&](int64_t guid) { return cs && cs->find(guid) ? cs->find(guid)->value : -1.0f; };
    const float constitution_before = value(constitution), rating_before = value(rating);
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    const int64_t mitigation_stat = cw.data().profile().magical.mitigation;
    const int64_t physical_stat = cw.data().profile().physical.mitigation;
    CHECK(mitigation_stat != 0 && physical_stat != 0, "the damage profile names the mitigation stat a hit reads");
    TlfStats view;
    view.cs = cs;
    stats::DamageHit magical;
    magical.tag = "Element.Magical.Slash";
    magical.physical = false;
    const double factor_before = cw.data().profile().calculate_damage_mitigation(view, magical);

    // wear it: the chest's Common values are the worn ones
    r.h.client_sends({r.h.client.actor(r.h.pawn_ch, tlf_item_used(chest->slot, chest->instance_guid), true)});
    r.h.settle();
    r.play(0.3, false);
    const svc::InventoryItem* worn = r.h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Chest);
    CHECK(worn != nullptr && worn->item_record_id == tlf::kChestItem && worn->quality == 1, "the crafted chest is worn in the Chest slot");
    CHECK(std::fabs(value(constitution) - constitution_before - 13.0f) < 0.01f, "Constitution +13 (the chest's Common value)");
    CHECK(std::fabs(value(rating) - rating_before - 69.0f) < 0.01f, "MagicalMitigationRating +69, the engine's value for the chest's 59 rating");
    const double mitigation = cw.data().profile().calculate_damage_mitigation(view, magical);
    const double read = view.stat_value(mitigation_stat);
    CHECK(read > 0.0 && std::fabs(mitigation - (1.0 - read)) < 1e-6, "a magical hit's mitigation factor is 1 less the worn Magical.Mitigation the damage formula reads");
    CHECK(mitigation > factor_before, "the chest's MagicalMitigationRating leaves less of a magical hit to take (the damage multiplier the profile reads fell, the mitigation rose)");

    // the fight: teach the 1H axe combo (the character's kit has no axe) and swing at the Wolf, as a player does
    const abilities::AbilityDef* axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    int64_t targeting = 0;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    emit::FIntrepidNetworkGUID cylinder;
    cylinder.ObjectId = ww.wolf + 4;
    cylinder.ServerId = kServerId;
    cylinder.Randomizer = kRandomizer;
    r.h.fx.world->tick_world(r.now());
    const double max_hp = cw.max_health_of(ww.wolf);
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_object(setup, axe->row.guid, id, cylinder);
    r.send_trigger(axe->row.guid, id, &trig);
    r.play(0.6, false);
    r.send_reliable(hit_request_at_object(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, cylinder));
    for (int i = 0; i < 5; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    const repl::HitInstance* conf = nullptr;
    for (const repl::HitInstance& h : r.m.hits) {
        if (!h.targets.empty()) conf = &h;
    }
    CHECK(conf != nullptr, "the swing at the Wolf is confirmed with a target");
    const bool missed = conf && (conf->targets[0].flags & stats::kHitMiss);
    CHECK(missed || cw.health_of(ww.wolf) < max_hp, "the Wolf lost Health to the swing (or the hit missed: it is a fight)");
    // it comes for the player and bites: the chest is still worn while its Health falls
    const float full = r.m.stats[tlf::kHealth];
    double bitten_at = -1;
    for (int i = 0; i < 80 && bitten_at < 0; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
        if (r.m.stats[tlf::kHealth] < full) bitten_at = r.now();
    }
    CHECK(bitten_at >= 0, "the Wolf goes for the Bard and its bite takes his Health, with the crafted chest on");
    const svc::InventoryItem* still = r.h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Chest);
    CHECK(still != nullptr && still->item_record_id == tlf::kChestItem, "the chest stays in the Chest slot through the fight");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
