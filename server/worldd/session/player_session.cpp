// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_session.cpp
//
//  The player session's core: the login identity, the join, sending and the queue of replies, the periodic save and the end of the session, the intake of the client's bunches
//  and the table of the client's calls (rpc_table.h).  The features' members are defined in the player_<feature>.cpp files next to it; the class is declared in player_session.h.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "crafting/job.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "world/city_nodes.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "items/acquired.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "world/zoi_surface.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

#include "session/player_session_util.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

namespace {

/// Where a mount's ground is looked for over the point it appears at (inside a bowl, where there are floors above each other): from 1.6 m over the player's feet down to 10 m under them.
constexpr double kMountGroundUpCm = 160.0;

constexpr double kMountGroundDownCm = 1000.0;

const char* role_name(Role r) {
    switch (r) {
        case Role::PlayerController: return "PlayerController";
        case Role::GameState: return "GameState";
        case Role::Pawn: return "pawn";
        case Role::PlayerState: return "PlayerState";
    }
    return "?";
}
// (C | Y | B : C O A)

const char* mode_name(bootstrap::GameStateMode m) {
    switch (m) {
        case bootstrap::GameStateMode::Tier1: return "tier1";
        case bootstrap::GameStateMode::Tier2: return "tier2";
        case bootstrap::GameStateMode::Tier3: return "tier3";
        case bootstrap::GameStateMode::Captured: return "captured";
    }
    return "?";
}

/// The parts Connection::send_chain makes of a chain (connection.h: each export bunch, then the content whole up to
/// kMaxSingleBunchBits, else parts of kMaxPartialBunchBits).
size_t parts_of(const Chain& c) {
    const size_t bits = c.payload.bit_pos();
    const size_t content =
        bits > Connection::kMaxSingleBunchBits ? (bits + Connection::kMaxPartialBunchBits - 1) / Connection::kMaxPartialBunchBits : 1;
    return c.exports.size() + content;
}

}  // namespace

// ============================================================================================================================
//  the login identity
// ============================================================================================================================

std::string url_option(std::string_view url, std::string_view key) {
    size_t pos = 0;
    while (pos <= url.size()) {
        size_t end = url.find('?', pos);
        if (end == std::string_view::npos) end = url.size();
        const std::string_view token = url.substr(pos, end - pos);
        const size_t eq = token.find('=');
        if (eq != std::string_view::npos && iequals(token.substr(0, eq), key)) return std::string(token.substr(eq + 1));
        pos = end + 1;
    }
    return {};
}

bootstrap::LoginIdentity make_login_identity(const std::optional<LoginMessage>& message, const svc::Character& character,
                                             std::vector<std::string>* notes) {
    bootstrap::LoginIdentity id;
    if (message) id.name_option = url_option(message->request_url, "Name");
    if (id.name_option.empty()) {
        id.name_option = character.name;
        if (notes) notes->push_back("the Login has no ?Name= option: the PlayerState's name is the character's");
    }
    if (message && message->has_unique_id && !message->unique_id.empty()) {
        id.unique_id = message->unique_id;
    } else {
        id.unique_id = id.name_option;
        if (notes) notes->push_back("the Login has no unique id of the form the capture's has: the PlayerState's is the name option");
    }
    return id;
}

// ============================================================================================================================
//  join
// ============================================================================================================================

bool PlayerSession::allocate_channels(const Connection& conn) {
    std::array<bool, 4> done{};
    uint32_t index = 3;   // 0 is the control channel; the original's actors began at 3
    for (size_t role = 0; role < channels_.size(); ++role) {
        for (;; ++index) {
            if (index > 4096) return false;
            if (conn.channel_open(index) || conn.channel_closing(index)) continue;
            break;
        }
        channels_[role] = index++;
        done[role] = true;
    }
    return std::all_of(done.begin(), done.end(), [](bool b) { return b; });
}

PlayerSession::~PlayerSession() {
    forget_party();
    clear_pet();
    cancel_quick_make(nullptr, "the session is destroyed");
    cancel_gather(nullptr, "the session is destroyed");
    leave_combat();
    leave_presence("the session is destroyed");   // (a session dropped without finish(): the other players' views still close it)
}

std::unique_ptr<PlayerSession> PlayerSession::join(Connection& conn, PlayerWorld& world, const svc::Character& character,
                                                   const bootstrap::LoginIdentity& login, double world_time,
                                                   std::string& error, bootstrap::SetupRefusal* refusal) {
    if (refusal) *refusal = bootstrap::SetupRefusal::Other;   // the setup's own kind replaces it below
    if (!world.ready()) {
        error = "content.db is not loaded: " + world.content_problem();
        return nullptr;
    }
    const bootstrap::SessionValues values = world.new_session(world_time);
    bootstrap::CharacterSetupResult built = bootstrap::build_player_setup(character, *world.content(), login, values, world.stat_context());
    if (!built.setup) {
        error = built.error;
        if (refusal) *refusal = built.refusal;
        return nullptr;
    }
    // the vendors milestone, step 0: the purse (the first OwnerInfo already carries it: build_player_setup)
    const items::Wallet purse(character.wallet);
    // Gathering stage 3: a resource container of a second or third bag needs a guid of its own (minted here, after everything else, so that the one bag every class starts with
    // changes no id)
    for (size_t i = 1; i < built.setup->resource_bags.size(); ++i) {
        if (built.setup->resource_bags[i].present && built.setup->guids.resource_storage[i].ObjectId == 0) built.setup->guids.resource_storage[i] = world.mint_guid();
    }
    // the five city nodes' bowls (world/town_floor.h): a character saved on the baked ground under a town (a rider in the air was saved at the ground below it, before the town floor was
    // known) would join about 50 m under the town: it joins on the town floor.  worldd only changes its own copy of the position; the record stays (the first move saves the new one).
    if (const auto lifted = world.lift_out_of_bowl(built.setup->x, built.setup->y, built.setup->z)) {
        const auto* node = world.city_nodes() ? world.city_nodes()->by_guid(lifted->node) : nullptr;   // (`world` is the PlayerWorld here)
        spdlog::info("[session] {}: was saved at ({:.1f}, {:.1f}, {:.1f}) cm, {:.0f} m under the floor of a town in a bowl ({}): joins on it, at z {:.1f}", built.setup->name, built.setup->x,
                     built.setup->y, built.setup->z, lifted->under_cm / 100.0, node ? node->display_name : std::to_string(lifted->node), lifted->z);
        built.setup->z = lifted->z;
    }
    // Nodes 1, stage 2: the GameState's node maps at the join, and OwnerInfo's list of the account's characters.  The registry's version is read BEFORE the maps are made: a
    // change that lands in between is sent again by run_nodes at the first tick (harmless: the arrays then differ in nothing).
    uint64_t nodes_version = 0;
    if (world.gamestate_mode() == bootstrap::GameStateMode::Tier3) {
        if (world.city_nodes() && world.node_wire()) {
            nodes_version = world.city_nodes()->version();
            built.setup->nodes = world.node_wire()->maps(*world.city_nodes());
        }
        std::string own = character.id;
        for (char& c : own) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        bool listed_self = false;
        for (const svc::AccountCharacter& other : character.account_characters) {
            bootstrap::AccountCharacterNode n;
            n.guid_hex = other.id;
            for (char& c : n.guid_hex) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            n.name = other.name;
            listed_self = listed_self || n.guid_hex == own;
            built.setup->account_characters.push_back(std::move(n));
        }
        if (!listed_self) {
            bootstrap::AccountCharacterNode n;
            n.guid_hex = own;
            n.name = built.setup->name;
            built.setup->account_characters.insert(built.setup->account_characters.begin(), std::move(n));
        }
    }
    // missing systems, step 0: the tutorials the character finished go to the client with the join (an id the store would never hold, a repeat, or what is over the cap is left out)
    std::vector<int64_t> finished;
    for (const int64_t id : character.completed_ftues) {
        if (id > 0 && finished.size() < bootstrap::kMaxCompletedFtues && std::find(finished.begin(), finished.end(), id) == finished.end()) finished.push_back(id);
    }
    built.setup->completed_ftues = finished;
    std::unique_ptr<PlayerSession> p(
        new PlayerSession(std::move(*built.setup), world.gamestate_mode(), character.id, world.movement(), world.position_sink(),
                          world.ability_rules(), std::move(*built.inventory), world.item_catalog(), std::move(built.character_stats), &world,
                          character.class_id, built.progress));
    p->wallet_ = purse;
    p->jobs_ = character.processing_jobs;   // crafting, step 1: the jobs run on the wall clock, whether the character was logged in or not
    for (const svc::ProcessingJob& j : p->jobs_) p->job_seen_[j.job_guid] = crafting::status_at(j, wall_ms());   // (step 2: a window opened now shows them so; the changes after are sent)
    p->sync_active_stations(wall_ms());
    p->completed_ftues_ = std::move(finished);
    p->init_quests(world, character);   // quests: the saved log becomes the quest component's arrays, sent with the join
    if (!p->completed_ftues_.empty()) spdlog::info("[session] {}: {} finished tutorial(s) on the character's list, sent with the join", p->pb_.setup().name, p->completed_ftues_.size());
    p->wh_storage_.load(character.warehouse_items);   // storage clerks, step 0: what the character keeps in the warehouse (the base chunk gets its component below, the tabs at the first open of their town)
    p->init_attunements(character);   // Embersprings: the places the character discovered, its home and the last use of it, sent with the join
    p->taught_ = character.known_recipes;   // crafting, step 3: the recipes a game master taught the character (the others are known to the client by their own flag)
    if (!p->taught_.empty() && !p->pb_.set_known_recipes(p->taught_)) {
        spdlog::warn("[session] {}: the {} recipes the character was taught do not fit the player's state: the client is not told of them", p->pb_.setup().name, p->taught_.size());
    }
    p->init_ignored(character);   // social, step 6: the players and the guilds the character ignores, sent with the join
    p->notes_ = std::move(built.notes);
    if (built.items_repaired) {   // Gathering stage 3: the login moved resource items out of the Backpack: the record in the database is the old layout
        p->inventory_dirty_ = true;
        p->inventory_changed_ = true;
    }
    p->professions_ = std::move(built.professions);
    p->weapon_ = std::move(built.weapon);   // weapon XP: the totals of the weapon lines (the join already lists the ones with XP)
    if (p->stats_ && p->professions_.valid()) p->stats_->set_profession_levels(p->professions_.levels_by_name());
    p->node_maps_version_ = nodes_version;
    // the purchase reply's delay (a probe of the D1 draft's bar animation): the default is retail's, the variable changes it
    if (const char* env = std::getenv("AOC_PURCHASE_REPLY_DELAY_MS")) {
        char* end = nullptr;
        const long ms = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && ms >= 0 && ms <= 2000) {
            p->purchase_delay_ = std::chrono::milliseconds(ms);
            spdlog::info("[session] {}: AOC_PURCHASE_REPLY_DELAY_MS = {}: a purchase's reply goes out {} ms after the request", p->pb_.setup().name, ms, ms);
        } else {
            spdlog::warn("[session] {}: AOC_PURCHASE_REPLY_DELAY_MS = '{}' is not 0 to 2000: the default {} ms stays", p->pb_.setup().name, env,
                         kPurchaseReplyDelay.count());
        }
    }
    // plan D1, stage 2: the character's ranks, its action bar and the abilities the join sent
    p->book_ = std::move(built.book);
    p->bar_ = std::move(built.action_bar);
    p->wielded_line_ = built.wielded_line;
    p->known_ = p->pb_.setup().known_abilities;
    for (const bootstrap::AbilityRecord& r : p->pb_.setup().records_to_load) p->records_.push_back(r.guid);
    if (p->book_ && world.content()) {
        if (const auto w = bootstrap::worn_weapon(*world.content(), p->inventory_, items::equip::Primary)) p->combo_ = w->combo;
    }
    if (!p->pb_.ok()) {
        error = "the bootstrap does not build: " + p->pb_.error();
        return nullptr;
    }
    if (!p->allocate_channels(conn)) {
        error = "no free actor channels";
        return nullptr;
    }
    // plan E1: the world's other actors (the NPCs, the nodes and this player's lingering zones). The view never takes a channel of the player's four actors.
    p->registry_ = &world.registry();
    p->npc_world_ = &world;
    {
        PlayerSession* self = p.get();
        ActorView::Config view_config = world.view_config();
        view_config.player_id = static_cast<uint64_t>(p->pb_.setup().player_id);
        view_config.player_name = p->pb_.setup().name;
        {
            // the player's own interaction state: its CharacterGuid goes in the maps of every interactable actor the view opens for this connection
            const auto& g = p->pb_.setup().character_guid;
            view_config.player_guid = repl::GuidValue{g[0], g[1], g[2], g[3]};
        }
        // the mounts milestone: this player's mount (the world's registry holds the actor; the session says what the client asked)
    if (world.mounts()) {
        MountController::Owner owner;
        owner.player_id = static_cast<uint64_t>(p->pb_.setup().player_id);
        const auto& cg = p->pb_.setup().character_guid;
        owner.guid = repl::GuidValue{cg[0], cg[1], cg[2], cg[3]};
        owner.name = p->pb_.setup().name;
        owner.pawn = p->pb_.setup().guids.pawn;
        owner.server_id = p->pb_.setup().server_id;
        PlayerWorld* w = &world;
        p->mounts_ = std::make_unique<MountController>(world.registry(), *world.mounts(), [w] { return w->mint_guid(); }, std::move(owner));
        if (world.nav() || world.town_platforms()) {
            p->mounts_->set_ground([w](double x, double y, double hint) -> std::optional<double> {
                // inside the footprint of a bowl whose platform is read the ground is a floor of the place: the street, a terrace, the floor of a building (the baked terrain there is the pit under
                // the town, 50 m down, which is why a summon was refused in every town: "Cannot summon mount here.").  It is the highest floor from 1.6 m over the player's feet down to 10 m
                // under them, as a trace down from over the player would meet it: a terrace the player stands on is the ground, and a tower top, whose point ahead is over the street far
                // below, has none and keeps the refusal (the mount's rule then: at most 2.5 m between the feet and the ground).
                if (w->bowl_floor_at(x, y)) return w->floor_under(x, y, hint, kMountGroundDownCm, kMountGroundUpCm);
                const nav::AreaNav* nav = w->nav();
                if (!nav) return hint;   // (a world with platforms and no ground file: nothing to judge outside the bowls)
                double z = 0;
                // (review A I4) a chunk that is not loaded yet (the first moments after a login or a teleport) has none of its navmesh floors: the terrain alone would refuse a summon on
                // a bridge.  Where the terrain exists but the floors are not here to judge, the player's own height stands.
                if (nav && !nav->ready_at(x, y) && nav->terrain_height(x, y, &z)) return hint;
                if (nav && nav->ground_height(x, y, hint, &z)) return z;
                return std::nullopt;
            });
        }
        const spawns::MountBook* book = world.mounts();
        p->mounts_->set_speeds([book](const std::string& blueprint) -> double {
            const spawns::MountBook::Speeds* s = book->speeds_of(blueprint);
            return s ? s->run : 0.0;
        });
        PlayerSession* me = p.get();
        p->mounts_->set_instance_source([me]() -> int64_t { return me->abilities_ ? me->abilities_->allocate_instance_id() : -1; });
    }
    p->view_ = std::make_unique<ActorView>(world.registry(), view_config, [self](uint32_t index) {
            for (const uint32_t c : self->channels_) {
                if (c == index) return true;
            }
            return false;
        });
    }
    auto sequence = p->pb_.join_sequence();
    if (!sequence) {
        error = "the join sequence does not build: " + (p->pb_.error().empty() ? std::string("a step failed") : p->pb_.error());
        return nullptr;
    }
    for (bootstrap::Send& s : *sequence) p->join_.push_back(std::move(s));
// [C | O | A x C Y B]

    const bootstrap::PlayerSetup& setup = p->pb_.setup();
    spdlog::info("[session] {}: joining as ServerId {}, PlayerId {}, GameState {}; channels PlayerController {}, GameState {}, pawn {}, "
                 "PlayerState {}; at ({:.1f}, {:.1f}, {:.1f}) cm",
                 setup.name, setup.server_id, setup.player_id, mode_name(world.gamestate_mode()), p->channels_[0], p->channels_[1],
                 p->channels_[2], p->channels_[3], setup.x, setup.y, setup.z);
    for (const std::string& note : p->notes_) spdlog::info("[session] {}: {}", setup.name, note);
    {   // the game hour the join's ClientSetTime starts this player's sky at (world/world_clock.h)
        const double hour = setup.clock.hour(setup.world_time);
        spdlog::info("[clock] {}: the join sets the client's game time to {} (hour {:.2f}, {}; game time {:.0f} s)", setup.name, world::hour_text(hour), hour,
                     world::WorldClock::is_night(hour) ? "night" : "day", setup.clock.game_time(setup.world_time));
    }
    if (p->inventory_.size() == 0) {
        spdlog::warn("[session] {}: the character has no items (the character service gives a character its class's starting kit when it starts: is it running a build with items?)", setup.name);
    }
    spdlog::info("[session] {}: level {}, Adventuring {} XP (debt {}), class {}", setup.name, p->progress_.level, p->progress_.xp, p->progress_.debt,
                 bootstrap::client_class_name(p->class_id_) ? bootstrap::client_class_name(p->class_id_) : "?");
    if (p->professions_.valid()) {
        size_t beyond = 0;
        std::string highest;
        int best = 0;
        for (const professions::Profession& pf : p->professions_.book()->all()) {
            const professions::State st = p->professions_.state(pf.guid);
            beyond += st.tier > professions::kTierNovice || st.xp > 0;
            if (st.level > best) {
                best = st.level;
                highest = pf.name + " " + std::to_string(st.level) + " (" + p->professions_.book()->tier(st.tier).name + ")";
            }
        }
        spdlog::info("[session] {}: {} professions, {} beyond Novice level 1 with no XP{}", setup.name, p->professions_.book()->size(), beyond,
                     highest.empty() || best <= 1 ? std::string() : "; the highest is " + highest);
    }
    if (p->weapon_.valid()) {
        std::string lines;
        for (const auto& [type, xp] : p->weapon_.with_xp()) {
            const weaponxp::Line* line = p->weapon_.lines()->find(type);
            lines += (lines.empty() ? "" : ", ") + (line ? line->name : std::to_string(type)) + " level " + std::to_string(p->weapon_.level(type)) + " (" + std::to_string(xp) + " XP)";
        }
        spdlog::info("[session] {}: {} weapon lines, {} with XP{}", setup.name, p->weapon_.lines()->size(), p->weapon_.with_xp().size(), lines.empty() ? std::string() : ": " + lines);
    }
    p->list_item_chunk(conn);   // storage clerks, step 0: the base item chunk is on the pawn from the join, listed in OwnerInfo (queued behind the join)
    p->pump(conn);
    if (refusal) *refusal = bootstrap::SetupRefusal::None;
    return p;
}

spdlog::level::level_enum PlayerSession::level_for(size_t& count, size_t limit) {
    return ++count <= limit ? spdlog::level::info : spdlog::level::debug;
}

// ============================================================================================================================
//  sending
// ============================================================================================================================

void PlayerSession::fail(std::string why) {
    if (failed_) return;
    failed_ = true;
    failure_ = std::move(why);
    spdlog::error("[session] {}: {}", name(), failure_);
}

PlayerSession::Outcome PlayerSession::send(Connection& conn, bootstrap::Send& s) {
    Chain chain;
    chain.channel = s.channel != 0 ? s.channel : channels_[static_cast<size_t>(s.role)];
    chain.open = s.opens_channel;
    chain.reliable = s.reliable();
    chain.exports = std::move(s.emission.exports);
    chain.payload = std::move(s.emission.payload);
    if (!conn.can_send_chain(chain)) {
        s.emission.exports = std::move(chain.exports);
        s.emission.payload = std::move(chain.payload);
        // Only a full window is a reason to wait: a reliable chain behind unacknowledged bunches of its channel. With
        // nothing in flight there, the chain cannot go out however long it waits (too many parts, a channel in the
        // wrong state) and an unreliable one is simply not sent.
        if (!chain.reliable || conn.unacked_reliable(chain.channel) == 0) return Outcome::Refused;
        blocked_ = true;   // nothing is built or tried again until an acknowledgement frees some of this channel's window
        blocked_channel_ = chain.channel;
        blocked_unacked_ = conn.unacked_reliable(chain.channel);
        return Outcome::Wait;
    }
    const size_t parts = parts_of(chain);
    const size_t export_parts = chain.exports.size();
    const ChainId id = conn.send_chain(chain);
    if (id == 0) return Outcome::Refused;   // cannot happen after can_send_chain; the Connection logged why
    if (!s.emission.exported_by_bunch.empty()) {
        ChainExports e;
        e.by_bunch = std::move(s.emission.exported_by_bunch);
        e.reliable = chain.reliable;
        e.outstanding = std::min(export_parts, e.by_bunch.size());
        exports_[id] = std::move(e);
    }
    // The join's chains each get a line; the replies (the client can ask for them again and again) only the first
    // kLoggedPerKind, and the unreliable move acknowledgements none.
    const spdlog::level::level_enum level = !chain.reliable  ? spdlog::level::debug
                                            : !join_.empty() ? spdlog::level::info
                                                             : level_for(reply_sent_logged_);
    spdlog::log(level, "[session] {}: sent {} on channel {} ({} part{}{})", name(), s.what, chain.channel, parts, parts == 1 ? "" : "s",
                chain.reliable ? "" : ", unreliable");
    return Outcome::Sent;
}
// C-O-A - Cyb

void PlayerSession::pump(Connection& conn) {
    if (failed_) return;
    if (blocked_ && conn.unacked_reliable(blocked_channel_) >= blocked_unacked_) return;
    blocked_ = false;
    // The join, in order: what is queued behind a chain that cannot go yet does not overtake it.
    while (!join_.empty()) {
        bootstrap::Send& head = join_.front();
        switch (send(conn, head)) {
            case Outcome::Sent:
                join_.pop_front();
                waiting_logged_ = false;
                break;
            case Outcome::Wait:
                if (!waiting_logged_) {
                    spdlog::info("[session] {}: the window of channel {} is full: {} chain(s) of the join wait for acknowledgements", name(),
                                 channels_[static_cast<size_t>(head.role)], join_.size());
                    waiting_logged_ = true;
                }
                return;
            case Outcome::Refused:
                fail(std::string("the join cannot be sent: ") + head.what + " was refused by the connection");
                return;
        }
    }
    // The replies. Each is built as it goes: a chain that cannot go yet is built again, once the window has room, from the
    // state before it (the checkpoint), never sent twice and never lost.
    while (!replies_.empty()) {
        Step& step = replies_.front();
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        std::optional<bootstrap::Send> built = step.build(pb_);
        if (!built) {
            if (step.must_build) {
                spdlog::warn("[session] {}: {} produced nothing to send: the client gets no reply (the bootstrap says: {})", name(),
                             step.what, pb_.error().empty() ? "nothing changed" : pb_.error());
            } else {
                spdlog::debug("[session] {}: nothing to send for {}", name(), step.what);
            }
            replies_.pop_front();
            continue;
        }
        switch (send(conn, *built)) {
            case Outcome::Sent:
                replies_.pop_front();
                break;
            case Outcome::Wait:
                pb_.restore(checkpoint);
                return;
            case Outcome::Refused:
                pb_.restore(checkpoint);
                spdlog::error("[session] {}: {} was refused by the connection: dropped", name(), step.what);
                replies_.pop_front();
                break;
        }
    }
}

void PlayerSession::tick(Connection& conn) { pump(conn); }

void PlayerSession::tick(Connection& conn, Clock::time_point now) {
    const TickPlan plan = tick_logic(conn, now, /*plan_view=*/false);   // (the three parts are in session/player_view_phases.cpp)
    if (plan.stopped) return;
    // #14 review: the presence sync (this player as the others see it) is part of tick_logic now, once a tick, and runs there with the view passes on threads as well: the server does not call
    // this function then.
    if (plan.view) view_->tick(conn, plan.x, plan.y, now);
    tick_after(now, plan);
}
// -- C Y B & CoA --

void PlayerSession::log_position(Clock::time_point) {
    // The investigation of 2026-10-10: the periodic save logs its first three at info, so a 5-minute stretch of a test had no position in the log.  This line is every 30 s and says what
    // the ground and the view hold at that place.
    const GoodPosition at = checker_.good();
    std::string ground;
    if (npc_world_ && npc_world_->nav()) {
        const nav::AreaNav* nav = npc_world_->nav();
        const nav::ChunkKey k = nav->chunk_at(at.x, at.y);
        ground = "; ground chunk (" + std::to_string(k.x) + ", " + std::to_string(k.y) + ") " + (nav->ready_at(at.x, at.y) ? "ready" : "NOT LOADED YET") +
                 (nav->has_region() && !nav->in_region(at.x, at.y) ? ", outside the Riverlands" : "");
    }
    const double from_login = npc_world_ ? std::hypot(at.x - npc_world_->area().center_x, at.y - npc_world_->area().center_y) / 100.0 : 0.0;
    spdlog::info("[session] {}: at ({:.0f}, {:.0f}, {:.0f}) cm, {:.0f} m from the login spot, movement mode {}{}; {} moves acknowledged, {} refused ({} under the ground), {} dropped as stale{}; its view holds {} awake and {} dormant actors",
                 name(), at.x, at.y, at.z, from_login, static_cast<unsigned>(at.movement_mode), mounted() ? " (mounted)" : "", moves_acked_, moves_refused_, moves_surface_refused_, moves_stale_, based_move_summary() + ground,
                 view_ ? view_->count(world::ClientState::Awake) : 0, view_ ? view_->count(world::ClientState::Dormant) : 0);
}

// ============================================================================================================================
//  saving
// ============================================================================================================================

bool PlayerSession::position_changed(bool with_yaw) const {
    // Moves carry the location at 0.01 cm, so the spawn point a character loaded with comes back a fraction of a millimetre
    // off in its first move: a centimetre is a move. Yaw is a 16-bit angle, the same on both sides when it did not turn.
    constexpr double kMovedCm = 1.0;
    constexpr double kTurnedDegrees = 1e-4;
    const GoodPosition g = last_position();
    return std::hypot(std::hypot(g.x - saved_.x, g.y - saved_.y), g.z - saved_.z) > kMovedCm ||
           (with_yaw && std::fabs(g.yaw_degrees - saved_.yaw_degrees) > kTurnedDegrees);
}

GoodPosition PlayerSession::last_position() const {
    GoodPosition g = checker_.good();
    if (mounted()) {
        // a rider is saved where it would be put beside its mount, so that a relog (or a second login) puts it on foot there, not inside the ground
        const Ride::Place p = Ride::rest_place(g.x, g.y, g.z, g.yaw_degrees);
        const double mount_z = g.z;
        g.x = p.x;
        g.y = p.y;
        g.z = p.z;
        // stage 4, modelled: a rider in the air logs back in on the ground below its last good position (a player stands 98 cm above its ground)
        if (const auto ground = (g.movement_mode == repl::kMoveFlying || g.movement_mode == repl::kMoveFalling) ? ground_below(p.x, p.y, mount_z) : std::nullopt) {
            if (mount_z - *ground > Ride::kAirborneCm) g.z = *ground + 98.0;
        }
    }
    return g;
}

void PlayerSession::hand_over(const char* why, bool periodic) {
    if (!sink_) return;
    const GoodPosition g = last_position();
    svc::Position p;
    p.x = g.x;
    p.y = g.y;
    p.z = g.z;
    p.yaw = static_cast<float>(g.yaw_degrees);
    sink_->save(character_id_, p);
    saved_ = g;
    ++handed_over_;
    const spdlog::level::level_enum level = periodic ? level_for(periodic_save_logged_) : spdlog::level::info;
    spdlog::log(level, "[session] {}: saving the last good position ({:.1f}, {:.1f}, {:.1f}) cm, yaw {:.1f} ({})", name(), g.x, g.y, g.z,
                g.yaw_degrees, why);
}
// built with C-O-A and C_Y_B

void PlayerSession::finish(const char* why) {
    if (finished_) return;
    finished_ = true;
    leave_party_on_finish();
    clear_pet();
    cancel_quick_make(nullptr, why);   // Crafting, step 3: nothing was taken, so nothing is given back
    end_gathering(why);   // Gathering stage 2: a node this player channels is available again, and the pile a full bag made is made now if it still waits
    // this player's lingering zones leave the world with it (the actor system closes them for a connection that is still there)
    if (registry_) {
        for (const auto& [id, za] : zone_actors_) registry_->remove(za.actor);
    }
    zone_actors_.clear();
    leave_presence(why);   // the other players' views close this player's pawn (Destroyed)   (#14 review: it had a PlayerState beside it, which is gone)
    if (mounts_) {
        // the mount leaves the world with its owner; its item keeps the Health it had
        if (const auto kept = mounts_->shutdown()) apply_persist(nullptr, *kept);
    }
    leave_combat();
    if (npc_world_) npc_world_->forget_player(static_cast<uint64_t>(pb_.setup().player_id));
    if (view_) {
        spdlog::info("[session] {}: the world's actors: {}", name(), view_->describe());
        view_->finish();
    }
    // (the client's calls by name: what reached dispatch, so that a call worldd never logged -- a tree's start -- is told from one that never arrived)
    if (!client_calls_.empty()) {
        std::map<std::string, size_t> by_name;
        for (const auto& [rpc_name, count] : client_calls_) by_name[rpc_name] += count;
        std::vector<std::pair<std::string, size_t>> calls(by_name.begin(), by_name.end());
        std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        std::string text;
        for (size_t i = 0; i < calls.size() && i < 30; ++i) text += (i ? ", " : "") + calls[i].first + " " + std::to_string(calls[i].second);
        spdlog::info("[session] {}: the client's calls by name ({} kinds, the most first): {}{}", name(), calls.size(), text, calls.size() > 30 ? ", ..." : "");
    }
    // (what the reader did not serve: a bunch that does not read takes every call in it away, so the count is part of the summary)
    if (malformed_bunches_ > 0 || in_place_blocks_ > 0 || skipped_fields_ > 0) {
        spdlog::info("[session] {}: the client's calls the reader did not serve: {} bunches that did not read at all (ignored whole); {} calls on subobjects it names by path (skipped, in {} bunches, {} names); {} RPC "
                     "fields of no RPC of their class (skipped)",
                     name(), malformed_bunches_, in_place_blocks_, in_place_bunches_, in_place_names_.size(), skipped_fields_);
    }
    // Is what the pawn's Stamina array holds (what was last put into the updates the client gets) what the server's runtime has?  A cache that
    // drifted would freeze the HUD until a relog (E1 stage 1, fix 2: it did not, the move clock was the cause; this keeps checking).
    if (abilities_ && !abilities_failed_) {
        const auto held = pb_.current_stats();
        const auto it = held.find(content::kGuidStamina);
        if (it != held.end()) {
            const double server = abilities_->stamina();
            const bool same = static_cast<float>(server) == it->second;
            spdlog::log(same || abilities_dirty_ ? spdlog::level::info : spdlog::level::warn,
                        "[session] {}: Stamina at the end: the server's {:.2f}, the pawn's array {:.2f} ({}{})", name(), server, it->second,
                        same ? "the same" : "DIFFERENT", abilities_dirty_ ? "; an update was still waiting to be built" : "");
        }
    }
    // The saver may have given up on a periodic hand-over (the service was down), so what was last handed over proves
    // nothing: compare with the record the session started from. A position that went away and came back to the
    // record's is covered by the hand-overs made before.
    // quests: the log once more when it changed during the session (an earlier save may have been lost), then the character's commands are not taken any more
    if (quests_on_ && quest_worker_) {
        if (quest_log_.changed() || quests_touched_) quest_worker_->save(character_id_, quest_log_.to_saved());
        quest_worker_->unwatch(character_id_);
    }
    saved_ = loaded_;
    if (handed_over_ > 0 || position_changed(/*with_yaw=*/true)) hand_over(why, /*periodic=*/false);
    // the inventory, too, if it changed at all: what an earlier hand-over gave the saver may have been lost, and the latest wins
    if (inventory_changed_) hand_over_inventory(why, /*periodic=*/false);
    // and the progress (plan D1): handed over at once when it changed, and once more here for the same reason
    if (progress_changed_) hand_over_progress(why);
}

void PlayerSession::hand_over_progress(const char* why) {
    if (!sink_) return;
    svc::Progress p;
    p.level = progress_.level;
    p.class_id = class_id_;
    p.experience.push_back({content::kGuidAdventuring, progress_.xp, progress_.debt});
    for (const svc::Experience& e : weapon_.to_svc()) p.experience.push_back(e);   // weapon XP: the weapon lines' totals (a line with no row is not written)
    p.professions = professions_.to_svc();   // Gathering stage 2: all 22 whole, as the session holds them (empty without the world's book)
    if (book_) {
        // plan D1, stage 2: the ranks and the bar go whole with every hand-over (what the session holds is what the character has)
        p.replace_skill_ranks = true;
        for (const auto& [tree, ranks] : book_->purchased()) p.skill_ranks.push_back({tree, ranks});
        p.replace_action_bar = true;
        for (const repl::ActionbarSlot& slot : bar_.slots()) {
            svc::ActionbarSlot a;
            a.bank = slot.bank_index;
            a.slot = slot.slot_index;
            a.use_type = slot.use.use_type;
            a.instigator = slot.use.instigator;
            a.ability_id = slot.use.ability_id;
            a.skill_rank_id = slot.use.skill_rank_id;
            a.pet_command_id = slot.use.pet_command_id;
            a.item_record_id = slot.use.item_record_id;
            const repl::GuidValue& g = slot.use.item_guid;
            if (g.a | g.b | g.c | g.d) a.item_guid = items::guid_hex(g);
            a.item_rarity = slot.use.item_rarity;
            a.item_quality = slot.use.item_quality;
            a.combo_button = slot.use.combo_button;
            p.action_bar.push_back(std::move(a));
        }
    }
    p.replace_attunements = true;   // Embersprings: the discovered places, the home and its last use go whole with every hand-over
    for (const int n : attunements_.discovered) p.attunements.push_back(n);
    p.home_attunement = attunements_.home;
    p.home_last_used_ms = attunements_.home_last_used_ms;
    p.replace_completed_ftues = true;   // missing systems, step 0: the finished tutorials go whole with every hand-over (what the session holds is what the character has)
    p.completed_ftues = completed_ftues_;
    p.replace_ignored_names = true;   // social, step 6: the ignored players and guilds go whole with every hand-over (what the session holds is what the character has)
    p.ignored_character_names = ignored_characters_;
    p.ignored_guild_names = ignored_guilds_;
    sink_->save_progress(character_id_, std::move(p));
    ++progress_handed_over_;
    spdlog::info("[session] {}: saving the progress (level {}, class {}, Adventuring {} XP, debt {}) ({})", name(), progress_.level, class_id_, progress_.xp,
                 progress_.debt, why);
}

void PlayerSession::hand_over_inventory(const char* why, bool periodic) {
    inventory_dirty_ = false;
    if (!sink_) return;
    std::vector<svc::InventoryItem> items = inventory_.items();
    const size_t n = items.size();
    sink_->save_inventory(character_id_, std::move(items), wallet_.entries(), jobs_, taught_, wh_storage_.rows());
    ++inventories_handed_over_;
    spdlog::log(periodic ? level_for(item_periodic_logged_) : spdlog::level::info, "[session] {}: saving the inventory ({} items) ({})", name(), n, why);
}

void PlayerSession::queue(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build,
                          Connection& conn, bool must_build) {
    if (failed_) return;
    replies_.push_back({what, std::move(build), must_build});
    pump(conn);
}

void PlayerSession::queue_later(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build, Connection& conn,
                                Clock::time_point now, bool must_build) {
    if (failed_) return;
    if (purchase_delay_.count() <= 0 && delayed_.empty()) {
        queue(what, std::move(build), conn, must_build);
        return;
    }
    // behind steps that still wait, the due time is theirs or later: the order of the requests is kept
    const Clock::time_point due = std::max(now + purchase_delay_, delayed_.empty() ? now : delayed_.back().due);
    delayed_.push_back({due, {what, std::move(build), must_build}});
}

bool PlayerSession::within_budget(RequestBudget& budget, const char* what, Clock::time_point now) {
    if (budget.take(now)) return true;
    if (++dropped_ == 1) {
        spdlog::warn("[session] {}: {} comes faster than its budget allows: dropped (further requests over budget are counted, not logged)",
                     name(), what);
    }
    return false;
}

bool PlayerSession::room_for(size_t steps) {
    if (failed_) return false;
    if (replies_.size() + delayed_.size() + steps <= kMaxPendingReplies) return true;
    if (!replies_full_logged_) {
        spdlog::warn("[session] {}: {} reply steps are waiting for the connection: the client's further requests are ignored", name(),
                     replies_.size() + delayed_.size());
        replies_full_logged_ = true;
    }
    return false;
}

void PlayerSession::take_acks(Connection& conn) {
    for (const AckEvent& e : conn.take_ack_events()) {
        if (view_) view_->on_ack(e);
        const auto it = exports_.find(e.chain);
        if (it == exports_.end() || !e.export_part) continue;
        ChainExports& c = it->second;
        if (e.delivered) {
            if (e.part < c.by_bunch.size()) pb_.mark_exported(c.by_bunch[e.part]);
        } else if (c.reliable) {
            continue;   // the Connection sends the part again and its event follows
        }
        if (c.outstanding > 0 && --c.outstanding == 0) exports_.erase(it);   // a lost unreliable part is final, too
    }
}

// ============================================================================================================================
//  the client's bunches
// ============================================================================================================================
// C:Y:B.CoA

const schema::ClassSchema* PlayerSession::class_of(Role role) const {
    switch (role) {
        case Role::PlayerController: return pb_.player_controller().obj.schema;
        case Role::GameState: return pb_.game_state().obj.schema;
        case Role::Pawn: return pb_.pawn().obj.schema;
        case Role::PlayerState: return pb_.player_state().obj.schema;
    }
    return nullptr;
}

bool PlayerSession::role_of_channel(uint32_t channel, Role* role) const {
    for (size_t i = 0; i < channels_.size(); ++i) {
        if (channels_[i] == channel) {
            *role = static_cast<Role>(i);
            return true;
        }
    }
    return false;
}

void PlayerSession::on_bunch(Connection& conn, const InBunch& bunch, Clock::time_point now) {
    // the mounts milestone, stage 3: the controlled actor's channel is the mount's while the player rides it
    if (mounted() && bunch.channel != 0 && bunch.channel == mount_channel()) return on_mount_bunch(conn, bunch, now);
    Role role;
    if (!role_of_channel(bunch.channel, &role)) {
        spdlog::debug("[session] {}: ignoring a bunch on channel {}, which is none of the player's", name(), bunch.channel);
        return;
    }
    if (bunch.close || bunch.bits == 0) return;
    const schema::ClassSchema* cls = class_of(role);
    const repl::Actor* actor = role == Role::PlayerController ? &pb_.player_controller()
                               : role == Role::GameState      ? &pb_.game_state()
                               : role == Role::Pawn           ? &pb_.pawn()
                                                              : &pb_.player_state();
    const auto resolve = [actor](const FIntrepidNetworkGUID& g) -> const schema::ClassSchema* {
        const repl::Subobject* sub = actor->find(g);
        return sub ? sub->obj.schema : nullptr;
    };
    repl::CodecOptions client_opts;
    // ServerTriggerAbility's Targets may name the pawn's CollisionCylinder by path: on the pawn's channel only (the other
    // actors' RPCs have no such parameter, and an object named in place there is a malformed call)
    client_opts.inline_objects = (role == Role::Pawn);
    const auto parsed = repl::read_client_bunch(*cls, resolve, bunch.data.data(), bunch.bits, client_opts);
    if (!parsed) {
        // The connection already refused a broken packet; this is a bunch whose content blocks do not read.
        // (The first bytes in hex: the client's own data, what an unknown RPC looks like when the next run is read; stage 3 fix 2.  A bunch that does not read takes every call in it with it,
        // so none may go unseen: the first three are given whole, up to 160 bytes, and so is the first of every KIND -- its size and first eight bytes -- up to kMaxUnreadKinds, which is how
        // a start that shared a bunch with a call the reader could not read shows as a bunch of its own size; the rest one in ten, with their first 24 bytes (retest 3: they were debug lines, invisible.)
        static const char* const kHex = "0123456789abcdef";
        std::string head;
        for (size_t i = 0; i < bunch.data.size() && i < 160; ++i) {
            head += kHex[bunch.data[i] >> 4];
            head += kHex[bunch.data[i] & 15];
        }
        ++malformed_bunches_;
        const std::string kind = std::to_string(bunch.bits) + ":" + head.substr(0, 16);
        const bool new_kind = unread_kinds_.size() < kMaxUnreadKinds && unread_kinds_.insert(kind).second;
        if (malformed_bunches_ <= 3 || new_kind) {
            spdlog::warn("[session] {}: a bunch of {} bits on channel {} ({}) does not read: ignored (starts {})", name(), bunch.bits, bunch.channel, role_name(role), head);
        } else {
            spdlog::log(malformed_bunches_ % 10 == 0 ? spdlog::level::info : spdlog::level::debug,
                        "[session] {}: a bunch of {} bits on channel {} ({}) does not read: ignored ({} so far; starts {})", name(), bunch.bits, bunch.channel, role_name(role), malformed_bunches_,
                        head.substr(0, 48));
        }
        return;
    }
    note_unserved_blocks(role, *parsed, bunch);
    for (const repl::ClientRpc& rpc : parsed->rpcs) {
        ++client_calls_[rpc.call.rpc->name];   // (by the schema's own string: no allocation; the summary at the session's end adds the classes' copies of a name up)
        dispatch(conn, role, rpc, bunch, now);
    }
}

void PlayerSession::note_unserved_blocks(Role role, const repl::ClientBunch& parsed, const InBunch& bunch) {
    // Calls on a subobject the client names by path (the server never gave it a GUID: a component worldd does not replicate): the reader
    // skipped each by its bit count and read the rest of the bunch.  A client sends them every 100 ms, and one of them in the bunch of a tree's
    // start used to take the start with it (the trees' retests 3 and 4: 0 starts).  Said once per name, with the first bunch whole.
    if (parsed.in_place_blocks > 0) {
        in_place_blocks_ += parsed.in_place_blocks;
        ++in_place_bunches_;
        static const char* const kHex = "0123456789abcdef";
        for (const repl::InPlaceBlock& block : parsed.in_place) {
            const repl::InPlaceName& n = block.name;
            const std::string key = std::to_string(n.outer.ObjectId) + "/" + n.path;
            const auto seen = in_place_names_.find(key);
            if (seen != in_place_names_.end()) {
                ++seen->second;
                continue;
            }
            if (in_place_names_.size() >= kMaxInPlaceNames) continue;   // (the names beyond these are only counted)
            in_place_names_[key] = 1;
            std::string whole;
            for (size_t i = 0; i < bunch.data.size() && i < 160; ++i) {
                whole += kHex[bunch.data[i] >> 4];
                whole += kHex[bunch.data[i] & 15];
            }
            spdlog::warn("[session] {}: the client calls an RPC on a subobject of its {} that it names by path (\"{}\", outer {}/{}/{}, export flags {}, {} payload bits): worldd "
                         "replicates no such subobject, so the call is skipped and the rest of its bunch is read; said once per name; the bunch is {}, {} bits, and starts {}",
                         name(), role_name(role), n.path, n.outer.ObjectId, n.outer.ServerId, n.outer.Randomizer, static_cast<unsigned>(n.flags), block.payload_bits,
                         bunch.reliable ? "reliable" : "unreliable", bunch.bits, whole);
        }
        // (does a call named by path share a bunch with the client's other calls?  A bunch that did not read took them all away: the first few such bunches are said, with the others' names)
        if (!parsed.rpcs.empty() && ++in_place_shared_logged_ <= kMaxInPlaceSharedLogged) {
            std::string others;
            for (const repl::ClientRpc& rpc : parsed.rpcs) others += (others.empty() ? "" : ", ") + std::string(rpc.call.rpc ? rpc.call.rpc->name : "?");
            spdlog::info("[session] {}: a {} bunch of {} bits held {} calls named by path and {} others ({}): the others are read and answered as usual", name(),
                         bunch.reliable ? "reliable" : "unreliable", bunch.bits, parsed.in_place_blocks, parsed.rpcs.size(), others);
        }
        // (the running count at info on every power of ten of the blocks: the stream is ten a second)
        for (size_t cutoff = 10; cutoff <= in_place_blocks_; cutoff *= 10) {
            if (in_place_blocks_ - parsed.in_place_blocks < cutoff && in_place_blocks_ >= cutoff) {
                spdlog::info("[session] {}: {} calls on subobjects the client names by path so far, skipped ({} bunches, {} names)", name(), in_place_blocks_, in_place_bunches_,
                             in_place_names_.size());
            }
        }
    }
    if (parsed.skipped_fields.count > 0) {
        skipped_fields_ += parsed.skipped_fields.count;
        if (++skipped_fields_logged_ <= kLoggedPerKind) {
            std::string ids;
            for (const uint32_t id : parsed.skipped_fields.first) ids += (ids.empty() ? "" : ", ") + std::to_string(id);
            spdlog::warn("[session] {}: the client sent {} RPC fields on the {}'s channel that its class has no RPC for (field indices {}): skipped, the rest of the bunch is read", name(),
                         parsed.skipped_fields.count, role_name(role), ids);
        }
    }
}
// C O A + C_Y_B

const RpcTable& PlayerSession::rpc_table() {
    static const RpcTable table = [] {
        RpcTable t;
#define AOC_RPC_FEATURE(feature) PlayerSession::register_##feature##_rpcs(t);
#include "session/rpc_registered.inc"
#undef AOC_RPC_FEATURE
        return t;
    }();
    return table;
}

void PlayerSession::dispatch(Connection& conn, Role role, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now) {
    const std::string_view name_of_rpc = rpc.call.rpc->name;
    // where the call came from: the PlayerController's or the pawn's own calls, or a call on the pawn's ability component or the PlayerController's tutorial component
    std::optional<RpcScope> scope;
    if (rpc.on_actor && role == Role::PlayerController) scope = RpcScope::PlayerController;
    else if (rpc.on_actor && role == Role::Pawn) scope = RpcScope::Pawn;
    else if (!rpc.on_actor && role == Role::Pawn && rpc.object == pb_.setup().guids.ability) scope = RpcScope::AbilityComponent;
    else if (!rpc.on_actor && role == Role::PlayerController && rpc.object == pb_.setup().guids.ftue) scope = RpcScope::FtueComponent;
    if (scope) {
        if (const RpcHandler handler = rpc_table().find(*scope, name_of_rpc)) return handler(*this, RpcContext{conn, role, rpc, bunch, now});
    }
    if (unhandled_logged_.insert(std::string(name_of_rpc)).second) {
        // (info since retest 3: which calls of the client worldd answers with nothing, once each)
        spdlog::info("[session] {}: ignoring the client's {} ({}): worldd does not answer it (once per call)", name(), name_of_rpc, role_name(role));
    }
}

// The calls dispatch() answered in place before there was a table: each is a member of its own now.

/// ServerAcknowledgePossession: the client says which actor it controls now.
void PlayerSession::on_possession_ack(const repl::ClientRpc& rpc) {
    const auto it = rpc.call.params.values.find(1);
    const auto* pawn = it != rpc.call.params.values.end() ? it->second.get_if<FIntrepidNetworkGUID>() : nullptr;
    const bool its_pawn = pawn && *pawn == pb_.setup().guids.pawn;
    const bool its_mount = pawn && ride_ && mounted() && *pawn == mount_guid_;   // stage 3: the controlled actor may be the mount
    if (possession_logged_ >= kLoggedPerKind) {
        ++possession_logged_;
        spdlog::debug("[session] {}: possession ack #{} from the client ({})", name(), possession_logged_,
                      its_pawn ? "its pawn" : its_mount ? "its mount" : "not its pawn");
    } else if (its_pawn || its_mount) {
        ++possession_logged_;
        spdlog::info("[session] {}: the client acknowledged possession of its {}", name(), its_mount ? "mount" : "pawn");
    } else {
        ++possession_logged_;
        spdlog::warn("[session] {}: the client acknowledged possession of {}, not of its pawn", name(),
                     pawn ? "another object" : "nothing readable");
    }
    return;
}

/// ServerClientIsReadyToPhaseIn: the phase-in, once.
void PlayerSession::on_phase_in_ready(Connection& conn, Clock::time_point now) {
    if (phase_in_requested_ || !room_for(1)) return;
    phase_in_requested_ = true;
    spdlog::info("[session] {}: the client is ready to phase in", name());
    // The join to the phase-in, the one number the login fix is judged by (a healthy login takes about 30 s; waiting out the client's own cap takes 120 s or more).
    // The seconds run from the session's first tick, a few milliseconds after the join.
    if (scenery_opened_ > 0) {
        spdlog::info("[join] {}: phase-in after {:.1f} s (join to the client's ready call; {} town replicator{} sent before it, the last at {:.1f} s)", name(), session_seconds(now),
                     scenery_opened_, scenery_opened_ == 1 ? "" : "s", scenery_at_);
    } else {
        spdlog::info("[join] {}: phase-in after {:.1f} s (join to the client's ready call; no town replicator was sent before it)", name(), session_seconds(now));
    }
    queue("phase-in", [this](bootstrap::PlayerBootstrap& pb) {
        auto send = pb.phase_in();
        if (send) phase_in_done_ = true;
        return send;
    }, conn, /*must_build=*/true);
    send_welcome(conn);   // the first chat line, behind the phase-in (session/player_chat.cpp)
    // E1 stage 3 fix: the client's synchronized clock is set again once its loading frames are over (1 s and 5 s after the phase-in), then every
    // 120 s (retail's cadence). The client adds each frame's whole DeltaTime to the clock after a set, so the join's ClientSetTime, processed in
    // the first frame after the 3 s LoadMap, left the clock 3 s ahead: every cooldown ended 3 s early on the client (a play test of 2026-10-06).
    time_syncs_ = {now + kTimeSyncAfterPhaseIn, now + kTimeSyncSecondAfterPhaseIn};
    return;
}

void PlayerSession::end_of_datagram(Connection& conn) {
    if (!release_based_fix() && !pending_fix_) return;
    const float time_stamp = *pending_fix_;
    pending_fix_.reset();
    if (failed_) return;
    const GoodPosition& good = checker_.good();
    // (a move whose mode byte is 0, MOVE_None, is a good move like any other, but a client put back in mode 0 would not
    // walk: the correction says walking then)
    const uint8_t mode = good.movement_mode == 0 ? repl::kMoveWalking : good.movement_mode;
    auto fix = mounted() ? mount_correction(time_stamp, good) : pb_.correction(time_stamp, repl::Vec3d{good.x, good.y, good.z}, repl::Vec3d{}, mode);
    if (!fix) {
        spdlog::warn("[session] {}: the correction does not build ({})", name(), pb_.error().empty() ? "no reason" : pb_.error());
        return;
    }
    if (send(conn, *fix) == Outcome::Sent) ++corrections_;
}

int64_t PlayerSession::wall_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

double PlayerSession::sync_now(Clock::time_point now) { return bootstrap::effect_clock(pb_.setup().world_time + session_seconds(now)); }

double PlayerSession::session_seconds(Clock::time_point now) {
    if (!origin_set_) {
        origin_ = now;
        origin_set_ = true;
    }
    return std::chrono::duration<double>(now - origin_).count();
}

bool PlayerSession::gm_rpc_admitted(const char* rpc_name, const std::string& params, Clock::time_point now) {
    ++gm_rpcs_;
    const bool on = world_ && world_->gm_commands();
    // every GM RPC the client sends is logged, switch or no switch: it settles whether the shipping client's console can send them at all
    spdlog::log(level_for(gm_logged_, 100), "[gm] {}: the client sent {} ({}): {}", name(), rpc_name, log_text(params),
                on ? "answering" : "refused: GM commands are off (worldd --gm-commands, or AOC_GM_COMMANDS=1)");
    if (!on) return false;
    return gm_budget_.take(now);
}

/// The client calls of the possession acknowledgement and the phase-in (session/rpc_table.h).
void PlayerSession::register_core_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerAcknowledgePossession", [](PlayerSession& s, const RpcContext& c) { s.on_possession_ack(c.rpc); });
    t.add(RpcScope::Pawn, "ServerClientIsReadyToPhaseIn", [](PlayerSession& s, const RpcContext& c) { s.on_phase_in_ready(c.conn, c.now); });
}

}}  // namespace aoc::worldd
