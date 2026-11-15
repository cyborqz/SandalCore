// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/gm_travel.cpp (see gm_travel.h)
// ============================================================================
#include "session/gm_travel.h"

#include "content/content_db.h"
#include "quest/quest_book.h"
#include "session/admin_commands.h"
#include "session/leveling.h"
#include "spawns/mount_spawn.h"
#include "world/world_clock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace aoc { namespace worldd {

namespace {

bool iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string coords(double x, double y) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "(%.0f, %.0f)", x, y);
    return buf;
}

}  // namespace

PlayerHandle GmTravel::find_player(const std::string& name) {
    if (name.empty()) return {};
    for (const PlayerHandle& h : directory_()) {
        if (h && iequal(h.player->name(), name)) return h;
    }
    return {};
}

std::vector<GmPlayerInfo> GmTravel::list_players() {
    std::vector<GmPlayerInfo> out;
    for (const PlayerHandle& h : directory_()) {
        if (!h) continue;
        const PlayerSession& p = *h.player;
        const GoodPosition g = p.last_position();
        GmPlayerInfo info;
        info.name = p.name();
        info.character_id = p.character_id();
        info.position = {g.x, g.y, g.z};
        info.yaw_degrees = g.yaw_degrees;
        info.mounted = p.mounted();
        info.dead = p.dead();
        info.move_speed_multiplier = p.gm_speed();
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<world::Place> GmTravel::list_places() { return world_.places().all(); }
// CoA with C_Y_B

GmTeleportResult GmTravel::teleport(const GmTeleportRequest& request, const std::string& by) {
    GmTeleportResult out;
    if (request.character.empty()) {
        out.status = GmStatus::InvalidArgument;
        out.message = "no character is named";
        return out;
    }
    const PlayerHandle handle = find_player(request.character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + request.character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    // where to: a place by name, or a point (with the ground's height when no z is given)
    double x = 0, y = 0, z = 0;
    if (!request.place.empty()) {
        const world::Places places = world_.places();
        const world::Place* place = places.find(request.place);
        if (!place) {
            out.status = GmStatus::NotFound;
            out.message = "no place named \"" + request.place + "\" (worldctl places lists them)";
            return out;
        }
        x = place->x;
        y = place->y;
        z = world_.place_height(*place);
        out.place = place->name;
    } else {
        x = request.x;
        y = request.y;
        if (!std::isfinite(request.x) || !std::isfinite(request.y) || (request.has_z && !std::isfinite(request.z))) {
            out.status = GmStatus::InvalidArgument;
            out.message = "the point is not a finite number";
            return out;
        }
        if (request.has_z) {
            z = request.z;
        } else if (const auto ground = world_.ground_at(x, y)) {
            z = *ground + PlayerWorld::kCapsuleHalfHeightCm;
            out.ground_height = true;
        } else {
            out.status = GmStatus::InvalidArgument;
            out.message = "the server has no ground at " + coords(x, y) + ": give z";
            return out;
        }
    }
    const PlayerSession::GmMoveResult moved = handle.player->gm_teleport(*handle.connection, x, y, z, now_(), by);
    out.character = handle.player->name();
    out.from = {moved.from.x, moved.from.y, moved.from.z};
    switch (moved.status) {
        case PlayerSession::GmMoveResult::Status::Moved:
            out.to = {moved.to.x, moved.to.y, moved.to.z};
            out.dismounted = moved.dismounted;
            break;
        case PlayerSession::GmMoveResult::Status::Dead:
            out.status = GmStatus::FailedPrecondition;
            out.message = out.character + " is dead: it comes back first";
            break;
        case PlayerSession::GmMoveResult::Status::OutsideWorld:
            out.status = GmStatus::InvalidArgument;
            out.message = "the point is outside the world";
            break;
        case PlayerSession::GmMoveResult::Status::Busy:
            out.status = GmStatus::FailedPrecondition;
            out.message = out.character + "'s connection is busy: try again in a moment";
            break;
    }
    return out;
}

GmSpeedResult GmTravel::set_speed(const std::string& character, double multiplier, const std::string& by) {
    GmSpeedResult out;
    if (!std::isfinite(multiplier) || multiplier < kMinSpeed || multiplier > kMaxSpeed) {
        out.status = GmStatus::InvalidArgument;
        out.message = "the multiplier must be 0.5 to 10 (1 resets)";
        return out;
    }
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    const PlayerSession::GmSpeedChange change = handle.player->gm_set_speed(*handle.connection, multiplier, now_(), by);
    out.character = handle.player->name();
    out.previous = change.previous;
    out.multiplier = change.now;
    return out;
}

GmMoneyGrant GmTravel::give_money(const std::string& character, int64_t copper, const std::string& by) {
    GmMoneyGrant out;
    if (copper < 1) {
        out.status = GmStatus::InvalidArgument;
        out.message = "the amount must be at least 1 copper (100 copper = 1 silver, 100 silver = 1 gold)";
        return out;
    }
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    const PlayerSession::GmMoneyResult r = handle.player->gm_give_money(*handle.connection, copper, now_(), by);
    out.character = handle.player->name();
    out.previous = r.previous;
    out.balance = r.balance;
    out.added = r.added;
    if (!r.ok) {
        out.status = GmStatus::FailedPrecondition;
        out.message = r.refusal;
    }
    return out;
}

namespace {

GmStatus gm_status_of(PlayerWorld::StationStatus s) {
    switch (s) {
        case PlayerWorld::StationStatus::Ok: return GmStatus::Ok;
        case PlayerWorld::StationStatus::NotFound: return GmStatus::NotFound;
        case PlayerWorld::StationStatus::Invalid: return GmStatus::InvalidArgument;
        case PlayerWorld::StationStatus::Refused: return GmStatus::FailedPrecondition;
    }
    return GmStatus::FailedPrecondition;
}

GmStationResult station_result_of(const PlayerWorld::StationResult& r) {
    GmStationResult out;
    out.status = gm_status_of(r.status);
    out.message = r.message;
    out.actor = r.actor;
    out.node = r.node;
    out.plot = r.plot;
    out.station = r.station;
    out.building = r.building;
    out.position = {r.x, r.y, r.z};
    return out;
}

}  // namespace

GmStationResult GmTravel::place_station(const std::string& node, const std::string& plot, const std::string& station, const std::string& by) {
    return station_result_of(world_.place_station(log_text(node), log_text(plot), log_text(station), by));
}

GmStationResult GmTravel::remove_station(const std::string& node, const std::string& plot, const std::string& by) {
    return station_result_of(world_.remove_station(log_text(node), log_text(plot), by));
}

GmFinishResult GmTravel::finish_jobs(const std::string& character, const std::string& station, const std::string& by) {
    GmFinishResult out;
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    out.character = handle.player->name();
    out.finished = static_cast<uint32_t>(handle.player->gm_finish_jobs(*handle.connection, now_(), log_text(station), by));
    if (out.finished == 0) {
        out.status = GmStatus::FailedPrecondition;
        out.message = out.character + " has no job that is not over" + (station.empty() ? std::string() : " at " + log_text(station));
    }
    return out;
}

namespace {

/// The recipe a GM names: its record name (case does not count) or its Guid in digits.  nullptr, with `out` filled, when there is none or several.
const content::CraftRecipe* recipe_named(const content::CraftRecipeData* data, const std::string& text, GmRecipeResult& out) {
    if (!data) {
        out.status = GmStatus::FailedPrecondition;
        out.message = "worldd has no recipes (an older content.db)";
        return nullptr;
    }
    if (text.empty()) {
        out.status = GmStatus::InvalidArgument;
        out.message = "no recipe is named";
        return nullptr;
    }
    const bool digits = std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (digits) {
        if (const content::CraftRecipe* r = data->recipe(std::atoll(text.c_str()))) return r;
    }
    const content::CraftRecipe* found = nullptr;
    size_t count = 0;
    for (const auto& [guid, r] : data->recipes) {
        if (r.name.size() == text.size() && std::equal(r.name.begin(), r.name.end(), text.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
            found = &r;
            ++count;
        }
    }
    if (count == 1) return found;
    out.status = count == 0 ? GmStatus::NotFound : GmStatus::InvalidArgument;
    out.message = count == 0 ? "no recipe is named \"" + log_text(text) + "\"" : std::to_string(count) + " recipes are named \"" + log_text(text) + "\": give the Guid";
    return nullptr;
}

}  // namespace

GmRecipeResult GmTravel::teach_recipe(const std::string& character, const std::string& recipe, const std::string& by) {
    GmRecipeResult out;
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    out.character = handle.player->name();
    const content::CraftRecipe* r = recipe_named(world_.craft_recipes(), recipe, out);
    if (!r) return out;
    out.recipe = r->name;
    out.recipe_guid = r->guid;
    if (!r->learnable) {
        out.status = GmStatus::FailedPrecondition;
        out.message = r->name + " needs no teaching: every character knows it";
        return out;
    }
    if (!handle.player->gm_teach_recipe(*handle.connection, r->guid, by)) {
        out.status = GmStatus::FailedPrecondition;
        out.message = out.character + " was taught " + r->name + " already";
    }
    out.taught = static_cast<int32_t>(handle.player->taught_recipes().size());
    return out;
}

GmRecipeResult GmTravel::forget_recipe(const std::string& character, const std::string& recipe, const std::string& by) {
    GmRecipeResult out;
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    out.character = handle.player->name();
    const content::CraftRecipe* r = recipe_named(world_.craft_recipes(), recipe, out);
    if (!r) return out;
    out.recipe = r->name;
    out.recipe_guid = r->guid;
    if (!handle.player->gm_forget_recipe(*handle.connection, r->guid, by)) {
        out.status = GmStatus::FailedPrecondition;
        out.message = out.character + " was not taught " + r->name;
    }
    out.taught = static_cast<int32_t>(handle.player->taught_recipes().size());
    return out;
}

GmTutorialResult GmTravel::reset_tutorials(const std::string& character, const std::string& by) {
    GmTutorialResult out;
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    out.character = handle.player->name();
    out.cleared = static_cast<uint32_t>(handle.player->gm_reset_tutorials(*handle.connection, by));
    if (out.cleared == 0) {
        out.status = GmStatus::FailedPrecondition;
        out.message = out.character + " has finished no tutorial";
    }
    return out;
}

GmWeaponXpResult GmTravel::set_weapon_xp(const std::string& character, const std::string& line, bool by_level, int64_t amount, const std::string& by) {
    GmWeaponXpResult out;
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    out.character = handle.player->name();
    const weaponxp::Lines* lines = world_.weapon_lines();
    if (!lines) {
        out.status = GmStatus::FailedPrecondition;
        out.message = "this world has no weapon lines (its content.db lacks their leveling types)";
        return out;
    }
    const std::vector<const weaponxp::Line*> found = lines->match(line);
    if (found.empty()) {
        out.status = GmStatus::NotFound;
        out.message = "no weapon line is called \"" + log_text(line) + "\"; the lines are:";
        for (const weaponxp::Line& l : lines->all()) out.message += " " + l.name;
        return out;
    }
    if (found.size() > 1) {
        out.status = GmStatus::InvalidArgument;
        out.message = "\"" + log_text(line) + "\" fits more than one weapon line; give the whole name:";
        for (const weaponxp::Line* l : found) out.message += " " + l->name;
        return out;
    }
    int64_t xp = amount;
    if (by_level) {
        if (amount < 1 || amount > lines->max_level()) {
            out.status = GmStatus::InvalidArgument;
            out.message = "a weapon line's level is 1 to " + std::to_string(lines->max_level()) + " (the XP total that reaches level " + std::to_string(lines->max_level()) + " is " +
                          std::to_string(lines->max_xp()) + ")";
            return out;
        }
        xp = lines->xp_for(static_cast<int>(amount));
    } else if (amount < 0 || amount > lines->max_xp()) {
        out.status = GmStatus::InvalidArgument;
        out.message = "a weapon line's XP is 0 to " + std::to_string(lines->max_xp()) + " (the total that reaches level " + std::to_string(lines->max_level()) + ")";
        return out;
    }
    const PlayerSession::GmWeaponResult r = handle.player->gm_set_weapon_xp(*handle.connection, found.front()->leveling_type, xp, now_(), by);
    out.line = r.line.empty() ? found.front()->name : r.line;
    out.xp_before = r.xp_before;
    out.xp = r.xp_after;
    out.level_before = r.level_before;
    out.level = r.level_after;
    out.points_earned = r.points_earned;
    out.points_left = r.points_left;
    if (!r.ok) {
        out.status = GmStatus::FailedPrecondition;
        out.message = r.refusal;
    }
    return out;
}

GmTaughtList GmTravel::list_taught(const std::string& character) {
    GmTaughtList out;
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    out.character = handle.player->name();
    const content::CraftRecipeData* data = world_.craft_recipes();
    for (const int64_t guid : handle.player->taught_recipes()) {
        const content::CraftRecipe* r = data ? data->recipe(guid) : nullptr;
        out.recipes.emplace_back(r ? r->name : std::to_string(guid), guid);
    }
    return out;
}

GmItemGrant GmTravel::give_item(const std::string& character, const std::string& item, int32_t count, const std::string& by, const std::string& rarity, int32_t quality) {
    GmItemGrant out;
    if (item.empty()) {
        out.status = GmStatus::InvalidArgument;
        out.message = "no item is named";
        return out;
    }
    if (count < 1 || count > 100000) {
        out.status = GmStatus::InvalidArgument;
        out.message = "the count must be 1 to 100,000";
        return out;
    }
    const PlayerHandle handle = find_player(character);
    if (!handle) {
        out.status = GmStatus::NotFound;
        out.message = "the character \"" + character + "\" is not in the world (worldctl players lists them)";
        return out;
    }
    const content::ContentDb* db = world_.content();
    if (!db) {
        out.status = GmStatus::FailedPrecondition;
        out.message = "worldd has no content.db to look the item up in";
        return out;
    }
    // an item is named by its record name (case does not count) or by its Guid
    int64_t guid = 0;
    const bool digits = std::all_of(item.begin(), item.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (digits && item.size() <= 19) {
        guid = std::strtoll(item.c_str(), nullptr, 10);
    } else {
        std::string error;
        const std::vector<int64_t> found = db->items_named(item, error);
        if (!error.empty()) {
            out.status = GmStatus::FailedPrecondition;
            out.message = error;
            return out;
        }
        if (found.empty()) {
            out.status = GmStatus::NotFound;
            out.message = "no item is named \"" + item + "\" (the name is the record's, such as Consumable_Food_Regen_Rations_T1, or its Guid)";
            return out;
        }
        if (found.size() > 1) {
            out.status = GmStatus::InvalidArgument;
            out.message = "\"" + item + "\" names " + std::to_string(found.size()) + " items: give the Guid of one of them (first: " + std::to_string(found.front()) + ")";
            return out;
        }
        guid = found.front();
    }
    const PlayerSession::GmItemResult r = handle.player->gm_give_item(*handle.connection, guid, count, now_(), by, rarity, quality);
    out.character = handle.player->name();
    out.item = r.item_name;
    out.item_guid = guid;
    out.count = r.count;
    out.container = r.container;
    out.quality = r.quality;
    out.rarity = r.rarity;
    if (!r.ok) {
        out.status = r.item_name.empty() ? GmStatus::NotFound : (r.bad_request ? GmStatus::InvalidArgument : GmStatus::FailedPrecondition);
        out.message = r.refusal;
    }
    return out;
}

// ---- the NPC actions ------------------------------------------------------------------------------------------------------------------------

namespace {

std::string metres_at(double x, double y) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.0f, %.0f m)", x / 100.0, y / 100.0);
    return buf;
}

std::string count_of(uint64_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

std::string seconds_of(double s) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.0f s", s);
    return buf;
}

/// What a creature is called: its nameplate, else its Blueprint's name.
std::string npc_name_of(const world::WorldActor& a) {
    if (!a.npc.name.empty()) return a.npc.name;
    std::string leaf = spawns::blueprint_leaf(a.npc.archetype);
    constexpr char kDefault[] = "Default__";   // a class default object's name: "Default__AlphaWolf" is an AlphaWolf
    if (leaf.compare(0, sizeof kDefault - 1, kDefault) == 0) leaf.erase(0, sizeof kDefault - 1);
    return leaf.empty() ? std::string("creature") : leaf;
}

const char* kind_word(world::ActorKind k) {
    switch (k) {
        case world::ActorKind::Npc: return "creature";
        case world::ActorKind::Gatherable: return "resource node";
        case world::ActorKind::Effect: return "lingering effect";
        case world::ActorKind::Pile: return "loot pile";
        case world::ActorKind::Mount: return "mount";
        case world::ActorKind::NodeLayout: return "town's scenery";
        case world::ActorKind::Foliage: return "foliage cell";
        case world::ActorKind::Station: return "crafting station";
        case world::ActorKind::Shrine: return "Emberspring";
        case world::ActorKind::Prop: return "quest object";
    }
    return "actor";
}

/// The creature the call names; null, with the answer's status and message set, when it names none.
world::WorldActor* npc_named(PlayerWorld& world, const std::vector<PlayerHandle>& players, uint64_t id, GmNpcResult& out) {
    out.id = id;
    world::WorldActor* a = world.registry().find(id);
    if (!a) {
        for (const PlayerHandle& h : players) {
            if (h && h.player->bootstrap().setup().guids.pawn.ObjectId == id) {
                out.status = GmStatus::InvalidArgument;
                out.message = "actor " + std::to_string(id) + " is the player " + h.player->name() + ", not a creature";
                return nullptr;
            }
        }
        out.status = GmStatus::NotFound;
        out.message = "no creature with the actor id " + std::to_string(id) + " is in the world (the world stream and Inspect give the ids; a creature that came back has a new one)";
        return nullptr;
    }
    if (a->kind != world::ActorKind::Npc) {
        out.status = GmStatus::InvalidArgument;
        out.message = "actor " + std::to_string(id) + " is a " + kind_word(a->kind) + ", not a creature";
        return nullptr;
    }
    out.name = npc_name_of(*a);
    out.position = {a->x, a->y, a->z};
    return a;
}

/// Whether the creature is dead: a fighter at 0 Health, or a corpse the aftermath holds (a hunted huntable's, too).
bool npc_is_dead(PlayerWorld& world, uint64_t id) {
    if (world.combat() && world.combat()->dead(id)) return true;
    const combat::Aftermath* after = world.aftermath();
    const combat::Corpse* c = after ? after->corpse_of(id) : nullptr;
    return c && !c->living && !c->is_pile;
}

GmNpcResult refused(GmNpcResult out, GmStatus status, std::string message) {
    out.status = status;
    out.message = std::move(message);
    return out;
}

}  // namespace

std::string gm_creature_name(const world::WorldActor& a) { return npc_name_of(a); }

bool gm_creature_dead(PlayerWorld& world, uint64_t id) { return npc_is_dead(world, id); }

GmNpcResult GmTravel::kill_npc(uint64_t id, const std::string& by) {
    GmNpcResult out;
    const std::vector<PlayerHandle> players = directory_();
    world::WorldActor* a = npc_named(world_, players, id, out);
    if (!a) return out;
    combat::CombatWorld* combat = world_.combat();
    if (!combat) return refused(out, GmStatus::FailedPrecondition, "the fights are switched off (--no-combat): nothing can be killed");
    if (npc_is_dead(world_, id)) return refused(out, GmStatus::FailedPrecondition, out.name + " is dead already");
    const double now = world_.world_clock();
    std::string why;
    if (!combat->gm_kill(id, now, why)) return refused(out, GmStatus::InvalidArgument, out.name + ": " + why);
    combat::Aftermath* after = world_.aftermath();
    const combat::Corpse* corpse = after ? after->corpse_of(id) : nullptr;
    out.loot_stacks = corpse ? static_cast<uint32_t>(corpse->loot.size()) : 0;
    out.respawn_in_seconds = after ? after->respawn_in(id, now) : 0.0;
    std::string m = out.name + " was killed";
    if (out.loot_stacks > 0) {
        m += ": its corpse holds " + count_of(out.loot_stacks, "stack", "stacks") + " of loot" +
             (corpse->parts ? ", open to every player in the world" : ", and nobody is in the world to take it");
    } else {
        m += ": it dropped nothing";
    }
    m += out.respawn_in_seconds > 0 ? "; its spawn point makes it again in " + seconds_of(out.respawn_in_seconds) : std::string("; its spawn point does not make it again");
    out.message = m;
    spdlog::info("[npc] {}: killed {} (actor {}) at {}: {}", by, log_text(out.name), id, metres_at(a->x, a->y), log_text(out.message));
    return out;
}

GmNpcResult GmTravel::respawn_npc(uint64_t id, const std::string& by) {
    GmNpcResult out;
    const std::vector<PlayerHandle> players = directory_();
    combat::Aftermath* after = world_.aftermath();
    const double now = world_.world_clock();
    world::WorldActor* a = world_.registry().find(id);
    uint64_t made = 0;
    std::string how;
    if (!a) {
        // a dead creature whose corpse has gone: only its timer is left, and the timer still knows the id
        made = after ? after->respawn_now(id, now) : 0;
        if (made == 0) {
            npc_named(world_, players, id, out);
            return out;
        }
        how = " was made again at its spawn point now, without waiting for its timer";
        out.name = "the creature";
    } else {
        if (!npc_named(world_, players, id, out)) return out;
        if (npc_is_dead(world_, id)) {
            made = after ? after->respawn_now(id, now) : 0;
            if (made == 0) {
                if (after && after->refills(a->npc)) return refused(out, GmStatus::FailedPrecondition, out.name + " was made again already (its timer ran out, or a respawn was asked for): look for the new one");
                return refused(out, GmStatus::FailedPrecondition, out.name + "'s spawn point does not make it again (its respawn rule is NoRespawn, or it has no spawn point)");
            }
            how = " was made again at its spawn point now, without waiting for its timer";
        } else {
            const uint32_t ref = a->npc.spawn_ref;
            if (ref == 0) return refused(out, GmStatus::FailedPrecondition, out.name + " has no spawn point to be made again at (it was placed, not spawned)");
            if (spawns::NodePopulations::is_node_ref(ref)) return refused(out, GmStatus::FailedPrecondition, out.name + " belongs to a town's people: despawn or kill it first, and its node makes it again");
            made = world_.respawn_npc(ref, now);   // the fresh one first: when the spawn point cannot make it, the old one stays
            if (made == 0) return refused(out, GmStatus::FailedPrecondition, out.name + "'s spawn point could not make a new one");
            if (after) after->despawn(id, now, false, nullptr);
            else world_.registry().remove(id);
            how = " was replaced by a fresh one at its spawn point";
        }
    }
    if (const world::WorldActor* fresh = world_.registry().find(made)) {
        out.name = npc_name_of(*fresh);
        out.position = {fresh->x, fresh->y, fresh->z};
    }
    out.id = made;
    out.message = out.name + how + " (actor " + std::to_string(made) + ")";
    spdlog::info("[npc] {}: respawned {} (was actor {}, now actor {}) at {}", by, log_text(out.name), id, made, metres_at(out.position.x, out.position.y));
    return out;
}

GmNpcResult GmTravel::reset_npc(uint64_t id, const std::string& by) {
    GmNpcResult out;
    const std::vector<PlayerHandle> players = directory_();
    world::WorldActor* a = npc_named(world_, players, id, out);
    if (!a) return out;
    if (npc_is_dead(world_, id)) return refused(out, GmStatus::FailedPrecondition, out.name + " is dead: respawn it instead");
    npc::NpcWorld* npcs = world_.npcs();
    nav::Vec3 ground_home, ai_home;
    if (!npcs || !npcs->spawn_point(id, &ground_home, &ai_home)) return refused(out, GmStatus::FailedPrecondition, out.name + " has no walking life in the world to reset (worldd has no ground for it)");
    const double now = world_.world_clock();
    // its leash centre is its spawn point again
    a->npc.home_x = ai_home.x;
    a->npc.home_y = ai_home.y;
    a->npc.home_z = ai_home.z;
    npcs->restore_home(id, nullptr);
    std::string how;
    const combat::CombatWorld::GmReset fight = world_.combat() ? world_.combat()->gm_reset(id, now) : combat::CombatWorld::GmReset::Standing;
    if (fight == combat::CombatWorld::GmReset::Walking) {
        how = "runs back to its spawn point";
    } else if (std::hypot(a->x - ground_home.x, a->y - ground_home.y) <= 150.0) {
        how = "is at its spawn point";
    } else if (npcs->order_return_home(id)) {
        how = "runs back to its spawn point";
    } else {
        npcs->relocate(id, ground_home.x, ground_home.y, ground_home.z, false);   // no way home on foot: it is put there
        how = "was put back at its spawn point (there is no way home on foot)";
    }
    out.position = {a->x, a->y, a->z};
    out.message = out.name + " " + how + " " + metres_at(ground_home.x, ground_home.y) + ", at full health and with its threat cleared; its leash is centred there again";
    spdlog::info("[npc] {}: reset {} (actor {}): {}", by, log_text(out.name), id, log_text(out.message));
    return out;
}

GmNpcResult GmTravel::heal_npc(uint64_t id, const std::string& by) {
    GmNpcResult out;
    const std::vector<PlayerHandle> players = directory_();
    world::WorldActor* a = npc_named(world_, players, id, out);
    if (!a) return out;
    if (npc_is_dead(world_, id)) return refused(out, GmStatus::FailedPrecondition, out.name + " is dead: respawn it instead");
    combat::CombatWorld* combat = world_.combat();
    const double before = combat ? combat->health_of(id) : -1.0;
    const double max = combat ? combat->max_health_of(id) : -1.0;
    if (combat) combat->gm_heal(id);
    if (before < 0 || max <= 0 || before >= max - 0.5) {
        out.message = out.name + " was at full health already";
    } else {
        char buf[96];
        std::snprintf(buf, sizeof buf, " from %.0f to %.0f health", before, max);
        out.message = out.name + " was healed" + buf;
    }
    spdlog::info("[npc] {}: healed {} (actor {}): {}", by, log_text(out.name), id, log_text(out.message));
    return out;
}

GmNpcResult GmTravel::despawn_npc(uint64_t id, const std::string& by) {
    GmNpcResult out;
    const std::vector<PlayerHandle> players = directory_();
    world::WorldActor* a = npc_named(world_, players, id, out);
    if (!a) return out;
    const world::NpcInfo info = a->npc;
    combat::Aftermath* after = world_.aftermath();
    double secs = 0;
    if (after) after->despawn(id, world_.world_clock(), true, &secs);
    else world_.registry().remove(id);
    out.respawn_in_seconds = secs;
    std::string m = out.name + " was taken out of the world";
    if (secs > 0) m += "; its spawn point makes it again in " + seconds_of(secs);
    else if (info.spawn_ref == 0) m += "; it has no spawn point, so it stays gone until worldd restarts";
    else m += "; its spawn point does not make it again (its respawn rule is " + (info.respawn_rule.empty() ? std::string("off") : info.respawn_rule) + ")";
    out.message = m;
    spdlog::info("[npc] {}: despawned {} (actor {}) at {}: {}", by, log_text(out.name), id, metres_at(out.position.x, out.position.y), log_text(out.message));
    return out;
}

GmNpcResult GmTravel::move_npc(uint64_t id, double x, double y, double z, bool has_z, const std::string& by) {
    GmNpcResult out;
    const std::vector<PlayerHandle> players = directory_();
    world::WorldActor* a = npc_named(world_, players, id, out);
    if (!a) return out;
    if (npc_is_dead(world_, id)) return refused(out, GmStatus::FailedPrecondition, out.name + " is dead: respawn it instead");
    if (!std::isfinite(x) || !std::isfinite(y) || (has_z && !std::isfinite(z))) return refused(out, GmStatus::InvalidArgument, "the point is not a finite number");
    npc::NpcWorld* npcs = world_.npcs();
    if (!npcs || !npcs->has(id)) return refused(out, GmStatus::FailedPrecondition, out.name + " has no walking life in the world to move (worldd has no ground for it)");
    double use_z = z;
    if (!has_z) {
        if (const auto town = world_.town_ground_at(x, y)) use_z = town->z;   // inside a town's bowl the floor is the town's, not the pit's
        else if (const auto ground = world_.ground_at(x, y)) use_z = *ground;
        else return refused(out, GmStatus::InvalidArgument, "the server has no ground at " + coords(x, y) + ": give z");
    }
    out.from = {a->x, a->y, a->z};
    const npc::NpcWorld::RelocateResult r = npcs->relocate(id, x, y, use_z, has_z);
    switch (r.status) {
        case npc::NpcWorld::Relocated::Ok: break;
        case npc::NpcWorld::Relocated::UnknownNpc: return refused(out, GmStatus::NotFound, out.name + " is not held by the NPC world");
        case npc::NpcWorld::Relocated::WaitingForGround: return refused(out, GmStatus::FailedPrecondition, out.name + " still waits for its ground to load: try again in a moment");
        case npc::NpcWorld::Relocated::NotLoaded: return refused(out, GmStatus::FailedPrecondition, "the ground at " + coords(x, y) + " is not loaded (it loads around the players): move the creature near one");
        case npc::NpcWorld::Relocated::NotANumber: return refused(out, GmStatus::InvalidArgument, "the point is not a finite number");
    }
    // its leash is centred on the new place from now on (the AI reads the actor's home); its spawn point, where it comes back after a death, is the spawn point's and stays
    a->npc.home_x = x;
    a->npc.home_y = y;
    a->npc.home_z = r.origin_z;
    out.position = {a->x, a->y, a->z};
    nav::Vec3 spawn, ignored;
    npcs->spawn_point(id, &spawn, &ignored);
    out.message = out.name + " was moved from " + metres_at(out.from.x, out.from.y) + " to " + metres_at(a->x, a->y) + ": it stands there and its leash is centred there now; its spawn point stays " +
                  metres_at(spawn.x, spawn.y);
    spdlog::info("[npc] {}: moved {} (actor {}): {}", by, log_text(out.name), id, log_text(out.message));
    return out;
}

namespace {

/// The fields of a run, one line for the log: every value cut short and with control characters shown as '?' (a newline would start a log line of the caller's making).
std::string fields_for_log(const std::map<std::string, std::string>& fields) {
    std::string out;
    for (const auto& [name, value] : fields) {
        if (!out.empty()) out += " ";
        out += log_text(name) + "=\"" + log_text(value.size() > 80 ? value.substr(0, 80) + "..." : value) + "\"";
    }
    return out.empty() ? std::string("(none)") : out;
}

std::string coords3(double x, double y, double z) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.0f, %.0f, %.0f)", x, y, z);
    return buf;
}

std::string num_text(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

}  // namespace

GmRunResult GmTravel::run_command(const std::string& command, const std::map<std::string, std::string>& fields, const std::string& by) {
    using admin::FieldValue;
    GmRunResult out;
    out.command = command;
    const admin::AdminCommand* def = admin::find_command(command);
    if (!def) {
        out.status = GmStatus::InvalidArgument;
        out.message = "no command is named \"" + log_text(command) + "\" (ListCommands lists them)";
        spdlog::info("[admin] run {} by {}: fields={} -> refused: {}", log_text(command), log_text(by), fields_for_log(fields), log_text(out.message));
        return out;
    }
    admin::FieldValues values;
    std::string why;
    if (!admin::check_command_fields(*def, fields, &values, &why)) {
        out.status = GmStatus::InvalidArgument;
        out.message = why;
        spdlog::info("[admin] run {} by {}: fields={} -> refused: {}", log_text(def->name), log_text(by), fields_for_log(fields), log_text(out.message));
        return out;
    }
    const auto value = [&values](const char* name) -> const FieldValue* { return admin::field_value(values, name); };
    const auto text = [&](const char* name) -> std::string {
        const FieldValue* v = value(name);
        return v ? v->text : std::string();
    };
    const auto number = [&](const char* name, double fallback = 0.0) -> double {
        const FieldValue* v = value(name);
        return v ? v->number : fallback;
    };
    const auto whole = [&](const char* name, int64_t fallback = 0) -> int64_t {
        const FieldValue* v = value(name);
        return v ? v->whole : fallback;
    };
    PlayerHandle handle;
    if (def->acts_on_character) {
        handle = find_player(text("character"));
        if (!handle) {
            out.status = GmStatus::NotFound;
            out.message = "the character \"" + text("character") + "\" is not in the world";
            // (the log line below still says which command and fields were refused)
            spdlog::info("[admin] run {} by {}: character=\"{}\" fields={} -> refused: {}", log_text(def->name), log_text(by), log_text(text("character")),
                         fields_for_log(fields), log_text(out.message));
            return out;
        }
        out.character = handle.player->name();
    }

    const auto failed = [&](GmStatus status, std::string message) {
        out.status = status;
        out.message = std::move(message);
        spdlog::info("[admin] run {} by {}: character=\"{}\" fields={} -> refused: {}", log_text(def->name), log_text(by), log_text(out.character), fields_for_log(fields),
                     log_text(out.message));
        return out;
    };

    if (def->name == "list-players") {
        const std::vector<GmPlayerInfo> players = list_players();
        if (players.empty()) {
            out.message = "no players are in the world";
        } else {
            out.message = std::to_string(players.size()) + " player(s) in the world:";
            for (size_t i = 0; i < players.size() && i < 8; ++i) {
                out.message += " " + players[i].name + " " + coords3(players[i].position.x, players[i].position.y, players[i].position.z);
            }
            if (players.size() > 8) out.message += " and " + std::to_string(players.size() - 8) + " more";
        }
    } else if (def->name == "list-places") {
        const std::vector<world::Place> places = list_places();
        out.message = std::to_string(places.size()) + " place(s):";
        for (size_t i = 0; i < places.size() && i < 10; ++i) out.message += " " + places[i].display_name;
        if (places.size() > 10) out.message += " and " + std::to_string(places.size() - 10) + " more";
    } else if (def->name == "teleport") {
        GmTeleportRequest rq;
        rq.character = text("character");
        const bool has_place = value("place") != nullptr;
        const bool has_x = value("x") != nullptr, has_y = value("y") != nullptr, has_z = value("z") != nullptr;
        if (has_place && (has_x || has_y || has_z)) return failed(GmStatus::InvalidArgument, "give a place or a point, not both");
        if (!has_place && (!has_x || !has_y)) return failed(GmStatus::InvalidArgument, "give a place, or x and y for a point");
        if (has_z && !has_x) return failed(GmStatus::InvalidArgument, "z needs x and y");
        if (has_place) {
            rq.place = text("place");
        } else {
            rq.x = number("x");
            rq.y = number("y");
            rq.z = number("z");
            rq.has_z = has_z;
        }
        const GmTeleportResult r = teleport(rq, by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + " moved from " + coords3(r.from.x, r.from.y, r.from.z) + " to " + coords3(r.to.x, r.to.y, r.to.z) +
                      (r.place.empty() ? std::string() : " (" + r.place + ")") + (r.dismounted ? "; dismounted first" : "");
    } else if (def->name == "speed") {
        const GmSpeedResult r = set_speed(text("character"), number("multiplier"), by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + (r.multiplier == 1.0 ? " runs at normal speed again (was x" + num_text(r.previous) + ")"
                                                       : " runs at x" + num_text(r.multiplier) + " (was x" + num_text(r.previous) + ")");
    } else if (def->name == "give-money") {
        const GmMoneyGrant r = give_money(text("character"), whole("copper"), by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + ": +" + std::to_string(r.added) + " copper, the purse holds " + std::to_string(r.balance) + " (was " + std::to_string(r.previous) + ")" +
                      (r.added < whole("copper") ? "; the purse is full" : "");
    } else if (def->name == "give-item") {
        const GmItemGrant r = give_item(text("character"), text("item"), static_cast<int32_t>(whole("count", 1)), by, text("rarity"), static_cast<int32_t>(whole("quality")));
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + ": " + std::to_string(r.count) + " x " + r.item + " (" + std::to_string(r.item_guid) + ") given" +
                      (r.rarity.empty() ? std::string() : " as " + r.rarity + " (quality " + std::to_string(r.quality) + ")") + ", into " + r.container;
    } else if (def->name == "place-station") {
        const GmStationResult r = place_station(text("node"), text("plot"), text("station"), by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.node + " " + r.plot + ": " + r.station + " of " + r.building + " placed (actor " + std::to_string(r.actor) + ")";
    } else if (def->name == "remove-station") {
        const GmStationResult r = remove_station(text("node"), text("plot"), by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.node + " " + r.plot + ": " + r.station + " removed";
    } else if (def->name == "finish-jobs") {
        const GmFinishResult r = finish_jobs(text("character"), text("station"), by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + ": " + std::to_string(r.finished) + " job(s) finished" + (text("station").empty() ? "" : " at " + text("station")) + "; they are ready to collect";
    } else if (def->name == "teach-recipe") {
        const GmRecipeResult r = teach_recipe(text("character"), text("recipe"), by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + " was taught " + r.recipe + " (" + std::to_string(r.recipe_guid) + "); " + std::to_string(r.taught) + " recipe(s) taught in all";
    } else if (def->name == "forget-recipe") {
        const GmRecipeResult r = forget_recipe(text("character"), text("recipe"), by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + " forgot " + r.recipe + " (" + std::to_string(r.recipe_guid) + "); " + std::to_string(r.taught) + " recipe(s) taught in all";
    } else if (def->name == "list-taught-recipes") {
        const GmTaughtList r = list_taught(text("character"));
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + " was taught " + std::to_string(r.recipes.size()) + " recipe(s)";
        for (const auto& [name, guid] : r.recipes) out.message += "; " + name + " (" + std::to_string(guid) + ")";
    } else if (def->name == "reset-tutorials") {
        const GmTutorialResult r = reset_tutorials(text("character"), by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + ": " + std::to_string(r.cleared) + " finished tutorial(s) put back; the hints can start again";
    } else if (def->name == "weapon-xp") {
        const bool by_level = value("level") != nullptr;
        const bool by_xp = value("xp") != nullptr;
        if (by_level == by_xp) return failed(GmStatus::InvalidArgument, "give a level or an XP total, not both");
        const GmWeaponXpResult r = set_weapon_xp(text("character"), text("line"), by_level, by_level ? whole("level") : whole("xp"), by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.character + ": " + r.line + " is at level " + std::to_string(r.level) + " with " + std::to_string(r.xp) + " XP (was level " + std::to_string(r.level_before) +
                      ", " + std::to_string(r.xp_before) + " XP); " + std::to_string(r.points_earned) + " Weapon point(s) earned, " + std::to_string(r.points_left) + " left";
    } else if (def->name == "npc-kill" || def->name == "npc-respawn" || def->name == "npc-reset" || def->name == "npc-heal" || def->name == "npc-despawn" || def->name == "npc-move") {
        GmNpcResult r;
        const uint64_t id = static_cast<uint64_t>(whole("id"));
        if (def->name == "npc-kill") r = kill_npc(id, by);
        else if (def->name == "npc-respawn") r = respawn_npc(id, by);
        else if (def->name == "npc-reset") r = reset_npc(id, by);
        else if (def->name == "npc-heal") r = heal_npc(id, by);
        else if (def->name == "npc-despawn") r = despawn_npc(id, by);
        else r = move_npc(id, number("x"), number("y"), number("z"), value("z") != nullptr, by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = r.message;
    } else if (def->name == "heal") {
        const PlayerSession::GmHealResult r = handle.player->gm_heal(*handle.connection, now_(), by);
        if (!r.ok) return failed(GmStatus::FailedPrecondition, r.refusal);
        out.message = out.character + " healed to full Health and Mana (" + num_text(r.health.after) + " Health, " + num_text(r.mana.after) + " Mana)";
    } else if (def->name == "set-health" || def->name == "set-mana") {
        const bool health = def->name == "set-health";
        const PlayerSession::GmValueResult r = health ? handle.player->gm_set_health(*handle.connection, number("value"), now_(), by)
                                                      : handle.player->gm_set_mana(*handle.connection, number("value"), now_(), by);
        if (!r.ok) return failed(GmStatus::FailedPrecondition, r.refusal);
        out.message = out.character + ": " + (health ? "Health" : "Mana") + " set to " + num_text(r.after) + " of " + num_text(r.maximum) + " (was " + num_text(r.before) + ")" +
                      (health && handle.player->dead() ? "; it died" : "");
    } else if (def->name == "revive") {
        const PlayerSession::GmReviveResult r = handle.player->gm_revive(*handle.connection, now_(), by);
        if (!r.ok) return failed(GmStatus::FailedPrecondition, r.refusal);
        out.message = out.character + " came back at " + r.place + " " + coords3(r.x, r.y, r.z) + ", alive with full Health and Mana";
    } else if (def->name == "kill") {
        const PlayerSession::GmKillResult r = handle.player->gm_kill(*handle.connection, now_(), by);
        if (!r.ok) return failed(GmStatus::FailedPrecondition, r.refusal);
        out.message = out.character + " was killed (death " + std::to_string(r.deaths) + " of this session)";
    } else if (def->name == "give-experience") {
        const PlayerSession::GmXpResult r = handle.player->gm_give_experience(*handle.connection, whole("amount"), now_(), by);
        if (!r.ok) return failed(GmStatus::FailedPrecondition, r.refusal);
        out.message = out.character + ": +" + std::to_string(whole("amount")) + " Adventuring XP (now " + std::to_string(r.after) + ", level " + std::to_string(r.level_after) + ")" +
                      (r.leveled ? "; level " + std::to_string(r.level_before) + " -> " + std::to_string(r.level_after) : "");
    } else if (def->name == "start-quest" || def->name == "complete-quest") {
        const bool start = def->name == "start-quest";
        const bool bypass = text("bypass") == "yes";
        const PlayerSession::GmQuestResult r = start ? handle.player->gm_start_quest(*handle.connection, text("quest"), bypass, now_(), by)
                                                     : handle.player->gm_complete_quest(*handle.connection, text("quest"), now_(), by);
        if (!r.ok) {
            const bool missing = r.refusal.rfind("no quest is named", 0) == 0 || r.refusal.find(" has not taken ") != std::string::npos;
            return failed(missing ? GmStatus::NotFound : GmStatus::FailedPrecondition, r.refusal);
        }
        out.message = out.character + ": " + (start ? "started " : "completed ") + r.quest + " (" + std::to_string(r.quest_guid) + ")";
    } else if (def->name == "complete-objective") {
        if (value("quest") != nullptr) {   // a quest was named: the objective must be one of its own
            const quest::QuestBook* book = world_.quests();
            const content::QuestRow* named = book ? book->find(text("quest")) : nullptr;
            if (!named) return failed(GmStatus::NotFound, "no quest is named \"" + log_text(text("quest")) + "\"");
            const bool its = std::any_of(named->objectives.begin(), named->objectives.end(), [&](const content::QuestObjectiveRow& o) { return o.guid == whole("objective"); });
            if (!its) return failed(GmStatus::InvalidArgument, "the objective " + std::to_string(whole("objective")) + " is not an objective of " + named->name);
        }
        const content::QuestRow* of_quest = value("quest") != nullptr && world_.quests() ? world_.quests()->find(text("quest")) : nullptr;
        const PlayerSession::GmObjectiveResult r = handle.player->gm_complete_objective(*handle.connection, whole("objective"), now_(), by, of_quest ? of_quest->guid : 0);
        if (!r.ok) {
            const bool missing = r.refusal.rfind("no quest of", 0) == 0;
            return failed(missing ? GmStatus::NotFound : GmStatus::FailedPrecondition, r.refusal);
        }
        out.message = out.character + ": completed objective " + std::to_string(r.objective) + " of " + r.quest_name + " (" + std::to_string(r.quest) + ")";
    } else if (def->name == "reset-quests") {
        const PlayerSession::GmResetQuestsResult r = handle.player->gm_reset_quests(*handle.connection, now_(), by);
        if (!r.ok) return failed(GmStatus::FailedPrecondition, r.refusal);
        out.message = out.character + ": forgot " + std::to_string(r.completed) + " finished quest(s) and " + std::to_string(r.variables) + " variable(s); " +
                      std::to_string(r.taken) + " taken quest(s) stay";
    } else if (def->name == "clear-cooldowns") {
        const PlayerSession::GmCooldownResult r = handle.player->gm_clear_cooldowns(*handle.connection, now_(), by);
        if (!r.ok) return failed(GmStatus::FailedPrecondition, r.refusal);
        out.message = out.character + ": " + std::to_string(r.cleared) + " cooldown group(s) ended";
    } else if (def->name == "give-mount") {
        const spawns::MountBook* book = world_.mounts();
        if (!book) return failed(GmStatus::FailedPrecondition, "this world has no mounts (its content.db lacks the mount tables)");
        const content::MountRow* found = nullptr;
        size_t count = 0;
        for (const content::MountRow& row : book->data().mounts) {
            if (!iequal(row.item_name, text("mount"))) continue;
            if (!found) found = &row;
            ++count;
        }
        if (!found) {
            for (const content::MountRow& row : book->data().mounts) {
                if (!iequal(row.blueprint, text("mount"))) continue;
                if (!found) found = &row;
                ++count;
            }
        }
        if (!found) return failed(GmStatus::NotFound, "no mount is called \"" + text("mount") + "\" (give its item's name, such as Mount_Grounded_Horse_0)");
        if (count > 1) return failed(GmStatus::InvalidArgument, "\"" + text("mount") + "\" names " + std::to_string(count) + " mounts: give the whole item's name or its Guid");
        const GmItemGrant r = give_item(out.character, std::to_string(found->item), 1, by);
        if (r.status != GmStatus::Ok) return failed(r.status, r.message);
        out.message = out.character + ": the mount " + found->item_name + " (" + std::to_string(found->item) + ") is in its bag; summon it as any mount";
    } else if (def->name == "set-time") {
        double hour = number("hour");
        if (hour >= 24.0) hour = 0.0;
        if (hour < 0.0) hour = 0.0;
        const double now = world_.world_clock();
        const double game_time = world_.day_clock().game_time(now);
        const double day = std::floor(game_time / world::WorldClock::kGameDay);
        world_.set_day_clock(world::WorldClock::reading(day * world::WorldClock::kGameDay + hour * 3600.0, now));
        size_t sent = 0;
        for (const PlayerHandle& h : directory_()) {
            if (h && h.player->gm_send_clock(*h.connection, now_())) ++sent;
        }
        out.message = "the world clock reads " + world::hour_text(hour) + "; " + std::to_string(sent) + " player(s) were sent the new time";
    } else if (def->name == "node-xp") {
        world::NodeControl* control = world_.node_control();
        if (!control) return failed(GmStatus::FailedPrecondition, "worldd has no city node service: the node commands are unavailable");
        control->add_experience(text("node"), whole("amount"), by);
        out.message = "asked the city node service to add " + std::to_string(whole("amount")) + " XP to " + text("node") + " (its answer is in the log)";
    } else {
        return failed(GmStatus::InvalidArgument, "no command is named \"" + log_text(def->name) + "\"");
    }

    spdlog::info("[admin] run {} by {}: character=\"{}\" fields={} -> {}", log_text(def->name), log_text(by), log_text(out.character), fields_for_log(fields), log_text(out.message));
    return out;
}

size_t GmTravel::teleport_characters(const std::string& names, double x, double y, double z, const std::string& by, Clock::time_point now) {
    size_t moved = 0;
    std::string token;
    const auto flush = [&] {
        if (token.empty()) return;
        const PlayerHandle h = find_player(token);
        if (!h) {
            spdlog::warn("[gm] {}: the character \"{}\" is not in the world", by, log_text(token));
        } else if (h.player->gm_teleport(*h.connection, x, y, z, now, by).status == PlayerSession::GmMoveResult::Status::Moved) {
            ++moved;
        }
        token.clear();
    };
    for (const char c : names) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) flush();
        else token.push_back(c);
    }
    flush();
    return moved;
}

// The session side of the game-master test commands (a file of its own, included here: CMakeLists is not this change's to edit).
#include "session/gm_admin.inc"

}}  // namespace aoc::worldd
