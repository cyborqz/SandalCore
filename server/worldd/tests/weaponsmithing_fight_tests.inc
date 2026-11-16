// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  weaponsmithing_fight_tests.inc: a crafted weapon wielded and used in a fight (the Weaponsmithing milestone)
//
//  A session on the real content.db with ground and a Wolf: the player crafts the Brooksteel Axe at a Weaponsmithing
//  station, equips it, and the axe's 1H combo is in KnownAbilities and the weapon line in hand.  The combo is triggered at
//  the Wolf and the client's hit request is confirmed with the Wolf as its target, whose Health falls: the crafted item is
//  wielded and used in a fight.  Included by test_worldd_ability_session.cpp before its registered list; main() calls it.
// ============================================================================
#include "spawns/captured_spawns.h"
#include "world/actor_registry.h"
#include "world/city_nodes.h"
#include "world/node_wire.h"
#include "protocol/repl/craft_rpc.h"
#include "protocol/repl/vendor_rpc.h"

namespace wsf {
constexpr int64_t kSmith = 108752746862200ll;
constexpr int64_t kSmithT1 = 6064630515162480654ll;
constexpr int64_t kHalcyon = 108646610851452ll;
constexpr int64_t kAxeRecipe = 6064634092895538328ll, kAxeItem = 6064633866903617615ll;
constexpr int64_t kCopper = 6064630417661755489ll, kTimber = 6064629252204724244ll, kReagent = 6064634088230420900ll;
constexpr const char* kTax = "NodeTax.ArtisanTax.SmithyBaseTax.ServiceTax.WeaponsmithServiceTax";
}  // namespace wsf

/// A bag stack with a fresh instance guid, for the record the fight's session loads.
static svc::InventoryItem wsf_item(int slot, int64_t record, int32_t quantity = 1) {
    static uint32_t n = 0;
    svc::InventoryItem it;
    it.container = svc::ItemContainer::Backpack;
    it.slot = slot;
    it.item_record_id = record;
    char buf[33];
    std::snprintf(buf, sizeof buf, "%032x", ++n + 0x7000);
    it.instance_guid = buf;
    it.quantity = quantity;
    it.quality = 1;
    it.durability = 11;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

/// A client RPC of `cls` (the PlayerController or the pawn) in the client's own form.
static BunchWriter wsf_rpc(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) {
    const schema::RpcSchema* rpc = cls.rpc(name);
    BunchWriter field;
    bool failed = false;
    if (!rpc || repl::write_rpc_field(cls, *rpc, params, field, client_options(), &failed) == 0 || failed) return BunchWriter();
    BunchWriter out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// One named stack of a start: the record, how many and the rarity the client names (Common).
static repl::CraftResource wsf_resource(int64_t record, int count, int rarity = 0) {
    repl::CraftResource r;
    r.record = record;
    r.rarities.push_back({static_cast<uint8_t>(rarity), count, false});
    return r;
}

/// ServerInteractWithInteractableComponent on a station's component.
static BunchWriter wsf_interact(uint64_t component) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest r;
    r.interactable.ObjectId = component;
    r.interactable.ServerId = 60;
    r.interactable.Randomizer = 0x6ee87ee4u;
    r.counter = 1;
    const auto params = rpc ? repl::interact_params(*rpc, r) : std::nullopt;
    return params ? wsf_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

/// ServerStartCraft of the axe recipe with the stacks the client picked (the reagent is not named).
static BunchWriter wsf_start_craft(const emit::FIntrepidNetworkGUID& station, std::vector<repl::CraftResource> general) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerStartCraft");
    repl::StartCraft s;
    s.station = station;
    s.transaction = {11, 12, 13, 14};
    s.recipe = wsf::kAxeRecipe;
    s.quantity = 1;
    s.general = std::move(general);
    const auto params = rpc ? repl::start_craft_params(*rpc, s) : std::nullopt;
    return params ? wsf_rpc(pc, "ServerStartCraft", *params) : BunchWriter();
}

/// ServerItemUsed on a bag slot: the axe is worn where its record lists it.
static BunchWriter wsf_item_used(int slot, const std::string& guid) {
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

/// The fight's world: the captured area's ground and a Wolf, and a Weaponsmithing station 4 m behind the player.
struct WsFightWorld {
    uint64_t wolf = 0;
    uint64_t next_actor_id = 6000000;
    uint64_t station_id = 0;
    uint64_t station_component = 0;
    emit::FIntrepidNetworkGUID station_guid;
    std::optional<spawns::CapturedData> captured;
    std::unique_ptr<world::CityNodes> nodes;
    std::unique_ptr<world::NodeWire> wire;

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
        // the station's node: Halcyon at level 3, in the Riverlands (the recipes' biome and level predicates read it)
        nodes = world::CityNodes::load(ability_rig::real_db_path(), err);
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
        captured = spawns::CapturedData::load(err);
        if (!captured) {
            std::printf("  [FAIL] the captured data: %s\n", err.c_str());
            return;
        }
        std::string path;
        for (const auto& [p, row] : w.blueprints().rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) path = p;
        }
        wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + 120, kY, kZ, 8, 100);
        // the station: the captured station's open, the Novice Weaponsmithing definition and a Village's node
        const spawns::OpenTemplate* t = nullptr;
        for (const spawns::OpenTemplate& o : captured->opens()) {
            if (o.category != "station") continue;
            if (!t) t = &o;
            if (o.archetype_path.find("Weaponsmithing_T2") != std::string::npos) {
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
        p.seed = 0x6162u;
        const spawns::Minter mint = [&]() { return w.mint_guid(); };
        auto actor = spawns::instantiate(*t, mint, p, err);
        if (!actor) return;
        actor->station.def = wsf::kSmithT1;
        actor->station.name = "CraftingStation_Weaponsmithing_T1";
        actor->station.tax_tag = wsf::kTax;
        actor->station.node = wsf::kHalcyon;
        actor->station.processing = false;
        station_guid = actor->actor.guid;
        station_component = actor->npc.interact_component;
        station_id = actor->id;
        if (!w.registry().add(std::move(*actor))) station_id = 0;
    }
};

/// The fight: craft the axe at the station, wear it, and swing it at the Wolf.
static void test_ws_fight() {
    std::printf("a crafted axe is wielded and used in a fight\n");
    svc::Character rec = bard25();
    rec.items = {wsf_item(0, wsf::kCopper, 5), wsf_item(1, wsf::kTimber), wsf_item(2, wsf::kReagent)};
    rec.wallet.push_back({items::kCopperCurrency, 100000});
    WsFightWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    CHECK(r.h.s != nullptr && ww.wolf != 0 && ww.station_id != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf and a Weaponsmithing station");
    if (ww.wolf == 0 || ww.station_id == 0 || !r.h.fx.world->combat()) return;
    // the client phases in (the station and the Wolf open), then opens the station's window
    r.h.client_sends({r.h.client.actor(r.h.pawn_ch, wsf_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    r.h.settle();
    r.play(0.5, false);
    r.h.client_sends({r.h.client.actor(r.h.pawn_ch, wsf_interact(ww.station_component), true)});
    r.h.settle();
    CHECK(r.h.s->station_opens() == 1, "the station's window opens");
    r.h.client_sends({r.h.client.actor(r.h.pc_ch, wsf_start_craft(ww.station_guid, {wsf_resource(wsf::kCopper, 5), wsf_resource(wsf::kTimber, 1)}), true)});
    r.h.settle();
    CHECK(r.h.s->quick_active(), "the craft begins");
    r.play(2.4, false);
    r.h.settle();
    CHECK(r.h.s->quick_made() == 1, "the axe is made");
    std::optional<svc::InventoryItem> axe;
    for (const svc::InventoryItem& it : r.h.s->inventory().items()) {
        if (it.item_record_id == wsf::kAxeItem) axe = it;
    }
    CHECK(axe.has_value() && axe->container == svc::ItemContainer::Backpack, "the axe is in the bag");
    if (!axe) return;
    const int bag_slot = axe->slot;
    const std::string axe_guid = axe->instance_guid;
    r.h.client_sends({r.h.client.actor(r.h.pawn_ch, wsf_item_used(bag_slot, axe_guid), true)});
    r.h.settle();
    r.play(0.3, false);
    const svc::InventoryItem* worn = r.h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Primary);
    CHECK(worn != nullptr && worn->item_record_id == wsf::kAxeItem && worn->quality == 1, "the axe is worn in the primary hand");
    bool combo = false;
    for (const int64_t a : r.h.s->known_abilities()) combo = combo || a == 6064631012558503944ll;   // WeaponCombo_Melee_1H_Axe
    CHECK(combo, "the axe's WeaponCombo_Melee_1H_Axe is in the player's KnownAbilities (the weapon line in hand)");

    // the swing: the client names the Wolf's collision cylinder in place, as the client does in the captures
    const abilities::AbilityDef* axe_ability = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe_ability != nullptr && !axe_ability->hits.empty(), "the axe's combo is in the ability tables");
    if (!axe_ability || axe_ability->hits.empty()) return;
    int64_t targeting = 0;
    for (const content::TargetingRow& t : axe_ability->targetings) {
        if (t.key == axe_ability->hits[0].key) targeting = t.guid;
    }
    const world::WorldActor* wa = r.h.fx.world->registry().find(ww.wolf);
    CHECK(wa != nullptr, "(the Wolf is in the registry)");
    if (!wa) return;
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    r.h.fx.world->tick_world(r.now());   // the combat world takes the Wolf (its stats) before its maximum is read
    const double max_hp = cw.max_health_of(ww.wolf);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    repl::InlineObject cyl;
    cyl.flags = 5;
    cyl.outer = wa->actor.guid;
    cyl.path = "CollisionCylinder";
    cyl.checksum = 0xb876932f;
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, axe_ability->row.guid, id, repl::Value(cyl));
    r.send_trigger(axe_ability->row.guid, id, &trig);
    r.play(0.6, false);
    repl::HitComponentRef comp;
    comp.inline_object = cyl;
    r.send_reliable(hit_request_with(setup, axe_ability->row.guid, targeting, axe_ability->hits[0].hit_guid, comp));
    for (int i = 0; i < 5; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    const repl::HitInstance* conf = nullptr;
    for (const repl::HitInstance& h : r.m.hits) {
        if (!h.targets.empty()) conf = &h;
    }
    CHECK(conf != nullptr, "the swing at the Wolf is confirmed with a target");
    const double hp = cw.health_of(ww.wolf);
    const bool missed = conf && (conf->targets[0].flags & stats::kHitMiss);
    std::printf("  (the Wolf: %.1f of %.1f Health after the crafted axe's swing)\n", hp, max_hp);
    CHECK(conf && (missed || hp < max_hp), "the Wolf lost Health to the crafted axe (or the hit missed: the swing is a fight)");
    CHECK(r.h.s->known_abilities().size() > 0 && r.h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Primary) != nullptr,
          "the axe stays in hand after the fight");
    CHECK(r.h.s->quick_made() == 1, "the crafted axe in the fight");
}
