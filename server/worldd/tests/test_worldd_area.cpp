// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_area.cpp  (plan E1, stage 1)
//
//  The living area on the real content.db (the working tree's dist/content/content.db, or AOC_CONTENT_DB; contentc version 7):
//
//    captured data   the generated header parses: every open's blocks read into values with the generated schemas, the templates of the
//                    archetypes, the spawns of the captures
//    instances       an instance of a template differs from the captured open in nothing but the values that are an instance's own (the
//                    position, the owner and controller, the timestamp, the guids and the level...); every template's instance writes an open
//                    that reads back through the field loop (a RepLayout block per object, no leftovers)
//    population      the data's NPCs: the same inputs give the same list; every NPC is inside the area; the placements' points of the research
//                    (3,929 in a kilometre) and what became of them (counted by reason)
//    the area        load_area fills a registry: the data's NPCs, the captured ones without a twin in the data, the nodes and the GM vendors; ids
//                    are even and unique; every actor is inside the area but the GM vendors, which stand in front of the login spot; the radius
//                    is the one place the size is set (a smaller radius has fewer actors)
//    GM vendors      the PTR_ONLY vendor defs and their stock; the layout; the purchase plan on the real item records: stacks of MaxStackSize,
//                    an item no listing has, a wrong vendor, quantities, a cart of too many items, a bag too full
// ============================================================================
#include "combat/aftermath.h"
#include "combat/combat_world.h"
#include "combat/hit_resolver.h"
#include "content/content_db.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"
#include "npc/area_npcs.h"
#include "npc/motion_table.h"
#include "npc/retail_motion.h"
#include "session/player_world.h"
#include "spawns/area_loader.h"
#include "spawns/generic_npc.h"
#include "spawns/mount_donors.h"
#include "vendor/gm_vendor.h"
#include "world/interaction_state.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using aoc::protocol::emit::FIntrepidNetworkGUID;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
// (C Y B | C-O-A)

static uint64_t g_next = 4608;   // the next ObjectId a test mints (even)
static FIntrepidNetworkGUID mint() {
    FIntrepidNetworkGUID g;
    g.ObjectId = g_next;
    g_next += 2;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}

static const char* chr_n() { return "\n"; }

static std::string ms_since(std::chrono::steady_clock::time_point t) {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count()) + " ms";
}

/// Every handle whose value differs between two states, by name.
static std::set<std::string> differing(const repl::ObjectState& a, const repl::ObjectState& b) {
    std::set<std::string> out;
    std::set<uint16_t> handles;
    for (const auto& [h, v] : a.state.values()) handles.insert(h);
    for (const auto& [h, v] : b.state.values()) handles.insert(h);
    for (const uint16_t h : handles) {
        const repl::Value* x = a.state.get(h);
        const repl::Value* y = b.state.get(h);
        if (x && y && *x == *y) continue;
        const schema::RepCommand* cmd = nullptr;
        for (const schema::RepCommand& c : a.schema->commands) {
            if (c.handle == h) cmd = &c;
        }
        out.insert(cmd ? cmd->path : "#" + std::to_string(h));
    }
    return out;
}

static void test_captured(const spawns::CapturedData& captured) {
    std::printf("the captured spawns\n");
    std::printf("    %zu opens, %zu templates, %zu spawns\n", captured.opens().size(), captured.template_count(), captured.spawns().size());
    CHECK(captured.opens().size() >= captured.template_count() && captured.opens().size() > 50, "the header holds the opens of the templates and the spawns");
    CHECK(captured.template_count() >= 30, "templates for at least 30 archetypes");
    CHECK(!captured.spawns().empty(), "spawns the captures saw in the area");
    size_t blocks = 0;
    bool all_schema = true;
    for (const spawns::OpenTemplate& t : captured.opens()) {
        blocks += t.blocks.size();
        for (const spawns::ParsedBlock& b : t.blocks) all_schema = all_schema && b.schema != nullptr;
    }
    CHECK(all_schema && blocks > captured.opens().size(), "every block has a schema and was read into values");
    CHECK(captured.area_radius_cm() == 100000.0 && captured.area_center_x() == spawns::AreaConfig{}.center_x && captured.area_center_y() == spawns::AreaConfig{}.center_y,
          "the header was cut for the area's centre and 1 km radius (spawns/area.h)");
    const spawns::OpenTemplate* vendor = captured.template_for(spawns::kGmVendorArchetype);
    CHECK(vendor != nullptr && vendor->find("UInteractableComponent") > 0, "the vendor archetype has a template with an InteractableComponent");
}

static void test_instances(const spawns::CapturedData& captured) {
    std::printf("an instance is the captured open but for what is an instance's own\n");
    const std::set<std::string> allowed_actor = {"AuthServerIDReplicated", "ReplicatedMovement", "Owner", "Controller", "ReplicatedServerLastTransformUpdateTimeStamp",
                                                  "Instigator", "CurrentMovementInput", "GatherableGuid.A", "GatherableGuid.B", "GatherableGuid.C", "GatherableGuid.D"};
    size_t checked = 0, bad = 0, unreadable = 0, writes = 0;
    std::string first_bad;
    for (const spawns::OpenTemplate& t : captured.opens()) {
        if (t.category != "npc" && t.category != "gatherable") continue;
        spawns::InstanceParams p;
        p.x = -777000;
        p.y = 616000;
        p.z = 5000;
        p.yaw_degrees = 90;
        p.server_id = 60;
        p.level = 7;
        p.pop_instance = 111;
        p.pop_asset = 222;
        p.keep_captured_position = t.category == "gatherable";
        std::string error;
        auto w = spawns::instantiate(t, mint, p, error);
        if (!w) {
            ++bad;
            if (first_bad.empty()) first_bad = t.archetype_path + ": " + error;
            continue;
        }
        ++checked;
        // the actor block
        for (const std::string& path : differing(t.actor_block().state, w->actor.obj)) {
            // (an instance has no attachment, no based movement and no root motion: instantiate leaves them unset)
            const bool left_unset = path.rfind("AttachmentReplication.", 0) == 0 || path.rfind("ReplicatedBasedMovement.", 0) == 0 || path.rfind("RepRootMotion.", 0) == 0;
            if (!allowed_actor.count(path) && !left_unset) {
                ++bad;
                if (first_bad.empty()) first_bad = t.archetype_path + ": the actor's " + path + " differs";
            }
        }
        // the subobjects: all there, in order, with a NetGUID each; the NPC info's guid, level and the alignments are the instance's
        if (w->actor.subobjects.size() + 1 != t.blocks.size()) {
            ++bad;
            if (first_bad.empty()) first_bad = t.archetype_path + ": a subobject is missing";
        }
        const std::set<std::string> allowed_info = {"CharacterGuid.A", "CharacterGuid.B", "CharacterGuid.C", "CharacterGuid.D", "Level", "ServerId"};
        for (size_t i = 0; i + 1 < t.blocks.size() && i < w->actor.subobjects.size(); ++i) {
            const repl::Subobject& s = w->actor.subobjects[i];
            const std::string cls = s.obj.schema->name;
            for (const std::string& path : differing(t.blocks[i + 1].state, s.obj)) {
                const bool ok = (cls == "UNPCInfo" && allowed_info.count(path)) || (cls == "UAlignmentComponent" && (path == "Alignments" || path == "InheritedAlignments")) ||
                                (cls == "UNPCSecondaryInfo" && (path == "PopInstanceId" || path == "PopAssetId" || path == "AppearanceComponent")) || (cls == "UNPCCombatInfo" && path == "bIsDead");
                if (!ok) {
                    ++bad;
                    if (first_bad.empty()) first_bad = t.archetype_path + ": " + cls + "." + path + " differs";
                }
            }
        }
        // the open writes and reads back
        repl::ActorShadow shadow;
        repl::ExportAcks acks;
        bool failed = false;
        const auto open = repl::open_actor(w->actor, shadow, acks, &failed);
        if (!open || failed) {
            ++bad;
            if (first_bad.empty()) first_bad = t.archetype_path + ": the open does not build";
            continue;
        }
        ++writes;
        const auto views = repl::read_content_blocks(open->payload.data(), open->payload.bit_pos(), /*start=*/0);
        (void)views;
        if (open->payload.bit_pos() == 0) ++unreadable;
    }
    if (!first_bad.empty()) std::printf("    first problem: %s\n", first_bad.c_str());
    CHECK(checked > 30 && bad == 0, "every NPC and gatherable template instantiates and differs from its capture only in the instance's values");
    CHECK(writes == checked && unreadable == 0, "every instance's open builds (SerializeNewActor and a block per object)");
}
// [Cyb x C O A]

static void test_names_and_references(const spawns::CapturedData& captured) {
    std::printf("an instance's name, authority and object references\n");
    const spawns::OpenTemplate* vendor = captured.template_for(spawns::kGmVendorArchetype);
    CHECK(vendor != nullptr, "the vendor template exists");
    if (!vendor) return;
    spawns::InstanceParams p;
    p.x = -777000; p.y = 616000; p.z = 5000; p.server_id = 2; p.name = "GM Gear 0-9"; p.clear_population = true;
    std::string error;
    auto w = spawns::instantiate(*vendor, mint, p, error);
    CHECK(w.has_value(), "the vendor instantiates");
    if (!w) return;
    // the nameplate's name is UNPCInfo.CharacterName (every retail NPC open has it); CreatureOwnerName is a pet's owner and stays as the template had it
    const repl::Subobject* info = nullptr;
    for (const repl::Subobject& s : w->actor.subobjects) if (std::string(s.obj.schema->name) == "UNPCInfo") info = &s;
    const repl::Value* name = info ? info->obj.state.get(*info->obj.schema, "CharacterName") : nullptr;
    const repl::Value* owner_name = info ? info->obj.state.get(*info->obj.schema, "CreatureOwnerName") : nullptr;
    CHECK(name && name->get_if<std::string>() && *name->get_if<std::string>() == "GM Gear 0-9" && w->npc.name == "GM Gear 0-9", "the vendor's CharacterName is the label it was given (the retail template's 'Siege Vendor' is replaced)");
    CHECK(!owner_name, "CreatureOwnerName is not set: the NPC is no one's pet");
    const repl::Subobject* second_info = nullptr;
    for (const repl::Subobject& s : w->actor.subobjects) if (std::string(s.obj.schema->name) == "UNPCSecondaryInfo") second_info = &s;
    const repl::Value* pi = second_info ? second_info->obj.state.get(*second_info->obj.schema, "PopInstanceId") : nullptr;
    const repl::Value* pa = second_info ? second_info->obj.state.get(*second_info->obj.schema, "PopAssetId") : nullptr;
    CHECK(pi && pa && pi->get_if<int64_t>() && *pi->get_if<int64_t>() == 0 && pa->get_if<int64_t>() && *pa->get_if<int64_t>() == 0,
          "a GM vendor names no population records (the client would apply the retail instance's DisplayNameOverrides 'Siege Vendor' over its name)");
    const repl::Value* auth = w->actor.obj.state.get(*w->actor.obj.schema, "AuthServerIDReplicated");
    const uint64_t* a = auth ? auth->get_if<uint64_t>() : nullptr;
    CHECK(a && *a == 0, "AuthServerIDReplicated is the class default 0, as the player's actors and the retail NPCs have it (not the mesh's server id)");
    // every object a value names is 0 or one of this instance's own objects: the actor, its subobjects, its AI controller object
    std::set<uint64_t> own = {w->actor.guid.ObjectId, w->npc.controller};
    for (const repl::Subobject& s : w->actor.subobjects) own.insert(s.guid.ObjectId);
    size_t refs = 0, foreign = 0;
    std::string first;
    const auto scan = [&](const repl::ObjectState& o) {
        for (const auto& [h, v] : o.state.values()) {
            const auto* g = v.get_if<FIntrepidNetworkGUID>();
            if (!g || g->ObjectId == 0) continue;
            ++refs;
            if (!own.count(g->ObjectId)) {
                ++foreign;
                if (first.empty()) first = std::string(o.schema->name) + " handle " + std::to_string(h) + " names object " + std::to_string(g->ObjectId);
            }
        }
    };
    scan(w->actor.obj);
    for (const repl::Subobject& s : w->actor.subobjects) scan(s.obj);
    if (!first.empty()) std::printf("    first foreign reference: %s\n", first.c_str());
    CHECK(refs >= 3 && foreign == 0, "every object reference of a vendor names its own objects (the AppearanceComponent is the minted NPCAppearanceComponent, not the retail server's NetGUID)");
    // the same for every NPC template that has an appearance component
    size_t looked = 0, bad = 0;
    for (const spawns::OpenTemplate& t : captured.opens()) {
        if (t.category != "npc" || t.find("UNPCAppearanceComponent") < 0) continue;
        spawns::InstanceParams q;
        q.x = -777000; q.y = 616000; q.z = 5000; q.server_id = 2;
        auto x = spawns::instantiate(t, mint, q, error);
        if (!x) { ++bad; continue; }
        const repl::Subobject* second = nullptr; const repl::Subobject* look = nullptr;
        for (const repl::Subobject& s : x->actor.subobjects) {
            if (std::string(s.obj.schema->name) == "UNPCSecondaryInfo") second = &s;
            if (std::string(s.obj.schema->name) == "UNPCAppearanceComponent") look = &s;
        }
        const repl::Value* v = second ? second->obj.state.get(*second->obj.schema, "AppearanceComponent") : nullptr;
        const auto* g = v ? v->get_if<FIntrepidNetworkGUID>() : nullptr;
        ++looked;
        if (!look || !g || g->ObjectId != look->guid.ObjectId) ++bad;
    }
    CHECK(looked >= 10 && bad == 0, "every NPC with an appearance component points its InteractInfo.AppearanceComponent at it");
}
// Cyb and C | O | A

static void test_population(const content::ContentDb& db, const spawns::CapturedData& captured) {
    std::printf("the population: the data's NPCs\n");
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    const auto data = db.population_data(error);
    CHECK(data.has_value(), "content.db's population chain reads");
    if (!data) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    std::printf("    read in %s: %zu assets, %zu instances, %zu sets, %zu placements, %zu areas\n", ms_since(t0).c_str(), data->assets.size(), data->instances.size(),
                data->sets.size(), data->placements.size(), data->areas.size());
    CHECK(data->assets.size() == 4576 && data->instances.size() == 16902 && data->sets.size() == 4728 && data->placements.size() == 15179 && data->areas.size() == 608,
          "the chain is the research's: 4,576 assets, 16,902 instances, 4,728 sets, 15,179 placements, 608 areas");
    const spawns::AreaConfig area;
    spawns::PopulationStats stats;
    const auto t1 = std::chrono::steady_clock::now();
    const auto spawns_a = spawns::select_data_spawns(*data, spawns::TemplateSource{&captured}, area, stats);
    std::printf("    selected in %s\n", ms_since(t1).c_str());
    spawns::PopulationStats stats_b;
    const auto spawns_b = spawns::select_data_spawns(*data, spawns::TemplateSource{&captured}, area, stats_b);
    bool same = spawns_a.size() == spawns_b.size();
    for (size_t i = 0; same && i < spawns_a.size(); ++i) {
        same = spawns_a[i].x == spawns_b[i].x && spawns_a[i].y == spawns_b[i].y && spawns_a[i].level == spawns_b[i].level && spawns_a[i].open == spawns_b[i].open &&
               spawns_a[i].seed == spawns_b[i].seed;
    }
    CHECK(same && !spawns_a.empty(), "the same inputs give the same NPCs, in the same order, with the same assets and levels");
    CHECK(stats.points_in_area == 3781 && stats.area_sets == 101,
          "3,781 placement points are inside a kilometre of the login spot and 101 area sets keep their NPCs there (the research's 3,929 counted 148 offsets of the area sets' members)");
    CHECK(stats.area_points == 817, "the circles of the 101 area sets hold 817 random points in the area");
    CHECK(stats.points_in_area + stats.area_points == stats.spawned + stats.blacklisted + stats.no_asset + stats.no_template, "every point is counted once: spawned or dropped for a reason");
    bool inside = true, levels = true, templates = true;
    for (const spawns::DataSpawn& s : spawns_a) {
        inside = inside && area.contains(s.x, s.y);
        levels = levels && s.level >= 0 && s.level <= 25 + 60;
        templates = templates && s.open && s.asset && s.open == captured.template_for(s.asset->archetype_path);
    }
    CHECK(inside, "every NPC is inside the area");
    CHECK(templates, "every NPC's template is the one of its asset's archetype");
    CHECK(levels, "levels are drawn from the instance's range");
    std::printf("    captured archetypes only: %zu points and %zu area points: %zu spawned, %zu with no captured archetype, %zu blacklisted, %zu without an asset\n", stats.points_in_area,
                stats.area_points, stats.spawned, stats.no_template, stats.blacklisted, stats.no_asset);
    for (const auto& [leaf, n] : stats.by_archetype) std::printf("      %4zu %s\n", n, leaf.c_str());
    std::vector<std::pair<size_t, std::string>> missing;
    for (const auto& [leaf, n] : stats.missing) missing.push_back({n, leaf});
    std::sort(missing.rbegin(), missing.rend());
    std::printf("    the archetypes no capture opened, most points first (%zu archetypes):\n", missing.size());
    for (size_t i = 0; i < missing.size() && i < 12; ++i) std::printf("      %4zu %s\n", missing[i].first, missing[i].second.c_str());
    // a smaller radius has fewer
    spawns::AreaConfig small;
    small.radius_cm = 25000;
    spawns::PopulationStats stats_small;
    const auto few = spawns::select_data_spawns(*data, spawns::TemplateSource{&captured}, small, stats_small);
    CHECK(few.size() < spawns_a.size() && stats_small.points_in_area < stats.points_in_area, "a smaller radius selects fewer NPCs");

    // the generic NPCs (stage 2 fix): a Blueprint no capture opened is made from its path in the data (spawns/generic_npc.h)
    const char* env_motion = std::getenv("AOC_NPC_MOTION");
    const std::string motion_path = env_motion && *env_motion ? env_motion : "dist/content/npc_motion.tsv";
    npc::MotionTable table;
    std::string table_error;
    if (!table.load(motion_path, table_error)) {
        std::printf("  [skip] %s: the generic NPC tests need npc_motion.tsv (tools/mapc: mapc build)\n", table_error.c_str());
        return;
    }
    spawns::GenericNpcs generic(captured, &table, spawns::GenericConfig{});
    CHECK(!generic.donor().empty(), "a plain creature's open the captures hold is the donor of the generic NPCs (the Wolf's)");
    spawns::PopulationStats gs;
    const auto all = spawns::select_data_spawns(*data, spawns::TemplateSource{&captured, &generic}, area, gs);
    CHECK(gs.points_in_area + gs.area_points == gs.spawned + gs.blacklisted + gs.no_asset + gs.no_template, "with the generic NPCs every point is still counted once");
    CHECK(gs.spawned > 2 * stats.spawned && gs.generic_spawned > 0 && gs.spawned == all.size(), "the generic NPCs more than double the NPCs of the area");
    size_t lost_props = 0;
    for (const auto& [leaf, n] : gs.lost_props) lost_props += n;
    CHECK(gs.no_template == lost_props + gs.lost_by_verdict[spawns::Verdict::NoDonor] + gs.lost_by_verdict[spawns::Verdict::NoTable] + gs.lost_by_verdict[spawns::Verdict::NoPath] +
                              gs.lost_by_verdict[spawns::Verdict::Excluded] + gs.lost_by_verdict[spawns::Verdict::Disabled],
          "every point with no NPC lost it for a reason the loader names: a prop, no donor, no table, no path, excluded or switched off");
    CHECK(gs.lost_by_verdict[spawns::Verdict::NoDonor] == 0 && gs.lost_by_verdict[spawns::Verdict::NoTable] == 0 && gs.lost_by_verdict[spawns::Verdict::NoPath] == 0,
          "no point is lost for want of a donor, a table or a Blueprint path");
    std::printf("    with the generic NPCs: %zu spawned (%zu generic, %zu of them from area circles), %zu lost (%zu props)\n", gs.spawned, gs.generic_spawned, gs.area_spawned, gs.no_template, lost_props);
    bool paths_ok = true, checksum_ok = true, distinct = true;
    std::set<uint64_t> guid_of_archetype;
    std::map<std::string, uint64_t> guid_by_path;
    for (const spawns::DataSpawn& sp : all) {
        if (!sp.generic) continue;
        paths_ok = paths_ok && sp.open && sp.open->archetype_path == sp.asset->archetype_path && sp.open->archetype.size() == 2;
        if (!sp.open || sp.open->archetype.size() != 2) continue;
        checksum_ok = checksum_ok && sp.open->archetype[0].has_checksum && sp.open->archetype[0].checksum == 0;
        const uint64_t id = sp.open->archetype[0].guid.ObjectId;
        const auto [it, fresh] = guid_by_path.emplace(sp.open->archetype_path, id);
        distinct = distinct && it->second == id && (!fresh || guid_of_archetype.insert(id).second) && captured.template_for(sp.open->archetype_path) == nullptr;
    }
    CHECK(paths_ok, "a generic NPC's archetype is its Blueprint's path from the data");
    CHECK(checksum_ok, "its export carries NetworkChecksum 0, which the client does not compare");
    CHECK(distinct, "each Blueprint has one NetGUID of its own, no two share one, and none is a captured archetype's");
    // every generic Blueprint's open instantiates and builds, and its archetype export names the Blueprint with checksum 0
    {
        size_t built = 0, bad = 0, named = 0;
        std::string first_bad;
        std::set<std::string> seen;
        for (const spawns::DataSpawn& sp : all) {
            if (!sp.generic || !seen.insert(sp.open->archetype_path).second) continue;
            spawns::InstanceParams p;
            p.x = sp.x;
            p.y = sp.y;
            p.z = sp.z;
            p.level = sp.level;
            p.server_id = 60;
            p.pop_instance = sp.instance ? sp.instance->guid : 0;
            p.pop_asset = sp.asset ? sp.asset->guid : 0;
            p.seed = sp.seed;
            std::string why;
            auto w = spawns::instantiate(*sp.open, mint, p, why);
            if (!w) {
                ++bad;
                if (first_bad.empty()) first_bad = sp.open->archetype_path + ": " + why;
                continue;
            }
            repl::ActorShadow shadow;
            repl::ExportAcks acks;
            bool failed = false;
            const auto open = repl::open_actor(w->actor, shadow, acks, &failed);
            if (!open || failed || open->payload.bit_pos() == 0) {
                ++bad;
                if (first_bad.empty()) first_bad = sp.open->archetype_path + ": the open does not build";
                continue;
            }
            ++built;
            // the archetype's NetGUID is among the GUIDs the open exports (with its path), and the export names the Blueprint's package
            const uint64_t archetype_id = sp.open->archetype[0].guid.ObjectId;
            bool exported = false;
            for (const auto& g : open->exported) exported = exported || g.ObjectId == archetype_id;
            named += exported && sp.open->archetype[1].name == sp.asset->archetype_path.substr(0, sp.asset->archetype_path.rfind('.')) ? 1 : 0;
        }
        if (!first_bad.empty()) std::printf("    first problem: %s\n", first_bad.c_str());
        std::printf("    %zu generic Blueprints: %zu open builds, %zu export their archetype by the Blueprint's path\n", seen.size(), built, named);
        CHECK(seen.size() > 150 && bad == 0 && built == seen.size() && named == built, "the open of every generic Blueprint builds and exports the Blueprint's path (no NPC is refused)");
    }
    // the kill switch: one type by name, and all of them
    spawns::GenericConfig cfg;
    cfg.exclude = {"Goblin_Gravehide", "hawk_c"};
    spawns::GenericNpcs without(captured, &table, cfg);
    spawns::PopulationStats xs;
    const auto fewer = spawns::select_data_spawns(*data, spawns::TemplateSource{&captured, &without}, area, xs);
    size_t gravehide = 0;
    for (const auto& [leaf, n] : gs.generic_by_archetype) {
        if (leaf == "Goblin_Gravehide_C") gravehide = n;
    }
    size_t left = 0;
    for (const spawns::DataSpawn& sp : fewer) left += spawns::blueprint_leaf(sp.asset->archetype_path) == "Goblin_Gravehide" || spawns::blueprint_leaf(sp.asset->archetype_path) == "Hawk" ? 1 : 0;
    CHECK(gravehide > 100 && xs.lost_by_verdict[spawns::Verdict::Excluded] > 0 && left == 0 && fewer.size() < all.size(),
          "excluding a Blueprint by name (case apart, with or without _C) drops exactly its NPCs and no others");
    spawns::GenericConfig off;
    off.enabled = false;
    spawns::GenericNpcs disabled(captured, &table, off);
    spawns::PopulationStats os;
    const auto only = spawns::select_data_spawns(*data, spawns::TemplateSource{&captured, &disabled}, area, os);
    CHECK(only.size() == spawns_a.size() && os.generic_spawned == 0, "with the generic NPCs switched off the area is stage 1's");
    spawns::GenericNpcs no_table(captured, nullptr, spawns::GenericConfig{});
    spawns::PopulationStats ns;
    const auto bare = spawns::select_data_spawns(*data, spawns::TemplateSource{&captured, &no_table}, area, ns);
    CHECK(bare.size() == spawns_a.size() && ns.lost_by_verdict[spawns::Verdict::NoTable] > 0, "without npc_motion.tsv no generic NPC is made and the loader says so");
}
// CoA.C | Y | B

static void test_load_area(const content::ContentDb& db, const spawns::CapturedData& captured) {
    std::printf("load_area: the registry\n");
    std::string error;
    auto gm = vendor::GmVendors::load(db, error);
    CHECK(gm.has_value(), "the GM vendors load");
    if (!gm) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    spawns::AreaSources sources;
    sources.db = &db;
    sources.captured = &captured;
    sources.gm = &*gm;
    world::ActorRegistry registry;
    const spawns::AreaConfig area;
    const auto t0 = std::chrono::steady_clock::now();
    const auto report = spawns::load_area(registry, sources, area, mint, 60, 0.0, error);
    CHECK(report.has_value(), "the area loads");
    if (!report) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    std::printf("    loaded in %s\n    %s\n", ms_since(t0).c_str(), report->describe().c_str());
    for (const std::string& n : report->notes) std::printf("    note: %s\n", n.c_str());
    CHECK(report->refused == 0, "no spawn was refused");
    CHECK(registry.size() == report->total(), "the registry holds every actor the report counts");
    CHECK(report->data_npcs > 400 && report->captured_npcs > 0 && report->gatherables >= 20 && report->gm_vendors == gm->vendors().size(),
          "NPCs from the data and from the captures, resource nodes, and one GM vendor per PTR def");
    size_t npcs = registry.count(world::ActorKind::Npc), nodes = registry.count(world::ActorKind::Gatherable);
    CHECK(npcs == report->data_npcs + report->captured_npcs + report->gm_vendors && nodes == report->gatherables, "the registry's kinds match the report");
    bool even = true, inside = true, controllers = true, interact = true;
    std::set<uint64_t> ids, components;
    size_t gm_count = 0;
    double gm_min = 1e18, gm_max = 0;
    registry.for_each([&](const world::WorldActor& a) {
        even = even && (a.id & 1) == 0 && a.id == a.actor.guid.ObjectId && ids.insert(a.id).second;
        if (a.npc.is_gm_vendor) {
            ++gm_count;
            const double d = std::hypot(a.x - area.center_x, a.y - area.center_y);
            gm_min = std::min(gm_min, d);
            gm_max = std::max(gm_max, d);
            interact = interact && a.npc.interact_component != 0 && components.insert(a.npc.interact_component).second && gm->find(a.npc.vendor_guid) != nullptr;
            return;
        }
        inside = inside && area.contains(a.x, a.y);
        if (a.kind == world::ActorKind::Npc) controllers = controllers && a.npc.controller != 0;
    });
    CHECK(even, "every id is even, unique and the actor's own ObjectId");
    CHECK(inside, "every actor but the GM vendors is inside the area");
    CHECK(controllers, "every NPC has an AI controller object");
    CHECK(gm_count == gm->vendors().size() && interact, "every GM vendor has its own InteractableComponent and a vendor def the catalog knows");
    CHECK(gm_min >= 700 && gm_max <= 1400, "the GM vendors stand 8 to 12.5 m from the login spot");
    // the vendor window is found from the component
    const world::WorldActor* any = nullptr;
    registry.for_each([&](const world::WorldActor& a) {
        if (!any && a.npc.is_gm_vendor) any = &a;
    });
    CHECK(any && registry.find_by_component(any->npc.interact_component) == any, "an InteractableComponent finds its actor");
    CHECK(registry.find_by_component(1) == nullptr && registry.find_by_component(0) == nullptr, "an unknown component finds nothing");
    // a kilometre of NPCs near each other: the nearest-first query and the radius
    std::vector<uint64_t> near;
    registry.query(area.center_x, area.center_y, 15000, near);
    std::printf("    %zu actors within 150 m of the login spot, %zu within 350 m\n", near.size(), [&] { std::vector<uint64_t> v; registry.query(area.center_x, area.center_y, 35000, v); return v.size(); }());
    CHECK(near.size() >= gm->vendors().size(), "the GM vendors are within the awake ring of the login spot");
    // the radius is the one place the size is set
    world::ActorRegistry small_registry;
    spawns::AreaConfig small;
    small.radius_cm = 30000;
    const auto small_report = spawns::load_area(small_registry, sources, small, mint, 60, 0.0, error);
    CHECK(small_report && small_report->total() < report->total() && small_report->gm_vendors == report->gm_vendors, "a radius of 300 m loads fewer actors, the same GM vendors");
    // no GM sources: no vendors
    spawns::AreaSources none = sources;
    none.gm = nullptr;
    world::ActorRegistry no_gm;
    const auto r3 = spawns::load_area(no_gm, none, small, mint, 60, 0.0, error);
    CHECK(r3 && r3->gm_vendors == 0, "without a GM catalog there are no GM vendors");
    // and two loads of the same inputs make the same population (positions and levels)
    world::ActorRegistry again;
    g_next = 900000;
    const auto r4 = spawns::load_area(again, sources, small, mint, 60, 0.0, error);
    world::ActorRegistry again2;
    g_next = 900000;
    const auto r5 = spawns::load_area(again2, sources, small, mint, 60, 0.0, error);
    bool same = r4 && r5 && again.size() == again2.size();
    if (same) {
        again.for_each([&](const world::WorldActor& a) {
            const world::WorldActor* b = again2.find(a.id);
            same = same && b && b->x == a.x && b->y == a.y && b->npc.level == a.npc.level && b->npc.name == a.npc.name;
        });
    }
    CHECK(same, "the same inputs and ids make the same world, level for level");
}
// -- C | O | A x C | Y | B --

/// Mounts, stage 1: the donors of spawns/mount_donors.h (tools/capture/gen_mount.py), the opens stage 2 makes a summoned mount from: one per kind, the
/// actor block and eight subobjects each, the archetype exported (the Blueprint's Default__ object under /Game/GameBlueprints/Mounts/) and, where content.db
/// names the Blueprint of a mount item, the same Blueprint's path.
static void test_mount_donors(const content::ContentDb& db) {
    namespace md = spawns::mount_donors;
    std::printf("the mount donors\n");
    std::set<std::string> kinds;
    bool shape = true, archetypes = true, components = true;
    for (const md::Donor& d : md::kDonors) {
        kinds.insert(d.kind);
        const md::Open& o = md::kOpens[d.open];
        shape = shape && o.blocks == 9 && std::string(o.actor_class) == "ANPCCharacter" && o.location.present && md::kBlocks[o.first_block].kind == 0 &&
                std::string(md::kBlocks[o.first_block].cls) == "ANPCCharacter";
        const md::Chain& ac = md::kChains[o.archetype_chain];
        const md::Link& arch = md::kLinks[ac.first];
        archetypes = archetypes && std::string(o.archetype).rfind("/Game/GameBlueprints/Mounts/", 0) == 0 && std::string(arch.name).rfind("Default__Mount_", 0) == 0 &&
                     arch.checksum == 47039926u && ac.count == 2;
        const std::string want = std::string(d.kind) == "ground" ? "GroundMountComponent" : std::string(d.kind) == "aquatic" ? "AquaticMountComponent" : "FlyingMountComponent";
        bool has_mount_component = false;
        for (uint32_t i = 1; i < o.blocks; ++i) {
            const md::Block& b = md::kBlocks[o.first_block + i];
            if (std::string(b.cls) == "UMountComponent") has_mount_component = std::string(b.name) == want && b.checksum != 0 && b.kind == 1;
            components = components && b.kind == 1 && b.checksum != 0 && !std::string(b.name).empty();
        }
        components = components && has_mount_component;
    }
    CHECK(md::kDonors[0].open == 0 && sizeof(md::kDonors) / sizeof(md::kDonors[0]) == 3 && kinds.size() == 3 && kinds.count("ground") && kinds.count("aquatic") && kinds.count("flying"),
          "three donors: a ground, an aquatic and a flying mount");
    CHECK(shape && archetypes && components, "each is an ANPCCharacter open with eight stably named subobjects (the MountComponent named for its kind) and its archetype exported");
    // the donors' Blueprints are items of the data: the terror bird, the shoalskipper and the Dawnbreaker
    std::string error;
    const auto mounts = db.mount_data(error);
    std::set<std::string> from_data;
    if (mounts)
        for (const content::MountRow& m : mounts->mounts) from_data.insert(m.blueprint_path);
    bool all_in_data = mounts.has_value();
    for (const md::Donor& d : md::kDonors) {
        std::string path = md::kOpens[d.open].archetype;   // .../Mount_X.Default__Mount_X_C is the class default object of .../Mount_X.Mount_X_C
        if (const size_t at = path.find(".Default__"); at != std::string::npos) path.replace(at, 10, ".");
        all_in_data = all_in_data && from_data.count(path) == 1;
    }
    CHECK(all_in_data, "the donors' archetypes are the Blueprints content.db's mount items name (SummonNotifyClassPath)");
}

static void test_gm_vendors(const content::ContentDb& db) {
    std::printf("the GM vendors\n");
    std::string error;
    auto gm = vendor::GmVendors::load(db, error);
    if (!gm) {
        CHECK(false, "the GM vendors load");
        return;
    }
    const auto& defs = gm->vendors();
    std::printf("    %zu PTR vendor defs, %zu item records in their stock; %zu GM/PTR item records: %zu in a stock, %zu in none\n", defs.size(), gm->stocked_records(),
                gm->gm_item_count(), gm->gm_items_stocked(), gm->unstocked().size());
    CHECK(defs.size() == 26, "26 PTR_ONLY vendor defs");
    CHECK(vendor::label_of("PTR_ONLY_Vendor_Gear_0-9") == "GM Gear 0-9" && vendor::kind_of("PTR_ONLY_Vendor_Gear_0-9") == "Gear" &&
              vendor::kind_of("PTR_ONLY_Vendor_Recipes_Crafting_ANV") == "Recipes" && vendor::label_of("PTR_ONLY_Vendor_Materials") == "GM Materials",
          "labels and kinds from the def's name");
    bool stocked = true, labelled = true;
    std::set<std::string> labels;
    for (const vendor::GmVendor& v : defs) {
        stocked = stocked && !v.items.empty() && v.listings >= v.items.size() / 2;
        labelled = labelled && labels.insert(v.label).second && gm->find(v.guid) == &v;
    }
    CHECK(stocked && labelled, "every def has a stock and a label of its own");
    CHECK(gm->gm_item_count() > 400 && gm->gm_items_stocked() + gm->unstocked().size() == gm->gm_item_count(), "the GM / PTR item records are counted: stocked and not");

    // mounts, stage 1: the mounts def sells every mount item (the def lists 20 ground mounts; the 81 others are added to its stock)
    {
        std::string merror;
        const auto md = db.mount_data(merror);
        const auto it = std::find_if(defs.begin(), defs.end(), [](const vendor::GmVendor& v) { return v.name == vendor::kMountsVendorName; });
        CHECK(md && md->mounts.size() == 101 && it != defs.end() && gm->mount_error().empty() && gm->mounts_total() == 101, "the mount tables are read and the mounts def is among the 26");
        if (md && it != defs.end()) {
            bool all = true;
            for (const content::MountRow& m : md->mounts) all = all && it->items.count(m.item) == 1;
            CHECK(all, "the mounts def's stock holds all 101 mount items (51 ground, 46 flying, 4 aquatic, the GM Eagle Gryphon among them)");
            CHECK(gm->mounts_added() == 81, "81 of them are added to the 20 the def lists");
            // a cart for a flying mount the game's own def does not list is accepted by the mounts vendor; the same cart is refused by another def
            const content::MountRow* flyer = nullptr;
            const content::MountRow* gryphon = nullptr;
            for (const content::MountRow& m : md->mounts) {
                if (m.item_name == "Mount_Flying_Dawnbreaker_1") flyer = &m;
                if (m.item_name == "Mount_Flying_PTR_ONLY") gryphon = &m;
            }
            repl::VendorPurchaseCart c1;
            c1.vendor = it->guid;
            c1.items.push_back({flyer ? flyer->item : 0, 1, std::nullopt, std::nullopt});
            c1.items.push_back({gryphon ? gryphon->item : 0, 1, std::nullopt, std::nullopt});
            const auto p1 = gm->plan_purchase(db, *it, c1, 70);
            CHECK(flyer && gryphon && p1.ok && p1.stacks.size() == 2 && p1.stacks[0].record == flyer->item && p1.stacks[0].quantity == 1 && p1.stacks[1].record == gryphon->item,
                  "a cart of a Dawnbreaker and the GM Eagle Gryphon: two stacks of one");
            const auto other = std::find_if(defs.begin(), defs.end(), [](const vendor::GmVendor& v) { return v.name == "PTR_ONLY_Vendor_Materials"; });
            repl::VendorPurchaseCart c2;
            c2.vendor = other->guid;
            c2.items.push_back({flyer->item, 1, std::nullopt, std::nullopt});
            CHECK(other != defs.end() && !other->items.count(flyer->item) && !gm->plan_purchase(db, *other, c2, 70).ok, "the materials vendor does not sell a mount");
            repl::VendorPurchaseCart c3;
            c3.vendor = it->guid;
            for (size_t i = 0; i < 3; ++i) c3.items.push_back({md->mounts[i].item, 1, std::nullopt, std::nullopt});
            CHECK(!gm->plan_purchase(db, *it, c3, 2).ok, "three mounts do not fit two free slots: the cart is refused whole");
        }
    }
// (CoA + C | Y | B)

    // the layout
    const spawns::AreaConfig area;
    const auto layout = vendor::gm_layout(defs.size(), area, spawns::kLoginSpotYaw, spawns::kLoginSpotZ);
    bool spaced = layout.size() == defs.size();
    double nearest = 1e18;
    for (size_t i = 0; i < layout.size(); ++i) {
        for (size_t j = i + 1; j < layout.size(); ++j) nearest = std::min(nearest, std::hypot(layout[i].x - layout[j].x, layout[i].y - layout[j].y));
    }
    CHECK(spaced && nearest > 150.0, "the layout puts every NPC at least 1.5 m from every other");
    CHECK(vendor::gm_layout(0, area, 0, 0).empty() && vendor::gm_layout(1, area, 0, 0).size() == 1 && vendor::gm_layout(13, area, 0, 0).size() == 13, "layouts of 0, 1 and 13");

    // the purchase plans on the real records
    const vendor::GmVendor& gear = *std::find_if(defs.begin(), defs.end(), [](const vendor::GmVendor& v) { return v.name == "PTR_ONLY_Vendor_Gear_0-9"; });
    const vendor::GmVendor& materials = *std::find_if(defs.begin(), defs.end(), [](const vendor::GmVendor& v) { return v.name == "PTR_ONLY_Vendor_Materials"; });
    const int64_t gear_item = *gear.items.begin();
    repl::VendorPurchaseCart cart;
    cart.vendor = gear.guid;
    cart.items.push_back({gear_item, 1, std::nullopt, std::nullopt});
    auto plan = gm->plan_purchase(db, gear, cart, 70);
    CHECK(plan.ok && plan.stacks.size() == 1 && plan.stacks[0].record == gear_item && plan.stacks[0].quantity == 1, "one item of the gear def: one stack of one");
    // materials: stacks of MaxStackSize
    int64_t stackable = 0;
    int32_t max_stack = 1;
    for (const int64_t item : materials.items) {
        std::string e;
        const auto rec = db.item(item, e);
        if (rec && rec->max_stack_size > 1) {
            stackable = item;
            max_stack = rec->max_stack_size;
            break;
        }
    }
    CHECK(stackable != 0, "the materials def lists an item that stacks");
    repl::VendorPurchaseCart mats;
    mats.vendor = materials.guid;
    mats.items.push_back({stackable, max_stack * 2 + 1, std::nullopt, std::nullopt});
    plan = gm->plan_purchase(db, materials, mats, 70);
    CHECK(plan.ok && plan.stacks.size() == 3 && plan.stacks[0].quantity == max_stack && plan.stacks[1].quantity == max_stack && plan.stacks[2].quantity == 1,
          "a quantity of two stacks and one is three stacks: MaxStackSize, MaxStackSize, 1");
    plan = gm->plan_purchase(db, materials, mats, 2);
    CHECK(!plan.ok && plan.stacks.empty() && plan.refusal.find("free slot") != std::string::npos, "a bag with two free slots refuses it whole");
    // refusals
    repl::VendorPurchaseCart wrong = cart;
    wrong.vendor = materials.guid;
    CHECK(!gm->plan_purchase(db, gear, wrong, 70).ok, "a cart naming another vendor is refused");
    repl::VendorPurchaseCart unlisted = cart;
    unlisted.items[0].item_record = 1;
    CHECK(!gm->plan_purchase(db, gear, unlisted, 70).ok, "an item no listing has is refused");
    for (const int32_t q : {0, -1, vendor::kMaxQuantity + 1, INT32_MIN, INT32_MAX}) {
        repl::VendorPurchaseCart c = cart;
        c.items[0].quantity = q;
        CHECK(!gm->plan_purchase(db, gear, c, 70).ok, ("a quantity of " + std::to_string(q) + " is refused").c_str());
    }
    repl::VendorPurchaseCart empty;
    empty.vendor = gear.guid;
    CHECK(!gm->plan_purchase(db, gear, empty, 70).ok, "an empty cart is refused");
    repl::VendorPurchaseCart many = cart;
    many.items.assign(vendor::kMaxCartItems + 1, cart.items[0]);
    CHECK(!gm->plan_purchase(db, gear, many, 70).ok, "a cart of 71 items is refused");
    repl::VendorPurchaseCart huge = mats;
    huge.items[0].quantity = vendor::kMaxQuantity;
    const auto big = gm->plan_purchase(db, materials, huge, 70);
    CHECK(big.ok == (static_cast<int64_t>(vendor::kMaxQuantity + max_stack - 1) / max_stack <= 70), "the largest quantity costs stacks, never a loop over the quantity");
    repl::VendorPurchaseCart zero = cart;
    CHECK(!gm->plan_purchase(db, gear, zero, 0).ok, "no free slot at all refuses even one item");
    // hidden listings are not sold
    size_t listed = 0;
    for (const vendor::GmVendor& v : defs) listed += v.listings;
    CHECK(listed > 5000, "the defs list thousands of listings");
}

static void test_player_world(const content::ContentDb& db) {
    std::printf("PlayerWorld::load_area on the real content.db\n");
    aoc::protocol::ServerGuidAllocator guids(60, 0x6ee87ee4u);
    PlayerWorld world(&db, "", bootstrap::GameStateMode::Tier2, 60, guids);
    CHECK(world.ready() && world.registry().size() == 0 && world.area_report() == nullptr, "a world with content.db is ready and has no other actors until the area is loaded");
    std::string error;
    spawns::AreaConfig small;
    small.radius_cm = 40000;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = world.load_area(small, error);
    CHECK(ok, "load_area loads a 400 m area");
    if (!ok) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    std::printf("    %s in %s\n", world.area_report()->describe().c_str(), ms_since(t0).c_str());
    CHECK(world.registry().size() == world.area_report()->total() && world.gm_vendors() && world.gm_vendors()->vendors().size() == 26, "the registry holds what the report counts; the GM catalog is the world's");
    CHECK(world.area().radius_cm == 40000.0 && !world.load_area(small, error), "the area is the world's, and loading it twice is refused");
    std::set<uint64_t> ids;
    bool unique = true;
    world.registry().for_each([&](const world::WorldActor& a) { unique = unique && ids.insert(a.id).second && a.actor.guid.ServerId == 60; });
    CHECK(unique, "every id is unique and under the world's ServerId");
    // a world whose content.db has no population tables does not load an area and says why
    aoc::protocol::ServerGuidAllocator guids2(60, 0x6ee87ee4u);
    PlayerWorld bare(nullptr, "no content.db", bootstrap::GameStateMode::Tier2, 60, guids2);
    CHECK(!bare.load_area(small, error) && error.find("content.db") != std::string::npos && bare.registry().size() == 0, "a world with no content.db refuses to load an area");
}

// (CoA | C_Y_B)

/// plan E1 stage 2: the ground.  The NPCs of the real area stand on the extracted ground (dist/content/area_nav.bin, tools/mapc) one capsule half height up, wander and
/// patrol when a player is near and stand still when none is; without the file they stay where stage 1 put them.
static void test_ground(const content::ContentDb& db) {
    std::printf("the ground (stage 2): the NPCs on the real area's navmesh\n");
    const char* env_nav = std::getenv("AOC_AREA_NAV");
    const std::string nav_path = env_nav && *env_nav ? env_nav : "dist/content/area_nav.bin";
    const char* env_motion = std::getenv("AOC_NPC_MOTION");
    const std::string motion_path = env_motion && *env_motion ? env_motion : "dist/content/npc_motion.tsv";
    std::string error;
    spawns::AreaConfig area;
    const auto make_world = [&](aoc::protocol::ServerGuidAllocator& g) {
        auto w = std::make_unique<PlayerWorld>(&db, "", bootstrap::GameStateMode::Tier2, 60, g);
        std::string e, table_error;
        if (!w->load_blueprints(motion_path, table_error)) std::printf("    (no Blueprint table: %s)\n", table_error.c_str());
        if (!w->load_area(area, e)) { std::printf("    the area does not load: %s\n", e.c_str()); std::exit(2); }
        return w;
    };
    aoc::protocol::ServerGuidAllocator guids(60, 0x6ee87ee4u);
    auto w = make_world(guids);
    std::map<uint64_t, std::array<double, 3>> before;
    w->registry().for_each([&](const world::WorldActor& a) { before[a.id] = {a.x, a.y, a.z}; });
    // no ground: nothing changes, the NPCs stand where stage 1 put them, and the error says what is missing
    CHECK(!w->load_ground("no/such/area_nav.bin", motion_path, error) && error.find("cannot open") != std::string::npos && w->npcs() == nullptr && w->nav() == nullptr,
          "a missing navmesh file: no ground, no NPC world, a message");
    bool same = true;
    w->registry().for_each([&](const world::WorldActor& a) { same = same && before[a.id] == std::array<double, 3>{a.x, a.y, a.z}; });
    CHECK(same, "...and every actor is where it was");
    w->tick_world(10.0);
    w->note_player(1, area.center_x, area.center_y);
    w->forget_player(1);
    CHECK(true, "tick_world, note_player and forget_player without a ground do nothing (no crash)");
    {
        FILE* f = std::fopen(nav_path.c_str(), "rb");
        if (!f) {
            std::printf("  [skip] %s is not there (tools/mapc: mapc build): the rest of the ground tests need it\n", nav_path.c_str());
            return;
        }
        std::fclose(f);
    }
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(w->load_ground(nav_path, motion_path, error), "the real ground loads");
    if (!w->npcs()) { std::printf("    %s\n", error.c_str()); return; }
    const npc::GroundReport rep = *w->ground_report();
    std::printf("    loaded and populated in %s: %s\n", ms_since(t0).c_str(), rep.describe().c_str());
    CHECK(!w->load_ground(nav_path, motion_path, error), "the ground is loaded once");
    const size_t npcs = w->registry().count(world::ActorKind::Npc);
    CHECK(rep.npcs == npcs && rep.on_ground + rep.no_ground == npcs, "every NPC of the registry is under the NPC world's care");
    CHECK(rep.on_ground >= npcs * 0.95, "at least 95 % of the NPCs found a ground (the rest are outside the extracted terrain or over a hole)");
    CHECK(rep.wander > 100 && rep.stationary > 100, "wanderers and standing NPCs both");
    // the answer key (tools/capture/gen_npc_motion.py): every archetype the retail captures show moving in most of its observed epochs (of instances that may
    // move) must walk here; where the data's instances of it are all Stationary it stands, as the retail captures' Stationary instances do (their epochs are in
    // the key too).  Fliers stay in the air: there are none, retail's creatures all walk (GroundReport::by_type)
    {
        size_t keyed = 0, present = 0, still_epochs = 0, still_moved = 0;
        std::vector<std::string> never_walks, stands_by_data;
        for (size_t i = 0; i < npc::kRetailMotionCount; ++i) {
            const npc::RetailMotion& rm = npc::kRetailMotion[i];
            still_epochs += static_cast<size_t>(rm.still);
            still_moved += static_cast<size_t>(rm.still_moving);
            if (rm.epochs < 3 || rm.moving * 2 < rm.epochs) continue;
            ++keyed;
            const auto it = rep.by_type.find(npc::archetype_leaf(rm.archetype));
            if (it == rep.by_type.end()) continue;
            ++present;
            const auto& c = it->second;
            if (c.wander + c.patrol > 0) continue;
            const std::string what = it->first + " (" + std::to_string(c.total) + " NPCs: " + std::to_string(c.stand_by_data) + " Stationary by the data, " + std::to_string(c.stand_no_life) +
                                     " with no life of their own; retail: " + std::to_string(rm.moving) + " of " + std::to_string(rm.epochs) + " epochs moved, its Stationary instances " +
                                     std::to_string(rm.still_moving) + " of " + std::to_string(rm.still) + ")";
            (c.stand_no_life > 0 ? never_walks : stands_by_data).push_back(what);
        }
        std::printf("    the answer key: %zu archetypes moved in most of their observed retail epochs, %zu of them are in the area; the Stationary instances' epochs: %zu, %zu moved%s", keyed, present,
                    still_epochs, still_moved, chr_n());
        for (const std::string& s : stands_by_data) std::printf("      stands, as retail's Stationary instances do: %s%s", s.c_str(), chr_n());
        for (const std::string& s : never_walks) std::printf("      STANDS: %s%s", s.c_str(), chr_n());
        CHECK(keyed >= 20 && present >= 10, "the answer key names many archetypes of the area");
        CHECK(never_walks.empty(), "every archetype of the area that retail moves in most of its epochs walks here, but where the data's instances of it are Stationary");
        std::map<std::string, size_t> modes;
        for (const auto& [name, c] : rep.by_type) { modes["stand"] += c.stand; modes["wander"] += c.wander; modes["patrol"] += c.patrol; }
        CHECK(rep.fly == 0 && modes["stand"] == rep.stationary && modes["wander"] == rep.wander && modes["patrol"] == rep.patrol,
              "the per-type counts add up to the stand / wander / patrol / fly counts (no NPC flies: retail's creatures all walk, hawks and ravens included)");
        std::printf("    stand / wander / patrol / fly: %zu / %zu / %zu / %zu in %zu types%s", rep.stationary, rep.wander, rep.patrol, rep.fly, rep.by_type.size(), chr_n());
        for (const std::string& line : rep.describe_types(10)) std::printf("      %s%s", line.c_str(), chr_n());
    }
    // the standing: terrain + the capsule's half height times the scale, to 1.5 cm, for every NPC that stands on the terrain
    size_t on_terrain = 0, off_terrain = 0, gm_checked = 0, gm_exact = 0;
    double worst = 0;
    w->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind != world::ActorKind::Npc || !w->npcs()->has(a.id)) return;
        double tz = 0;
        const bool has = w->nav()->terrain_height(a.x, a.y, &tz);
        const double foot = w->npcs()->mover(a.id)->foot_offset();
        if (has && std::fabs(a.z - foot - tz) < 1.5) ++on_terrain;
        else { ++off_terrain; worst = std::max(worst, has ? std::fabs(a.z - foot - tz) : 0.0); }
        if (a.npc.is_gm_vendor) {
            ++gm_checked;
            double gz = 0;
            const bool exact = w->nav()->ground_height(a.x, a.y, a.z - foot, &gz, 100, 100) && std::fabs(a.z - foot - gz) < 1.5;   // the terrain, or a floor (a step of the ruins)
            gm_exact += exact ? 1 : 0;
            if (!exact) std::printf("    GM vendor %s at (%.0f, %.0f): origin %.1f, foot %.1f, terrain %s%.1f\n", a.npc.name.c_str(), a.x, a.y, a.z, foot, has ? "" : "none ", has ? tz : 0.0);
        }
    });
    std::printf("    %zu NPCs stand on the terrain exactly, %zu elsewhere (a floor, no ground; the most %.0f cm off the terrain)\n", on_terrain, off_terrain, worst);
    CHECK(on_terrain >= npcs * 0.80, "most NPCs stand on the terrain: origin = its height + the half height times the scale (to 1.5 cm)");
    CHECK(gm_checked == 26 && gm_exact == 26, "all 26 GM vendors stand on the ground (the terrain or a floor) at the login spot");
    // replicated state agrees with the registry's
    size_t rm_bad = 0;
    w->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind != world::ActorKind::Npc || !a.actor.obj.schema) return;
        const repl::Value* v = a.actor.obj.state.get(*a.actor.obj.schema, "ReplicatedMovement");
        const auto* m = v ? v->get_if<repl::RepMovement>() : nullptr;
        if (!m) return;
        const auto loc = repl::dequantize_location(a.actor.obj.opts, m->location);
        if (std::fabs(loc.x - a.x) > 0.02 || std::fabs(loc.y - a.y) > 0.02 || std::fabs(loc.z - a.z) > 0.02) ++rm_bad;
    });
    CHECK(rm_bad == 0, "every NPC's ReplicatedMovement is where the registry puts it (to 0.01 cm)");

    // E1 review A1: a respawn is a fresh actor with its OWN CollisionCylinder (the actor's + 4), made while the corpse still stands (an unlooted corpse
    // outlives most respawn times). The first generation's cylinder stayed in the respawn's NpcInfo: the registry refused the respawn while the corpse
    // held that GUID (the spawn point never refilled), and once the corpse was gone the first hit exported it under the new actor (the fix-3
    // NetGuidMismatch disconnect again).
    // (in a world of its own: the two-world comparison below must not see the kill)
    {
        aoc::protocol::ServerGuidAllocator guids_r(60, 0x6ee87ee4u);
        auto rw = make_world(guids_r);
        std::string e_r;
        CHECK(rw->load_ground(nav_path, motion_path, e_r) && rw->combat() != nullptr && rw->aftermath() != nullptr, "a world of its own for the respawn, with its ground, its fights and its aftermath");
        uint64_t victim = 0;
        if (rw->combat() && rw->aftermath()) {
            rw->registry().for_each([&](const world::WorldActor& a) {
                if (victim == 0 && a.kind == world::ActorKind::Npc && a.npc.spawn_ref != 0 && a.npc.respawn_rule != "NoRespawn" && !a.npc.is_gm_vendor &&
                    a.npc.collision_cylinder == a.id + 4 && rw->npcs()->has(a.id))
                    victim = a.id;
            });
        }
        CHECK(victim != 0, "a data NPC with a spawn point, its cylinder at its id + 4");
        if (victim != 0) {
            const uint32_t ref = rw->registry().find(victim)->npc.spawn_ref;
            const uint64_t old_cylinder = rw->registry().find(victim)->npc.collision_cylinder, old_interact = rw->registry().find(victim)->npc.interact_component;
            rw->registry().modify(victim, world::Change::State, [](world::WorldActor& a) {
                a.npc.respawn_seconds = 2.0;
                a.npc.respawn_variance = 0.0;
                a.npc.respawn_rule = "Refill";
            });
            combat::Resolved r;
            r.mods.push_back({1, content::kGuidHealth, -1.0e9});
            double now = 50.0;
            rw->tick_world(now);
            const combat::Delivery d = rw->combat()->apply_hit_to_npc(victim, 0, r, now);
            CHECK(d.applied && d.lethal && rw->aftermath()->respawns_pending() == 1, "a hit of a billion kills it, and its spawn point is due to refill in 2 s");
            for (int i = 0; i < 30; ++i) rw->tick_world(now += 0.1);   // 3 s: the respawn comes while the corpse (15 s, nobody fought it) still stands
            uint64_t fresh = 0;
            rw->registry().for_each([&](const world::WorldActor& a) {
                if (a.id != victim && a.npc.spawn_ref == ref) fresh = a.id;
            });
            const world::WorldActor* corpse = rw->registry().find(victim);
            CHECK(corpse != nullptr && fresh != 0 && rw->aftermath()->stats().respawns == 1, "the spawn point refilled while the corpse still stands");
            const world::WorldActor* f = fresh ? rw->registry().find(fresh) : nullptr;
            // (a generic Blueprint's NPC has no InteractableComponent: 0 both times)
            const bool own_interact = f && (old_interact == 0 ? f->npc.interact_component == 0 : f->npc.interact_component != old_interact);
            CHECK(f && f->npc.collision_cylinder == fresh + 4 && f->npc.collision_cylinder != old_cylinder && f->npc.controller == fresh + 2 && own_interact,
                  "the respawn's cylinder, controller and interactable are its own block's, not the first generation's");
            CHECK(f && rw->registry().find_by_component(f->npc.collision_cylinder) == f && rw->combat()->hit_component_of(fresh) == f->npc.collision_cylinder,
                  "...and the registry and the combat world name the new cylinder");
            CHECK(f && corpse && f->npc.level == corpse->npc.level && f->npc.name == corpse->npc.name && f->npc.spawn_ref == ref && f->npc.loot_tables == corpse->npc.loot_tables &&
                      rw->npcs()->has(fresh),
                  "the recipe's data came along, and the NPC world walks it");
        }
    }
// (Cyb + C | O | A)

    // a wander target gives a path: from where each of a sample of the NPCs stands, eight targets 6 to 17 m away
    {
        size_t tried = 0, no_start = 0, no_target = 0, no_path = 0, partial = 0, too_long = 0, ok = 0, npcs_ok = 0, npcs_sampled = 0, i = 0;
        w->registry().for_each([&](const world::WorldActor& a) {
            if (a.kind != world::ActorKind::Npc || !w->npcs()->has(a.id) || (++i % 7) != 0) return;
            const auto* m = w->npcs()->mover(a.id);
            const nav::Vec3 g = m->ground();
            ++npcs_sampled;
            bool any = false;
            for (int k = 0; k < 8; ++k) {
                const double ang = (k + 0.5) * 0.7853981633974483, r = 600.0 + 150.0 * k;
                nav::Vec3 np;
                ++tried;
                if (!w->nav()->nearest_point({g.x, g.y, g.z}, 200, 400, &np)) { ++no_start; continue; }
                if (!w->nav()->nearest_point({g.x + std::cos(ang) * r, g.y + std::sin(ang) * r, g.z}, 150, 300, &np)) { ++no_target; continue; }
                const auto pr = w->nav()->find_path(g, np, 200, 400);
                if (!pr.found) { ++no_path; continue; }
                if (!pr.complete) { ++partial; continue; }
                if (pr.length > r * 3.0 + 500.0) { ++too_long; continue; }
                ++ok;
                any = true;
            }
            npcs_ok += any ? 1 : 0;
        });
        std::printf("    %zu NPCs sampled, %zu with a wander path: of %zu targets %zu are off the navmesh (%zu at the start), %zu have no path, %zu a partial one, %zu a long detour, %zu are good\n", npcs_sampled,
                    npcs_ok, tried, no_target, no_start, no_path, partial, too_long, ok);
        CHECK(npcs_sampled > 100 && npcs_ok * 100 >= npcs_sampled * 90, "from where it stands 90 % of the NPCs can walk to a wander target");
    }
    // life: a player at the login spot for two minutes
    std::map<uint64_t, std::array<double, 3>> start;
    w->registry().for_each([&](const world::WorldActor& a) { start[a.id] = {a.x, a.y, a.z}; });
    double t = 100.0, total_us = 0, max_us = 0;
    size_t steps = 0;
    // the NPCs near the player that have a life of their own (the data wanders or patrols them and they found a ground): each walks at least once
    std::vector<uint64_t> lively;
    w->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Npc && w->npcs()->has(a.id) && !w->npcs()->stands(a.id) && std::hypot(a.x - area.center_x, a.y - area.center_y) <= 15000.0) lively.push_back(a.id);
    });
    std::sort(lively.begin(), lively.end());
    std::set<uint64_t> walked;
    for (int i = 0; i < 1800; ++i) {
        t += 1.0 / 15.0;
        if (i % 15 == 0) {
            for (const uint64_t id : lively) {
                const world::WorldActor* a = w->registry().find(id);
                const auto& s = start[id];
                if (a && std::hypot(a->x - s[0], a->y - s[1]) > 100.0) walked.insert(id);
            }
        }
        w->note_player(1, area.center_x, area.center_y);
        const auto s0 = std::chrono::steady_clock::now();
        w->tick_world(t);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - s0).count();
        total_us += us;
        max_us = std::max(max_us, us);
        ++steps;
    }
    size_t moved = 0, moved_far = 0, awake = 0;
    double farthest = 0;
    w->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind != world::ActorKind::Npc) return;
        const auto& s = start[a.id];
        const double d0 = std::hypot(s[0] - area.center_x, s[1] - area.center_y);
        const double moved_by = std::hypot(a.x - s[0], a.y - s[1]);
        if (d0 <= 17000.0) ++awake;
        if (moved_by > 100.0) {
            ++moved;
            farthest = std::max(farthest, moved_by);
            if (d0 > 17000.0 + 2000.0 + 2500.0) ++moved_far;   // (it can only have walked in from the edge of the margin)
        }
    });
    std::printf("    a player at the login spot for 120 s: %zu NPCs within 170 m, %zu of the area moved more than 1 m (up to %.0f cm), %zu of those began beyond the margin\n", awake, moved,
                farthest, moved_far);
    std::printf("    %zu steps: %.0f us on average, %.0f us at most; %llu legs, %llu registry writes\n", steps, total_us / static_cast<double>(steps), max_us,
                static_cast<unsigned long long>(w->npcs()->stats().moves_started), static_cast<unsigned long long>(w->npcs()->stats().writes));
    {
        const auto st = w->npcs()->stats();
        std::printf("    NPCs now: %zu standing, %zu idling, %zu wandering, %zu patrolling, %zu pausing on a patrol, %zu returning; %zu active now, %llu wander/patrol targets with no path\n", st.standing, st.idling,
                    st.wandering, st.patrolling, st.patrol_pausing, st.returning, st.active_now, static_cast<unsigned long long>(st.no_path));
    }
    {
        const auto st = w->npcs()->stats();
        std::printf("    wander: %llu asks, %llu no point, %llu no path, %llu deferred; patrol: %llu asks, %llu no path, %llu deferred\n", (unsigned long long)st.wander_asks, (unsigned long long)st.wander_no_point,
                    (unsigned long long)st.wander_no_path, (unsigned long long)st.wander_deferred, (unsigned long long)st.patrol_asks, (unsigned long long)st.patrol_no_path, (unsigned long long)st.patrol_deferred);
    }
    std::printf("    of the %zu NPCs within 150 m that have a life of their own, %zu walked more than 1 m in the two minutes%s", lively.size(), walked.size(), chr_n());
    CHECK(lively.size() > 50 && walked.size() * 100 >= lively.size() * 90, "90 % of the NPCs near the player that wander or patrol walked within two minutes");
    CHECK(moved > 5, "NPCs near the player walked");
    CHECK(moved_far == 0, "NPCs far from the player did not");
    CHECK(total_us / static_cast<double>(steps) < 3000.0, "a step takes under 3 ms on average with the area's crowd");
    // the walkers stayed on the ground
    size_t off_ground = 0, checked = 0;
    w->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind != world::ActorKind::Npc || !w->npcs()->has(a.id)) return;
        const auto& s = start[a.id];
        if (std::hypot(a.x - s[0], a.y - s[1]) < 100.0) return;
        ++checked;
        double gz = 0;
        const double foot = w->npcs()->mover(a.id)->foot_offset();
        if (!w->nav()->ground_height(a.x, a.y, a.z - foot, &gz, 100, 100) || std::fabs(a.z - foot - gz) > 1.5) {
            ++off_ground;
            if (off_ground <= 6) {
                double tz2 = 0;
                const bool has_t = w->nav()->terrain_height(a.x, a.y, &tz2);
                std::printf("    off the ground: %s (%.0f, %.0f) origin %.1f foot %.1f ground %.1f (misses %zu), started at (%.0f, %.0f, %.1f), terrain %s%.1f\n", a.npc.name.c_str(), a.x, a.y, a.z, foot, gz,
                            w->npcs()->mover(a.id)->ground_misses(), s[0], s[1], s[2], has_t ? "" : "none ", has_t ? tz2 : 0.0);
            }
        }
    });
    CHECK(checked > 0 && off_ground * 20 <= checked, "(nearly) every NPC that walked stands on the ground where it stopped");
    // and the same world gives the same walks
    aoc::protocol::ServerGuidAllocator guids2(60, 0x6ee87ee4u);
    auto w2 = make_world(guids2);
    std::string e2;
    w2->load_ground(nav_path, motion_path, e2);
    double t2 = 100.0;
    for (int i = 0; i < 1800; ++i) {
        t2 += 1.0 / 15.0;
        w2->note_player(1, area.center_x, area.center_y);
        w2->tick_world(t2);
    }
    bool identical = true;
    w->registry().for_each([&](const world::WorldActor& a) {
        const world::WorldActor* b = w2->registry().find(a.id);
        identical = identical && b && b->x == a.x && b->y == a.y && b->z == a.z;
    });
    CHECK(identical, "a second world, built the same way and run the same, ends with every NPC where the first has it");
}

/// plan E1 stage 1, fix 3: the interaction gate's state.  Every GM vendor is allowed its four inputs (CanInputActivate), the other NPCs are not; the
/// per-player part, the player's CharacterGuid in the two state maps, is made on a copy (world/interaction_state.h) and never touches the registry's actor.
static void test_interaction_state(const content::ContentDb& db, const spawns::CapturedData& captured) {
    std::printf("the interaction gate: CanInputActivate and the per-player maps\n");
    std::string error;
    auto gm = vendor::GmVendors::load(db, error);
    if (!gm) {
        std::printf("  [FAIL] the GM vendors do not load: %s\n", error.c_str());
        g_fail++;
        return;
    }
    spawns::AreaSources sources;
    sources.db = &db;
    sources.captured = &captured;
    sources.gm = &*gm;
    spawns::AreaConfig area;
    area.radius_cm = 40000.0;
    world::ActorRegistry reg;
    const auto report = spawns::load_area(reg, sources, area, mint, 60, 0.0, error);
    CHECK(report && report->gm_vendors == 26 && report->refused == 0, "the area loads with its 26 GM vendors and nothing refused");
    if (!report) return;

    const auto can_input = [](const world::WorldActor& a) -> std::vector<bool> {
        std::vector<bool> out;
        for (const repl::Subobject& s : a.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent") continue;
            const repl::Value* v = s.obj.state.get(*s.obj.schema, "CanInputActivate");
            const auto* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
            if (!arr) continue;
            for (const auto& el : arr->elements) {
                const auto it = el.values.find(1);
                out.push_back(it != el.values.end() && it->second.get_if<bool>() && *it->second.get_if<bool>());
            }
        }
        return out;
    };
    const auto listed = [](const repl::Actor& a, const char* map) {
        std::vector<size_t> out;   // players listed per item
        for (const repl::Subobject& s : a.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent") continue;
            const repl::FastArrayState* arr = s.obj.arrays.find(s.obj.schema->field(map)->index);
            if (!arr) continue;
            for (const repl::FastArrayItem& it : arr->items()) {
                const auto v = it.values.values.find(2);
                const auto* w = v == it.values.values.end() ? nullptr : v->second.get_if<repl::GuidToBoolWrapper>();
                out.push_back(w ? w->pairs.size() : 0);
            }
        }
        return out;
    };
// [C_Y_B :: C O A]

    size_t vendors = 0, allowed = 0, defined = 0;
    size_t others = 0, others_interactable = 0, others_allowed = 0;
    reg.for_each([&](const world::WorldActor& a) {
        if (a.npc.is_gm_vendor) {
            ++vendors;
            allowed += can_input(a) == std::vector<bool>({true, true, true, true}) ? 1 : 0;
            defined += world::is_interactable(a.actor) ? 1 : 0;
        } else if (a.kind == world::ActorKind::Npc) {
            ++others;
            if (world::is_interactable(a.actor)) ++others_interactable;
            const auto c = can_input(a);
            others_allowed += std::find(c.begin(), c.end(), true) != c.end() ? 1 : 0;
        }
    });
    CHECK(vendors == 26 && allowed == 26 && defined == 26, "all 26 GM vendors have CanInputActivate true for the four inputs and an input with an interaction definition");
    CHECK(others_allowed == 0, "no other NPC is given CanInputActivate");
    std::printf("    %zu other NPCs, %zu with an interaction definition\n", others, others_interactable);

    // the per-player maps are made on a copy
    const world::WorldActor* vend = nullptr;
    reg.for_each([&](const world::WorldActor& a) {
        if (!vend && a.npc.is_gm_vendor) vend = &a;
    });
    if (!vend) return;
    const repl::GuidValue me{0xD8B2FE19u, 0x95FB4DF9u, 0x9B1E3958u, 0x6C2B2BD4u}, other{1, 2, 3, 4};
    const std::vector<int64_t> keys = world::interaction_inputs(vend->actor);
    CHECK(!keys.empty() && listed(vend->actor, "InteractVisibleStateMap").empty() && listed(vend->actor, "InteractGameplayStateMap").empty(),
          "a GM vendor has inputs with definitions and its registry maps are empty");
    repl::Actor mine = vend->actor;
    CHECK(world::set_player(mine, me, true), "the player is added to a copy");
    const auto v1 = listed(mine, "InteractVisibleStateMap"), g1 = listed(mine, "InteractGameplayStateMap");
    CHECK(v1.size() == keys.size() && g1 == v1 && v1.front() == 1, "...each input with a definition lists the player once in both maps");
    CHECK(listed(vend->actor, "InteractVisibleStateMap").empty() && listed(vend->actor, "InteractGameplayStateMap").empty(), "...and the registry's actor is as it was");
    repl::Actor again = mine;
    world::set_player(again, me, true);
    CHECK(listed(again, "InteractVisibleStateMap") == v1, "adding the same player twice lists it once");
    repl::Actor two = mine;
    world::set_player(two, other, true);
    CHECK(listed(two, "InteractVisibleStateMap").front() == 2, "a second player is listed beside the first");
    world::set_player(two, other, false);
    world::set_player(two, me, false);
    CHECK(listed(two, "InteractVisibleStateMap").front() == 0 && listed(two, "InteractGameplayStateMap").front() == 0, "taking both off leaves the lists empty");
    // a copy made twice from the same actor is the same state, item ids included (an update then sends nothing again)
    repl::Actor first = vend->actor, second = vend->actor;
    world::set_player(first, me, true);
    world::set_player(second, me, true);
    bool same_ids = true;
    for (size_t i = 0; i < first.subobjects.size(); ++i) {
        for (const auto& [field, st] : first.subobjects[i].obj.arrays.all()) {
            const repl::FastArrayState* o = second.subobjects[i].obj.arrays.find(field);
            same_ids = same_ids && o && o->items().size() == st.items().size();
            for (size_t k = 0; same_ids && k < st.items().size(); ++k) same_ids = st.items()[k].id == o->items()[k].id && st.items()[k].values == o->items()[k].values;
        }
    }
    CHECK(same_ids, "the same player on the same actor always gives the same state and item ids");
    // an actor with no interactable component is left alone
    repl::Actor plain;
    bool found_plain = false;
    reg.for_each([&](const world::WorldActor& a) {
        if (!found_plain && a.kind == world::ActorKind::Gatherable && !world::is_interactable(a.actor)) {
            plain = a.actor;
            found_plain = true;
        }
    });
    if (found_plain) CHECK(!world::set_player(plain, me, true) && !world::allow_inputs(plain), "an actor with no interaction definition is not changed");
}
// ~ CoA - C:Y:B ~

int main() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : AREA_CONTENT_DB_PATH;
    std::string error;
    auto db = content::ContentDb::open(path, error);
    if (!db) {
        std::printf("  [FAIL] content.db %s does not open: %s\n", path.c_str(), error.c_str());
        return 2;
    }
    std::printf("content.db %s (contentc version %s)\n", path.c_str(), db->meta().contentc_version.c_str());
    const auto t0 = std::chrono::steady_clock::now();
    auto captured = spawns::CapturedData::load(error);
    if (!captured) {
        std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
        return 2;
    }
    std::printf("the captured spawns parsed in %s\n", ms_since(t0).c_str());
    test_captured(*captured);
    test_instances(*captured);
    test_names_and_references(*captured);
    test_population(*db, *captured);
    test_load_area(*db, *captured);
    test_interaction_state(*db, *captured);
    test_gm_vendors(*db);
    test_mount_donors(*db);
    test_player_world(*db);
    test_ground(*db);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
