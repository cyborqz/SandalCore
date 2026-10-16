// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_session.cpp
//
//  See player_session.h.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "content/content_db.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "vendor/node_tax.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <random>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

namespace {

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

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
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

// ============================================================================================================================
//  plan E1, stage 3: this player in the fights of the area
// ============================================================================================================================

/// The world's view of this session: a Fighter (where the player stands, whether it lives, the stats the formulas run on) and a PlayerPort (what NPCs do to it,
/// queued here and applied by the session's own tick, which has the connection).
class PlayerSession::CombatSide final : public combat::PlayerPort, public combat::Fighter {
public:
    explicit CombatSide(PlayerSession& s) : s_(s), aligns_(combat::CombatData::player_alignments()) {}

    // PlayerPort
    uint64_t actor_id() const override { return s_.pb_.setup().guids.pawn.ObjectId; }
    combat::Fighter& fighter() override { return *this; }
    void receive_hit(const combat::IncomingHit& hit, double) override {
        if (inbox.size() < 256) inbox.push_back(hit);
    }
    void set_in_combat(bool in_combat, double) override { wants_in_combat = in_combat; }
    std::array<uint32_t, 4> character_guid() const override { return s_.pb_.setup().character_guid; }
    void receive_kill(const combat::KillReward& kill, double) override {
        if (kills.size() < 64) kills.push_back(kill);
    }

    // Fighter
    uint64_t id() const override { return actor_id(); }
    bool is_player() const override { return true; }
    int level() const override { return s_.stats_ ? s_.stats_->level() : 1; }
    combat::Vec3 position() const override {
        const GoodPosition& g = s_.checker_.good();
        return {g.x, g.y, g.z};
    }
    double radius_cm() const override { return 34.0; }
    bool alive() const override {
        const stats::StatValue* h = s_.stats_ ? s_.stats_->find(content::kGuidHealth) : nullptr;
        return h && h->value > 0;
    }
    const std::vector<int64_t>& alignments() const override { return aligns_; }
    stats::CharacterStats& stats() override { return *s_.stats_; }
    std::vector<stats::WornItem> worn() const override { return bootstrap::worn_items_of(s_.inventory_); }

    std::vector<combat::IncomingHit> inbox;
    std::vector<combat::KillReward>  kills;
    bool                             wants_in_combat = false;

private:
    PlayerSession&       s_;
    std::vector<int64_t> aligns_;
};

PlayerSession::~PlayerSession() {
    cancel_gather(nullptr, "the session is destroyed");
    leave_combat();
}

void PlayerSession::leave_combat() {
    if (combat_side_ && npc_world_ && npc_world_->combat()) npc_world_->combat()->remove_player(combat_side_.get());
    hit_port_.reset();
    if (pipeline_) pipeline_->set_combat(nullptr);
    combat_side_.reset();
}

bool PlayerSession::own_inline_object(const repl::InlineObject& o) const {
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    return o.outer == g.pawn || o.outer == g.collision_cylinder;
}

std::vector<uint64_t> PlayerSession::actors_of(const std::vector<protocol::emit::FIntrepidNetworkGUID>& objects, const std::vector<repl::InlineObject>& inline_objects) const {
    std::vector<uint64_t> out;
    if (!npc_world_ || !npc_world_->combat()) return out;
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    const auto add = [&](uint64_t object_id) {
        const uint64_t actor = npc_world_->combat()->actor_of_object(object_id);
        if (actor != 0 && std::find(out.begin(), out.end(), actor) == out.end()) out.push_back(actor);
    };
    for (const protocol::emit::FIntrepidNetworkGUID& o : objects) {
        if (o == g.collision_cylinder || o == g.pawn) continue;   // the caster's own components
        add(o.ObjectId);
    }
    // E1 stage 3 fix: a component named in place (the client has no GUID for an NPC's CollisionCylinder: worldd never exported it) belongs to its
    // outer, the actor; the user's run named every NPC so and every cast "found nobody"
    for (const repl::InlineObject& o : inline_objects) {
        if (own_inline_object(o) || o.outer.ObjectId == 0) continue;
        add(o.outer.ObjectId);
    }
    return out;
}
// C.O.A with C | Y | B

void PlayerSession::take_hit(Connection& conn, const combat::IncomingHit& h, Clock::time_point now) {
    if (!pipeline_) return;
    // the mounts milestone, stage 3: a hit aimed at a mounted rider lands on the MOUNT's Health; the rider's own is untouched
    if (mounted()) return hit_on_mount(conn, h, now);
    pipeline_->receive_hit(h, ability_seconds(now));
    if (mounts_ && (h.flags & stats::kHitMiss) == 0) {
        for (const combat::ResolvedMod& m : h.mods) {
            if (m.stat_guid == content::kGuidHealth && m.value < 0) {
                mount_damaged(now);
                break;
            }
        }
    }
}

void PlayerSession::drain_combat(Connection& conn, Clock::time_point now) {
    if (!combat_side_ || !pipeline_) return;
    const double sec = ability_seconds(now);
    std::vector<combat::IncomingHit> hits;
    hits.swap(combat_side_->inbox);
    for (const combat::IncomingHit& h : hits) take_hit(conn, h, now);
    // the experience of the kills (a level-up sends a dozen chains: it waits for room)
    while (!combat_side_->kills.empty()) {
        if (!room_for(12 + (world_ ? world_->auto_grants().size() : 0))) break;
        const combat::KillReward k = combat_side_->kills.front();
        combat_side_->kills.erase(combat_side_->kills.begin());
        apply_kill(conn, k, now);
    }
    if (combat_side_->wants_in_combat != combat_flag_ && stats_ && stats_->started()) {
        combat_flag_ = combat_side_->wants_in_combat;
        stats_->set_in_combat(combat_flag_);
        if (abilities_) abilities_->set_in_combat(combat_flag_, sec);
        pipeline_->refresh(sec, combat_flag_ ? "combat starts" : "combat ends", true);   // the regeneration is five times faster out of combat
        if (pb_.set_in_combat(combat_flag_)) {
            queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("combat state"); }, conn, /*must_build=*/true);
        }
    }
}
// C.Y.B x C O A

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
    std::unique_ptr<PlayerSession> p(
        new PlayerSession(std::move(*built.setup), world.gamestate_mode(), character.id, world.movement(), world.position_sink(),
                          world.ability_rules(), std::move(*built.inventory), world.item_catalog(), std::move(built.character_stats), &world,
                          character.class_id, built.progress));
    p->notes_ = std::move(built.notes);
    if (built.items_repaired) {   // Gathering stage 3: the login moved resource items out of the Backpack: the record in the database is the old layout
        p->inventory_dirty_ = true;
        p->inventory_changed_ = true;
    }
    p->professions_ = std::move(built.professions);
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
        if (world.nav()) {
            p->mounts_->set_ground([w](double x, double y, double hint) -> std::optional<double> {
                const nav::AreaNav* nav = w->nav();
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
    p->pump(conn);
    if (refusal) *refusal = bootstrap::SetupRefusal::None;
    return p;
}

spdlog::level::level_enum PlayerSession::level_for(size_t& count, size_t limit) {
    return ++count <= limit ? spdlog::level::info : spdlog::level::debug;
}

/// The ground speed diagnostic: accepted moving steps (one frame to a few, 1 cm or more along the ground) are added to the
/// sprinting or the running tally; a Dodge's steps are left out.  The first moving step after a Sprint logs both averages
/// and starts both tallies again.  It shows what speed the client really moves at while sprinting: the server replicates
/// MoveSpeedMult (1 + 0.2 for the Sprint, the retail server's own packet), and the client moves at 600 cm/s times it, so
/// 720 cm/s against 600 running (retail: 780 with two purchased sprint skills).  The line is logged for the first few Sprints
/// at info, the rest at debug.
void PlayerSession::tally_speed(const GoodPosition& before, const GoodPosition& after, bool sprinting, bool evading) {
    if (!before.has_time_stamp || evading) return;
    const double seconds = static_cast<double>(after.time_stamp) - before.time_stamp;
    const double cm = std::hypot(after.x - before.x, after.y - before.y);
    if (!(seconds > 0 && seconds < 0.5) || cm < 1.0) return;
    SpeedTally& t = sprinting ? sprint_speed_ : run_speed_;
    t.cm += cm;
    t.seconds += seconds;
    if (sprinting) {
        sprint_seen_ = true;
        return;
    }
    if (!sprint_seen_ || sprint_speed_.seconds <= 0) return;
    const auto average = [](const SpeedTally& s) { return s.seconds > 0 ? s.cm / s.seconds : 0.0; };
    spdlog::log(level_for(speed_logged_),
                "[session] {}: ground speed while sprinting {:.0f} cm/s over {:.1f} s; while running (not sprinting) {:.0f} cm/s "
                "over {:.1f} s (dodges left out; 600 x the sprint's MoveSpeedMult while sprinting: 720 with no talents, 780 for a level-25 Bard)",
                name(), average(sprint_speed_), sprint_speed_.seconds, average(run_speed_), run_speed_.seconds);
    sprint_speed_ = {};
    run_speed_ = {};
    sprint_seen_ = false;
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
    last_now_ = now;
    pump(conn);
    if (finished_ || failed_) return;
    session_seconds(now);   // the clock starts at the first tick
    run_abilities(conn, now);
    run_mounts(conn, now);
    run_ride(conn, now);
    run_timers(conn, now);
    run_gather(conn, now);
    run_foliage(conn, now);
    run_nodes(conn);
    sync_item_abilities(conn);
    // E1 stage 3 fix: the client's synchronized clock, set again after the phase-in and every 120 s (see the phase-in above)
    if (phase_in_done_ && !time_syncs_.empty() && now >= time_syncs_.front() && room_for(1)) {
        time_syncs_.pop_front();
        if (time_syncs_.empty()) time_syncs_.push_back(now + kTimeSyncInterval);
        const double world_time = pb_.setup().world_time + session_seconds(now);
        ++time_syncs_sent_;
        queue("clock", [world_time](bootstrap::PlayerBootstrap& pb) { return pb.set_time(world_time); }, conn);
    }
    // plan E1: the other actors, once the join and the phase-in are out (their opens need the client's world to exist)
    if (view_ && phase_in_done_ && join_.empty() && replies_.empty() && delayed_.empty()) {
        const GoodPosition at = checker_.good();
        view_->tick(conn, at.x, at.y, now);
        if (npc_world_) npc_world_->note_player(static_cast<uint64_t>(pb_.setup().player_id), at.x, at.y);   // the NPCs near the player are awake
        if (!position_log_started_) {
            position_log_started_ = true;
            last_position_log_ = now;
        } else if (now - last_position_log_ >= kPositionLogInterval) {
            last_position_log_ = now;
            log_position(now);
        }
    }
    if (!save_clock_started_) {
        save_clock_started_ = true;
        last_save_ = now;
        return;
    }
    if (now - last_save_ < save_interval_) return;
    last_save_ = now;
    if (position_changed(/*with_yaw=*/false)) hand_over("periodic save", /*periodic=*/true);
    if (inventory_dirty_) hand_over_inventory("periodic save", /*periodic=*/true);
}

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
    spdlog::info("[session] {}: at ({:.0f}, {:.0f}, {:.0f}) cm, {:.0f} m from the login spot, movement mode {}{}; {} moves acknowledged, {} refused, {} dropped as stale{}; its view holds {} awake and {} dormant actors",
                 name(), at.x, at.y, at.z, from_login, static_cast<unsigned>(at.movement_mode), mounted() ? " (mounted)" : "", moves_acked_, moves_refused_, moves_stale_, ground,
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

uint32_t PlayerSession::mount_channel() const { return ride_ && view_ ? view_->channel_of(ride_->actor_id()) : 0; }

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
    cancel_gather(nullptr, why);   // Gathering stage 2: a node this player channels is available again
    // this player's lingering zones leave the world with it (the actor system closes them for a connection that is still there)
    if (registry_) {
        for (const auto& [id, za] : zone_actors_) registry_->remove(za.actor);
    }
    zone_actors_.clear();
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
    sink_->save_inventory(character_id_, std::move(items));
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

void PlayerSession::dispatch(Connection& conn, Role role, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now) {
    const std::string_view name_of_rpc = rpc.call.rpc->name;
    if (rpc.on_actor && role == Role::PlayerController) {
        if (name_of_rpc == "ServerAcknowledgePossession") {
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
        if (name_of_rpc == "ServerStartInteractionWithFoliage") {
            // (retest 3: the client sets its own FoliageData the moment it sends this and says "while gathering" to everything else until the server answers; an unreadable call left no line)
            if (!rpc.call.decoded && ++foliage_unreadable_logged_ <= kLoggedPerKind) {
                spdlog::warn("[session] {}: ServerStartInteractionWithFoliage arrived and its parameters do not decode: no cutting starts, and the client is told to end its own (ClientInterruptInteractionWithFoliage)", name());
            }
            return on_foliage_interact(conn, rpc, now);
        }
        if (name_of_rpc == "ServerDesummonMount") return on_desummon_mount(conn, now);
        if (name_of_rpc == "ServerDismountInteraction") return on_dismount_request(conn, now);
        if (name_of_rpc == "ServerRequestClientLoad") return on_client_load(conn, now);
        if (name_of_rpc == "ServerUpdateAccountCombatSettings") return on_combat_settings(conn, rpc, now);
        if (name_of_rpc == "ServerRequestStorageEvent") return on_storage_event_request(conn, rpc, now);
        if (name_of_rpc == "ServerSetActionbarSlotUseData") return on_set_slot(conn, rpc, now);
        if (name_of_rpc == "ServerHandleVendorPurchase") return on_vendor_purchase(conn, rpc, now);
        if (name_of_rpc == "ServerRequestLootStorageEvents") return on_loot_request(conn, rpc, now);
        if (name_of_rpc == "ServerGetNodeTaxTable") return on_node_tax_request(conn, rpc, now);
        // Nodes 1, stage 2: the node window and the login's 49 + 49 requests, and the client's node GM console
        if (name_of_rpc == "ServerRequestNodeSummary") return on_node_summary_request(rpc);
        if (name_of_rpc == "ServerRequestCurrentSiegeSummary") return on_siege_summary_request(rpc);
        if (name_of_rpc == "ServerRequestAllNodeSiegeCooldowns") return on_siege_cooldowns_request();
        if (name_of_rpc == "ServerSetEssentialCitizenshipWindowData") return on_citizenship_window_request();
        if (name_of_rpc == "ServerSetNodeLevel" || name_of_rpc == "ServerAddNodeExperience" || name_of_rpc.substr(0, 8) == "ServerGM") return on_gm_node_rpc(rpc, now);
        // the GM travel tools: the client's own GM teleports (logged always, answered only with --gm-commands)
        if (name_of_rpc == "ServerTeleportXYZ") return on_gm_teleport_xyz(conn, rpc, now);
        if (name_of_rpc == "ServerTeleport") return on_gm_teleport_key(conn, rpc, now);
        if (name_of_rpc == "ServerTeleportPlayer") return on_gm_teleport_player(conn, rpc, now);
        if (name_of_rpc == "ServerHandleVendorTransaction") {
            if (!vendor_sale_logged_) {
                vendor_sale_logged_ = true;
                spdlog::info("[session] {}: a sale to a vendor: GM vendors buy nothing, ignored (further ones are not logged)", name());
            }
            return;
        }
    } else if (rpc.on_actor && role == Role::Pawn) {
        if (name_of_rpc == "ServerClientIsReadyToPhaseIn") {
            if (phase_in_requested_ || !room_for(1)) return;
            phase_in_requested_ = true;
            spdlog::info("[session] {}: the client is ready to phase in", name());
            queue("phase-in", [this](bootstrap::PlayerBootstrap& pb) {
                auto send = pb.phase_in();
                if (send) phase_in_done_ = true;
                return send;
            }, conn, /*must_build=*/true);
            // E1 stage 3 fix: the client's synchronized clock is set again once its loading frames are over (1 s and 5 s after the phase-in), then every
            // 120 s (retail's cadence). The client adds each frame's whole DeltaTime to the clock after a set, so the join's ClientSetTime, processed in
            // the first frame after the 3 s LoadMap, left the clock 3 s ahead: every cooldown ended 3 s early on the client (the user's run of 2026-10-06).
            time_syncs_ = {now + kTimeSyncAfterPhaseIn, now + kTimeSyncSecondAfterPhaseIn};
            return;
        }
        if (name_of_rpc == "ServerRequestStorageData") return on_storage_request(conn, rpc, now);
        if (name_of_rpc == "ServerMovePacked") return on_move(conn, rpc, bunch, now, /*on_mount=*/false);
        if (name_of_rpc == "ServerItemUsed") return on_item_used(conn, rpc, now);
        if (name_of_rpc == "ServerPurchaseSkillBuild") return on_purchase(conn, rpc, now);
        if (name_of_rpc == "ServerInteractWithInteractableComponent") return on_interact(conn, rpc, now);
        if (name_of_rpc == "ServerInterruptChannel") return on_interrupt_channel(conn, now);
        if (name_of_rpc == "RequestServerRespawnChoices") return on_respawn_choices_request(conn, rpc, now);
        if (name_of_rpc == "SendServerRespawnChoice") return on_respawn_choice(conn, rpc, now);
        if (name_of_rpc == "ServerSetUseRangedWeapon") return on_set_ranged(conn, rpc, now);
        if (name_of_rpc == "ServerActivateItem") {
            // the retail client sends it with every ServerItemUsed of an item that can be worn, with the same parameters; the answer to the
            // pair is one set of events, made for ServerItemUsed
            if (!activate_logged_) {
                activate_logged_ = true;
                spdlog::debug("[session] {}: ServerActivateItem is not answered: ServerItemUsed carries the same request", name());
            }
            return;
        }
    } else if (!rpc.on_actor && role == Role::Pawn && rpc.object == pb_.setup().guids.ability) {
        if (name_of_rpc == "ServerTriggerAbility") return on_ability_trigger(conn, rpc, bunch, now);
        if (name_of_rpc == "ServerNotifyHeldAbilityCancelled") return on_ability_cancel(conn, rpc, now);
        if (name_of_rpc == "RequestHitVerification_Server") return on_hit_request(conn, rpc, now);
        if (name_of_rpc == "ServerEndAbility") return on_ability_end(conn, rpc, now);
    }
    if (unhandled_logged_.insert(std::string(name_of_rpc)).second) {
        // (info since retest 3: which calls of the client worldd answers with nothing, once each; the hunt's ServerSetActiveTarget is one)
        spdlog::info("[session] {}: ignoring the client's {} ({}): worldd does not answer it (once per call)", name(), name_of_rpc, role_name(role));
    }
}
// (C-Y-B ~ CoA)

void PlayerSession::on_move(Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount) {
    // the mounts milestone, stage 3: the controlled actor's channel is the only one that moves.  A move on the other (a pawn's while the player rides, a mount's that is not
    // ridden: a race of a few frames at a possession, or a client that lies) is dropped without a word to the client
    if (on_mount != mounted()) {
        ++moves_wrong_channel_;
        if (++ride_wrong_logged_ <= kLoggedPerKind) {
            spdlog::debug("[session] {}: a move on the {} channel while the player {}: dropped", name(), on_mount ? "mount's" : "pawn's", mounted() ? "rides" : "is on foot");
        }
        return;
    }
    std::string why;
    const auto moves = repl::read_move_packed(bunch.data.data(), bunch.data.size(), rpc.call.param_pos,
                                              rpc.call.param_pos + rpc.call.param_bits, &why);
    if (!moves) {
        if (!unreadable_move_logged_) {
            unreadable_move_logged_ = true;
            spdlog::warn("[session] {}: a ServerMovePacked does not read ({}; {} parameter bits): not acknowledged (further ones are not logged)",
                         name(), why, rpc.call.param_bits);
        }
        return;
    }

    // The older moves of the packet first, oldest first, unless they were judged already (every packet repeats the last
    // frame's move as its pending move).  Their verdicts only move the last good position; the reply is the new move's.
    // (a Dodge's dash, and plan D2's movement abilities: Flourish's slide, a Blink: the speed their window allows, for that window only; a mount has its own limit)
    if (!on_mount) scale_pawn_limits(now);   // the GM travel tools: the GM's run speed multiplier scales the pawn's horizontal limits
    const double allow = on_mount ? 0.0
                                  : std::max(dash_allowed(now) ? checker_.limits().dash_speed : 0.0,
                                             pipeline_ ? pipeline_->movement_allowance(ability_seconds(now)) : 0.0);
    const auto judge = [&](const repl::NetworkMove& m) {
        if (on_mount) {
            // the Blueprint's speed times MoveSpeedMult, now (stage 4: the 3D modes, swimming and flying, scale with it as well)
            checker_.set_max_speed(ride_->speed_limit(m.movement_mode, ability_seconds(now)));
            checker_.set_speed_mult(ride_->effective_mult(ability_seconds(now)));
        }
        return checker_.check_allowing(m, now, allow);
    };
    for (const repl::NetworkMove* older : {moves->old ? &*moves->old : nullptr, moves->pending ? &*moves->pending : nullptr}) {
        if (older && !checker_.is_stale(older->time_stamp)) judge(*older);
    }
    const repl::NetworkMove& move = moves->new_move;
    const GoodPosition before = checker_.good();
    const MoveVerdict verdict = judge(move);
    if (on_mount && ride_) {
        // what the moves of the packet accepted moved the mount too (its place in the registry, the road under it)
        const GoodPosition& g = checker_.good();
        if (g.has_time_stamp) ride_->moved(g.x, g.y, g.z, g.yaw_degrees, ability_seconds(now), g.pitch16, g.movement_mode);
    }
    if (verdict.fault == MoveFault::TimeStampNotRising) {
        // A duplicate or a datagram that arrived out of order (UE ignores such a move too): nothing to answer, nothing to
        // correct. The client is not at fault, and it is not one of the refused moves.
        // A long run of them is not out of order, though: the client's clock restarted and the checker's did not (the death of 2026-10-10 dropped 66 s of moves and the info log showed
        // nothing).  Said once per run, at info.
        if (++stale_streak_ >= kStaleStreakLog && !stale_streak_logged_) {
            stale_streak_logged_ = true;
            spdlog::info("[session] {}: {} moves in a row at a TimeStamp that does not rise (this one {}, the last good {}): dropped without a reply; the client's movement clock may have restarted without the server forgetting its own",
                         name(), stale_streak_, move.time_stamp, checker_.good().time_stamp);
        }
        if (++moves_stale_ <= kLoggedPerKind) {
            spdlog::debug("[session] {}: the move at TimeStamp {} does not rise above {}: dropped without a reply", name(), move.time_stamp,
                          checker_.good().time_stamp);
        }
        return;
    }
    stale_streak_ = 0;
    stale_streak_logged_ = false;
    if (verdict.accepted && verdict.borderline) {
        // stage 4 fix 1: flight beyond the plain law but inside the client's physics is not corrected; the numbers go to the log so a later run can tune the law
        ++moves_borderline_;
        if (moves_borderline_ <= kLoggedPerKind || moves_borderline_ % 100 == 0) {
            spdlog::info("[session] {}: the {}move at TimeStamp {} (movement mode {}) would be refused by the flight law ({:.0f} cm in {:.3f} s, the law allows {:.0f}; speed multiplier {:.2f}, view pitch {:.1f} degrees): accepted, not corrected ({} so far)",
                         name(), on_mount ? "mount's " : "", move.time_stamp, static_cast<unsigned>(move.movement_mode), verdict.step, verdict.dt, verdict.soft_allowed,
                         checker_.speed_mult(),
                         (move.control_rotation.pitch >= 32768 ? move.control_rotation.pitch - 65536.0 : move.control_rotation.pitch) * 360.0 / 65536.0, moves_borderline_);
        }
    }
    if (verdict.accepted) {
        pending_fix_.reset();   // the client is where the server has it now: an earlier refusal of this datagram needs no correction
        if (!on_mount && is_moving(before, move)) moved_at_ = now;   // Gathering stage 3: a tree is not cut while walking
        if (AbilityRuntime* abilities = ability_runtime(now)) {
            const bool moving = is_moving(before, move);
            abilities->set_moving(moving, ability_seconds(now));
            if (pipeline_) pipeline_->set_moving(moving);
            if (!on_mount) tally_speed(before, checker_.good(), abilities->sprinting(), abilities->evading());
        }
        auto ack = on_mount ? mount_ack(move.time_stamp) : pb_.good_move_ack(move.time_stamp);
        if (!ack) return;
        if (send(conn, *ack) != Outcome::Sent) return;   // the actor's channel is not open yet: the client resends its moves
        if (on_mount) ++mount_moves_acked_;
        if (++moves_acked_ == 1) spdlog::info("[session] {}: the first move is acknowledged (TimeStamp {})", name(), move.time_stamp);
        return;
    }
// C O A and Cyb

    // Not plausible: the client is put back at the last good position, measured from there again with its next move.
    ++moves_refused_;
    if (moves_refused_ == 10 || moves_refused_ == 100 || moves_refused_ == 1000 || moves_refused_ == 10000) {
        spdlog::info("[session] {}: {} moves refused so far", name(), moves_refused_);
    }
    const GoodPosition& good = checker_.good();
    const repl::Vec3d at = move.location_cm();
    const spdlog::level::level_enum level = level_for(refused_logged_);
    if (verdict.step > 0) {
        spdlog::log(level, "[session] {}: the {}move at TimeStamp {} (movement mode {}) is refused ({}: {:.0f} cm in {:.3f} s, at most {:.0f}): put back at ({:.1f}, {:.1f}, {:.1f}) cm",
                    name(), on_mount ? "mount's " : "", move.time_stamp, static_cast<unsigned>(move.movement_mode), fault_name(verdict.fault), verdict.step, verdict.dt,
                    verdict.allowed, good.x, good.y, good.z);
    } else {
        spdlog::log(level, "[session] {}: the {}move at TimeStamp {} (movement mode {}) to ({:.1f}, {:.1f}, {:.1f}) cm is refused ({}): put back at ({:.1f}, {:.1f}, {:.1f}) cm",
                    name(), on_mount ? "mount's " : "", move.time_stamp, static_cast<unsigned>(move.movement_mode), at.x, at.y, at.z, fault_name(verdict.fault), good.x, good.y,
                    good.z);
    }
    // The correction goes out once the datagram's bunches are all judged (end_of_datagram), the latest refusal's.
    // (a TimeStamp that is NaN or infinite names no saved move: the client ignores it, so 0 goes out instead)
    pending_fix_ = std::isfinite(move.time_stamp) ? move.time_stamp : 0.0f;
}

void PlayerSession::end_of_datagram(Connection& conn) {
    if (!pending_fix_) return;
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

// ============================================================================================================================
//  abilities (plan A7)
// ============================================================================================================================

AbilityRuntime* PlayerSession::ability_runtime(Clock::time_point now) {
    if (abilities_failed_ || finished_ || failed_ || !ability_rules_) return nullptr;   // a finished session answers nothing more
    if (!abilities_) {
        // The stats the runtime reads or sets, from the pawn's own arrays as they are now (the join's values and the equips' changes since).
        std::map<int64_t, double> stats;
        for (const auto& [record, value] : pb_.current_stats()) stats[record] = value;
        session_seconds(now);
        abilities_origin_ = origin_;
        abilities_.emplace(*ability_rules_, std::move(stats), pb_.setup().world_time);
        if (!abilities_->ok()) {
            spdlog::error("[session] {}: the ability runtime cannot start: {}", name(), abilities_->error());
            abilities_failed_ = true;
            abilities_.reset();
            return nullptr;
        }
        // plan D2: the generic runtime beside it (Health and Mana's regeneration runs there even without the ability tables)
        if (stats_ && stats_->started()) {
            pipeline_.emplace(world_ ? world_->ability_catalog() : nullptr, *stats_, *abilities_,
                              [this] { return bootstrap::worn_items_of(inventory_); }, pb_.setup().world_time, static_cast<uint64_t>(pb_.setup().player_id) * 7919u + 1u);
            pipeline_->set_known(known_);
            sync_ability_values();
            if (npc_world_ && npc_world_->combat()) {
                combat_side_ = std::make_shared<CombatSide>(*this);
                // the runtime's clock is the session's (0 at the join); the combat world's and the aftermath's is the server's world time: the port
                // translates (the join's world time is the runtime's zero) [E1 stage 3 fix 2: the corpses closed and respawned at the next tick]
                hit_port_ = std::make_unique<combat::WorldHitPort>(*npc_world_->combat(), npc_world_->registry(), *combat_side_, pb_.setup().world_time);
                pipeline_->set_combat(hit_port_.get());
                npc_world_->combat()->add_player(combat_side_.get());
            }
            if (world_ && world_->ability_catalog()) {
                spdlog::info("[session] {}: the generic ability runtime is on ({} abilities known, {} in the tables)", name(), known_.size(),
                             world_->ability_catalog()->abilities());
            }
        }
    }
    return &*abilities_;
}

double PlayerSession::ability_seconds(Clock::time_point now) const {
    return std::chrono::duration<double>(now - abilities_origin_).count();
}

bool PlayerSession::dash_allowed(Clock::time_point now) const {
    return abilities_ && abilities_->dash_allowed(ability_seconds(now));
}

void PlayerSession::abilities_off(const std::string& why) {
    abilities_failed_ = true;
    pending_ends_.clear();
    // Nothing more is sent, so the client keeps whatever it was last told, never ended or removed: what the pawn lists is that (a
    // Sprint's effect and MoveSpeedMult stay until the client logs in again).
    std::string running;
    if (abilities_) {
        const auto add = [&](bool on, const char* what) {
            if (on) running += (running.empty() ? "" : ", ") + std::string(what);
        };
        add(abilities_->sprinting(), "a Sprint");
        add(abilities_->resting() || abilities_->rest_effect_on(), "a Rest");
        add(abilities_->evading(), "the Evasion");
    }
    spdlog::warn("[session] {}: abilities are off for this session ({}): the pawn lists {} ability item(s), {} effect(s) and {} cooldown(s) that are never "
                 "ended or removed now{}",
                 name(), why, pb_.listed_abilities(), pb_.listed_effects(), pb_.listed_cooldowns(),
                 running.empty() ? "" : "; the runtime has running: " + running);
}

bool PlayerSession::is_moving(const GoodPosition& before, const repl::NetworkMove& move) const {
    if (move.input_x() != 0 || move.input_y() != 0) return true;   // the player holds a direction
    if (!before.has_time_stamp) return false;
    const double dt = static_cast<double>(move.time_stamp) - static_cast<double>(before.time_stamp);
    if (!(dt > 0)) return false;
    const repl::Vec3d at = move.location_cm();
    return std::hypot(at.x - before.x, at.y - before.y) / dt > kMovingSpeed;   // or it is going somewhere
}

void PlayerSession::take_ability_events() {
    AbilityRuntime& runtime = *abilities_;
    std::vector<AbilityEvent> events = runtime.take_events();
    if (pipeline_) {
        std::vector<AbilityEvent> more = pipeline_->take_events();
        events.insert(events.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    }
    for (AbilityEvent& e : events) {
        if (!e.change.empty()) {
            if (!pb_.apply_ability_change(e.change)) {
                abilities_off("the pawn's state does not take the ability change (" + e.note + ": " +
                              (pb_.error().empty() ? std::string("its items and the runtime's keys disagree") : pb_.error()) + ")");
                return;
            }
            abilities_dirty_ = true;
        }
        for (bootstrap::HitConfirm& h : e.confirmations) {
            if (pending_confirms_.size() >= kMaxPendingConfirms) pending_confirms_.pop_front();
            pending_confirms_.push_back(std::move(h));
        }
        if (e.end_ability) pending_ends_.push_back(*e.end_ability);
        for (const bootstrap::ZoneOp& z : e.zones) {
            if (zone_ops_.size() < kMaxZoneOps) zone_ops_.push_back(z);
        }
        // State changes (an ability starts or ends, a refusal) are what an in-game run reads; Stamina's steps are debug.  A client
        // can cause them as often as its budget allows: the first few of each kind at info, the rest at debug.
        const bool transition = e.end_ability || !e.confirmations.empty() || !e.zones.empty() || !e.change.abilities_added.empty() || !e.change.abilities_removed.empty() ||
                                !e.change.abilities_changed.empty() || !e.change.effects_added.empty() || !e.change.effects_removed.empty() ||
                                !e.change.effects_changed.empty() || !e.change.cooldowns_set.empty() || !e.change.cooldowns_removed.empty();
        const bool refusal = e.refusal;
        const spdlog::level::level_enum level = refusal      ? level_for(ability_logged_)
                                                : transition ? level_for(ability_transitions_logged_, kLoggedAbilityTransitions)
                                                             : spdlog::level::debug;
        spdlog::log(level, "[session] {}: {} (Stamina {:.2f}){}", name(), e.note, runtime.stamina(),
                    e.end_ability ? ", ClientEndAbility " + std::to_string(*e.end_ability) : std::string());
    }
}
// == C:Y:B | CoA ==

/// The lingering zones' actors (plan D2's dance effect): each zone the ability runtime opens is an actor of the world's registry (plan E1), owned by this
/// player: the actor system opens it on a channel of its own, sends the bFadingOut update and closes it with Destroyed, as the retail server does for a
/// dance's visual effect.  The session only builds the actor, adds it, changes it and removes it.
void PlayerSession::flush_zones(Connection& conn) {
    (void)conn;
    const auto forget = [&](int64_t id) {
        const auto it = zone_actors_.find(id);
        if (it == zone_actors_.end()) return;
        if (registry_) registry_->remove(it->second.actor);
        pb_.lingering_remove(it->second.actor);
        zone_actors_.erase(it);
    };
    while (!zone_ops_.empty()) {
        const bootstrap::ZoneOp op = zone_ops_.front();
        zone_ops_.pop_front();
        const auto existing = zone_actors_.find(op.id);
        if (op.kind == bootstrap::ZoneOp::Kind::Open) {
            if (!world_ || !registry_ || existing != zone_actors_.end()) continue;
            bootstrap::LingeringSpec spec;
            if (!pawn_mesh_) pawn_mesh_ = world_->mint_guid();
            spec.actor = world_->mint_guid();
            spec.ability = world_->mint_guid();
            spec.stats = world_->mint_guid();
            spec.movement = world_->mint_guid();
            spec.pawn_mesh = *pawn_mesh_;
            spec.zone = op.zone;
            spec.source = op.ability;
            const GoodPosition at = checker_.good();
            spec.x = at.x;
            spec.y = at.y;
            spec.z = at.z;
            spec.yaw_degrees = at.yaw_degrees;
            if (zone_random_ == 0) zone_random_ = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(pb_.setup().player_id) * 0xBF58476D1CE4E5B9ull) ^ 1;
            const auto next_word = [&] {   // SplitMix64: the FGuids of an actor are random words
                zone_random_ += 0x9E3779B97F4A7C15ull;
                uint64_t z = zone_random_;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
            };
            for (int i = 0; i < 4; ++i) {
                spec.effect_guid[i] = next_word();
                spec.instance_guid[i] = next_word();
            }
            auto actor = pb_.lingering_actor_of(spec);
            if (!actor) {
                spdlog::warn("[session] {}: the zone actor of ability {} cannot be built: dropped", name(), op.ability);
                continue;
            }
            world::WorldActor w;
            w.id = spec.actor.ObjectId;
            w.kind = world::ActorKind::Effect;
            w.actor = std::move(*actor);
            w.x = at.x;
            w.y = at.y;
            w.z = at.z;
            w.yaw_degrees = at.yaw_degrees;
            w.never_dormant = true;
            w.owner = static_cast<uint64_t>(pb_.setup().player_id);
            w.npc.source = world::Source::Runtime;
            w.open_exports.push_back(pb_.lingering_mesh_export(*pawn_mesh_));
            if (!registry_->add(std::move(w))) {
                spdlog::warn("[session] {}: the registry does not take the zone actor of ability {}: dropped", name(), op.ability);
                continue;
            }
            ZoneActor& za = zone_actors_[op.id];
            za.actor = spec.actor.ObjectId;
            ++zone_opened_;
            continue;
        }
        if (existing == zone_actors_.end()) continue;   // an actor that was never made has nothing to fade or close
        if (op.kind == bootstrap::ZoneOp::Kind::Close) {
            ++zone_closed_;
            spdlog::log(level_for(zone_logged_), "[session] {}: the zone actor is removed from the world (the client's channel is closed, destroyed)", name());
            forget(op.id);
            continue;
        }
        registry_->modify(existing->second.actor, world::Change::State, [](world::WorldActor& a) {
            a.actor.obj.state.set(*a.actor.obj.schema, "bFadingOut", repl::Value::boolean(true));
        });
    }
}
// ~ C | Y | B & C-O-A ~

void PlayerSession::flush_abilities(Connection& conn) {
    if (abilities_failed_ || failed_ || !join_.empty()) return;   // the join's chains go first: the pawn's second state is theirs
    flush_zones(conn);
    if (blocked_ && conn.unacked_reliable(blocked_channel_) >= blocked_unacked_) return;
    if (abilities_dirty_) {
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        bool build_failed = false;
        std::optional<bootstrap::Send> update = pb_.ability_update(&build_failed);
        if (!update) {
            // Either the changes since the last update came to nothing (an effect added and removed), or the update does not
            // build: the shadow is as it was, so the next change sends both, and the cause is logged (a few times).
            if (build_failed) {
                spdlog::log(level_for(ability_build_logged_) == spdlog::level::info ? spdlog::level::warn : spdlog::level::debug,
                            "[session] {}: an ability update does not build ({}): it is sent with the next change, if it builds then", name(),
                            pb_.error().empty() ? "no reason" : pb_.error());
            }
            abilities_dirty_ = false;
        } else {
            switch (send(conn, *update)) {
                case Outcome::Sent:
                    abilities_dirty_ = false;
                    ++ability_updates_;
                    break;
                case Outcome::Wait:
                    pb_.restore(checkpoint);
                    return;
                case Outcome::Refused:
                    pb_.restore(checkpoint);
                    spdlog::error("[session] {}: the ability update was refused by the connection", name());
                    abilities_off("the connection refused an ability update");
                    return;
            }
        }
    }
    while (!pending_ends_.empty()) {
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        std::optional<bootstrap::Send> end = pb_.end_ability(pending_ends_.front());
        if (!end) {
            spdlog::warn("[session] {}: ClientEndAbility({}) does not build: dropped", name(), pending_ends_.front());
            pending_ends_.pop_front();
            continue;
        }
        switch (send(conn, *end)) {
            case Outcome::Sent:
                ++ability_ends_;
                pending_ends_.pop_front();
                break;
            case Outcome::Wait:
                pb_.restore(checkpoint);
                return;
            case Outcome::Refused:
                pb_.restore(checkpoint);
                spdlog::error("[session] {}: ClientEndAbility({}) was refused by the connection: dropped", name(), pending_ends_.front());
                pending_ends_.pop_front();
                break;
        }
    }
    // the confirmations of the hits (the client's floating numbers), after the update that holds their effect: up to 20 hits a call, as retail's
    while (!pending_confirms_.empty()) {
        std::vector<bootstrap::HitConfirm> batch;
        const size_t n = std::min(pending_confirms_.size(), kConfirmsPerCall);
        for (size_t i = 0; i < n; ++i) batch.push_back(pending_confirms_[i]);
        const GoodPosition at = checker_.good();
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        std::optional<bootstrap::Send> send_it = pb_.hit_confirmations(batch, repl::Vec3d{at.x, at.y, at.z});
        if (!send_it) {
            spdlog::warn("[session] {}: {} hit confirmation(s) do not build: dropped", name(), n);
            pending_confirms_.erase(pending_confirms_.begin(), pending_confirms_.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        switch (send(conn, *send_it)) {
            case Outcome::Sent:
                confirms_sent_ += n;
                pending_confirms_.erase(pending_confirms_.begin(), pending_confirms_.begin() + static_cast<std::ptrdiff_t>(n));
                break;
            case Outcome::Wait:
                pb_.restore(checkpoint);
                return;
            case Outcome::Refused:
                pb_.restore(checkpoint);
                spdlog::error("[session] {}: the hit confirmations were refused by the connection: dropped", name());
                pending_confirms_.erase(pending_confirms_.begin(), pending_confirms_.begin() + static_cast<std::ptrdiff_t>(n));
                break;
        }
    }
}

void PlayerSession::run_abilities(Connection& conn, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime) return;
    runtime->tick(ability_seconds(now));
    if (pipeline_) {
        pipeline_->tick(ability_seconds(now));
        drain_combat(conn, now);
    }
    take_ability_events();
    check_death(conn, now);
    flush_abilities(conn);
}
// (CoA : C.Y.B)

void PlayerSession::on_ability_trigger(Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime) return;
    if (!within_budget(ability_budget_, "ServerTriggerAbility", now)) {   // (a dead character's triggers take the budget too: each costs a reliable answer)
        ++ability_dropped_;
        return;
    }
    if (dead_) {
        // a dead character casts nothing: the instance is ended (ClientEndAbility), whichever runtime would have run it
        if (const auto t = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos, rpc.call.param_pos + rpc.call.param_bits)) {
            end_instance(conn, t->client_instance_id, on_mount);
        }
        return;
    }
    if (on_mount && !mounted()) {
        // a trigger on a mount nobody rides (a race at a dismount, or a client that lies): the cast is ended where it was started
        if (const auto t = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos, rpc.call.param_pos + rpc.call.param_bits)) {
            ++mount_triggers_refused_;
            end_instance(conn, t->client_instance_id, true);
        }
        return;
    }
    ++ability_triggers_;
    cancel_gather(&conn, "an ability was triggered");   // Gathering stage 2: a new ability ends the channel [modelled: no capture]
    int64_t ability = 0;
    int32_t instance = 0;
    int32_t item_slot = -1;
    uint32_t item_location = 0;
    TriggerInfo info;
    if (const auto t = repl::parse_trigger_ability(rpc.call)) {
        ability = t->ability_guid;
        instance = t->client_instance_id;
        item_slot = t->item_slot;
        item_location = t->item_location;
        info.animation_index = t->animation_index;
        info.input_x = t->input_x;
        info.input_y = t->input_y;
        info.target_count = t->target_count;
        info.target_actors = actors_of(t->targets, t->inline_targets);
        info.item = t->item_slot != -1;
    } else if (const auto prefix = repl::read_trigger_prefix(bunch.data.data(), bunch.data.size(), rpc.call.param_pos,
                                                             rpc.call.param_pos + rpc.call.param_bits)) {
        // The call does not read whole, but its first two parameters do: that is enough to refuse what worldd does not run.
        // An ability it does run is not started from half a call.
        if (runtime->enabled(prefix->ability_guid) || (pipeline_ && pipeline_->handles(prefix->ability_guid))) {
            if (++ability_unreadable_logged_ <= kLoggedPerKind) {
                spdlog::warn("[session] {}: a ServerTriggerAbility for an ability worldd runs does not read ({} parameter bits): ignored", name(),
                             rpc.call.param_bits);
            }
            return;
        }
        ability = prefix->ability_guid;
        instance = prefix->client_instance_id;
    } else {
        if (++ability_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[session] {}: a ServerTriggerAbility does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    // the mounts milestone, stage 3: what a rider may do, and the mount's dash
    if (mounted()) {
        if (!mounted_trigger(conn, ability, instance, on_mount, now)) return;
    } else if (mounts_ && ability != 0) {
        // the dash is the mount's: nobody rides, so it is not run on the pawn (the generic runtime has the ability: it would put its effect on the character)
        const content::MountAbilityRow* row = mounts_->book().data().ability(ability);
        if (row && row->role == "dash") {
            ++mount_triggers_refused_;
            end_instance(conn, instance, false);
            return;
        }
    }
    // the mounts milestone: Summon_Mount is the mount controller's (the pipeline refuses an ability that names an item)
    if (mounts_ && ability != 0 && ability == mounts_->book().summon_ability()) return on_summon_trigger(conn, instance, item_slot, item_location, now);
    if (pipeline_ && pipeline_->handles(ability)) {
        pipeline_->trigger(ability, instance, ability_seconds(now), info);
    } else {
        runtime->trigger(ability, instance, ability_seconds(now), info);
    }
    take_ability_events();
    flush_abilities(conn);
}
// C_Y_B x CoA

void PlayerSession::on_ability_cancel(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime) return;
    if (!within_budget(cancel_budget_, "ServerNotifyHeldAbilityCancelled", now)) {
        ++cancels_dropped_;
        return;
    }
    ++cancels_;
    const auto cancel = repl::parse_held_ability_cancelled(rpc.call);
    if (!cancel) return;
    if (!runtime->cancel(cancel->client_instance_id, ability_seconds(now)) && pipeline_) pipeline_->cancel(cancel->client_instance_id, ability_seconds(now));
    take_ability_events();
    flush_abilities(conn);
}

void PlayerSession::teach_ability(int64_t ability) {
    if (std::find(known_.begin(), known_.end(), ability) == known_.end()) known_.push_back(ability);
    if (pipeline_) pipeline_->set_known(known_);
}

void PlayerSession::on_ability_end(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return;
    if (!within_budget(cancel_budget_, "ServerEndAbility", now)) {
        ++cancels_dropped_;
        return;
    }
    const auto id = repl::parse_end_ability(rpc.call);
    if (!id) return;
    pipeline_->end_request(*id, ability_seconds(now));
    take_ability_events();
    flush_abilities(conn);
}

double PlayerSession::take_damage(Connection& conn, double amount, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return 0;
    if (mounted()) {
        // a rider's damage lands on its mount (stage 3); nothing reaches the rider's Health
        if (amount > 0 && ride_) {
            const Ride::Hurt hurt = ride_->hurt(amount);
            ++mount_hits_;
            if (hurt.died) mount_died(conn, now);
        }
        return 0;
    }
    if (amount > 0) mount_damaged(now);
    const double through = pipeline_->take_damage(amount, ability_seconds(now));
    take_ability_events();
    flush_abilities(conn);
    return through;
}

void PlayerSession::on_hit_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return;
    if (!within_budget(hit_budget_, "RequestHitVerification_Server", now)) {
        ++hit_dropped_;
        return;
    }
    ++hit_requests_;
    if (dead_) return;
    const auto req = repl::parse_hit_request(rpc.call);
    if (!req) {
        if (++hit_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a RequestHitVerification_Server does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    // what the client says it hit: the caster's own components (its pawn, its collision cylinder, by GUID or named in place) and the NPCs' (named
    // in place: the client has no GUID for a component worldd never exported; E1 stage 3 fix)
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    size_t own = 0;
    std::vector<protocol::emit::FIntrepidNetworkGUID> objects;
    std::vector<repl::InlineObject> inline_objects;
    for (const repl::HitComponentRef& c : req->components) {
        if (c.inline_object) {
            if (own_inline_object(*c.inline_object)) ++own;
            else inline_objects.push_back(*c.inline_object);
        } else if (c.object == g.collision_cylinder || c.object == g.pawn) {
            ++own;
        } else {
            objects.push_back(c.object);
        }
    }
    const AbilityPipeline::HitVerdict verdict = pipeline_->hit_request(req->ability_guid, req->targeting_guid, req->hit_guid, req->components.size(), own,
                                                                       ability_seconds(now), actors_of(objects, inline_objects));
    if (verdict != AbilityPipeline::HitVerdict::Scheduled && verdict != AbilityPipeline::HitVerdict::Quiet) {
        if (++hit_unserved_logged_ <= kLoggedPerKind) {
            spdlog::info("[session] {}: a hit request for {} / {} is {}", name(), req->ability_guid, req->hit_guid, AbilityPipeline::verdict_name(verdict));
        }
    }
    take_ability_events();
    flush_abilities(conn);
}

// ============================================================================================================================
//  items (plan B2)
// ============================================================================================================================

std::vector<bootstrap::ItemEvent> item_events_for(const items::Outcome& outcome) {
    std::vector<bootstrap::ItemEvent> out;
    out.reserve(outcome.events.size());
    for (const items::Event& e : outcome.events) {
        bootstrap::ItemEvent ev;
        ev.type = e.type;
        ev.storage = bootstrap::item_storage_of(e.container);
        ev.slot = e.slot;
        ev.item = items::to_instance(e.item);
        ev.instigated = e.instigated;
        out.push_back(std::move(ev));
    }
    return out;
}
// ~ C Y B - C-O-A ~

namespace {

/// A catalog that knows no record: a session without one routes every grant to the Backpack.
struct NoCatalog : items::ItemCatalog {
    std::optional<items::ItemInfo> info(int64_t) const override { return std::nullopt; }
};

std::optional<svc::ItemContainer> container_of_location(uint8_t location) {
    if (location == static_cast<uint8_t>(repl::ItemLocation::Backpack)) return svc::ItemContainer::Backpack;
    if (location == static_cast<uint8_t>(repl::ItemLocation::Equipment)) return svc::ItemContainer::Equipment;
    return std::nullopt;
}

}  // namespace

const items::ItemCatalog& PlayerSession::item_catalog() const {
    static const NoCatalog none;
    return catalog_ ? *catalog_ : none;
}

// ============================================================================================================================
//  mounts (the mounts milestone, stage 2: session/mounts.h has the rules and the numbers)
// ============================================================================================================================

void PlayerSession::on_mount_item_used(const svc::InventoryItem& item) {
    ++mount_item_uses_;
    const content::MountRow* row = mounts_ ? mounts_->book().find(item.item_record_id) : nullptr;
    spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: a mount item is used (item {}, {}, a {} mount activated by {}): its summon or desummon follows as the next call, nothing is consumed", name(),
                item.item_record_id, row ? row->item_name : std::string("?"), row ? row->kind : std::string("?"), row ? row->activation : std::string("?"));
}

void PlayerSession::on_summon_trigger(Connection& conn, int32_t client_instance, int32_t slot, uint32_t location, Clock::time_point now) {
    ++mount_summon_requests_;
    const auto ignore = [&](const char* why) {
        ++mount_ignored_;
        spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: a Summon_Mount is ignored: {}", name(), why);
        if (pending_ends_.size() < 64) pending_ends_.push_back(client_instance);   // the client's own instance of the cast is ended
        flush_abilities(conn);
    };
    if (!mounts_) return ignore("worldd has no mounts");
    const auto container = container_of_location(static_cast<uint8_t>(location));
    if (!container || *container != svc::ItemContainer::Backpack || slot < 0) return ignore("the call names no item of the bag");
    const svc::InventoryItem* item = inventory_.at(*container, slot);
    if (!item || !mounts_->is_mount_item(item->item_record_id)) return ignore("the slot holds no mount item");
    const auto guid = items::guid_of(item->instance_guid);
    if (!guid) return ignore("the item has no readable instance guid");
    ability_runtime(now);   // the clock the mount runs on starts with the ability runtime's
    if (!abilities_) return ignore("abilities are off");
    // a mount item used while its owner rides: the rider gets off first (one mount per player: the old one is desummoned and nothing is summoned)
    if (mounted()) dismount(conn, now, DismountWhy::Summon);
    MountController::Request r;
    r.item_record = item->item_record_id;
    r.item_guid = *guid;
    r.client_instance = client_instance;
    r.lock_expiration = item->lock_expiration;
    r.now_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const GoodPosition at = checker_.good();
    r.place = {at.x, at.y, at.z, at.yaw_degrees};
    // the mount inherits its owner's alignments as the pawn holds them now (stage 3 fix 2: never a donor's: a PvP flag, a node or a guild the owner has not)
    if (const repl::Subobject* alignment = pb_.pawn().find(pb_.setup().guids.alignment)) {
        if (const repl::Value* v = alignment->obj.state.get(*alignment->obj.schema, "Alignments")) {
            if (const auto* c = v->get_if<repl::AlignmentContainer>()) r.alignments = *c;
        }
    }
    r.owner_level = progress_.level;
    r.dead = dead_;
    // the mount comes back with the Health its item kept (a dead mount's is 0: the lock keeps it out; once the lock is over it comes back whole)
    if (item->persisted_health && *item->persisted_health > 0) r.health = static_cast<double>(*item->persisted_health);
    const double t = ability_seconds(now);
    r.world_time = pb_.setup().world_time + t;
    switch (mounts_->summon(r, t)) {
        case MountController::Outcome::Started:
            spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: Summon_Mount: the mount of item {} is summoned (the ability item and the bar in {:.0f} ms, the mount in {:.0f} ms)", name(),
                        r.item_record, MountController::kItemDelay * 1000, MountController::kOpenDelay * 1000);
            break;
        case MountController::Outcome::Refused:
            spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: Summon_Mount is refused (no ground in front of the player, a mount under lock or a dead player): \"{}\"", name(),
                        MountController::kRefusalText);
            break;
        case MountController::Outcome::Desummoning:
            spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: Summon_Mount while a mount is out: the old mount is desummoned and nothing is summoned", name());
            if (pending_ends_.size() < 64) pending_ends_.push_back(client_instance);
            flush_abilities(conn);
            break;
        case MountController::Outcome::Ignored:
            ++mount_ignored_;
            spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: Summon_Mount is ignored: a summon or a desummon is in progress", name());
            if (pending_ends_.size() < 64) pending_ends_.push_back(client_instance);
            flush_abilities(conn);
            break;
    }
    run_mounts(conn, now);
}
// [Cyb x C.O.A]

void PlayerSession::on_desummon_mount(Connection& conn, Clock::time_point now) {
    ++mount_desummon_requests_;
    if (!mounts_) return;
    if (!within_budget(item_budget_, "ServerDesummonMount", now)) return;
    ability_runtime(now);
    if (!abilities_) return;
    // a rider dismounts first.  The client asks for the dismount itself too (ServerDismountInteraction in the same packet): the two requests make two ClientRestarts [R: 893]
    if (mounted()) {
        dismount(conn, now, DismountWhy::Desummon);
    } else if (dismounted_valid_ && dismounted_at_ == now && !dismount_extra_sent_ && room_for(1)) {
        dismount_extra_sent_ = true;
        queue("ClientRestart (the own pawn) again", [](bootstrap::PlayerBootstrap& pb) { return pb.restart_own_pawn(1); }, conn, /*must_build=*/true);
    }
    if (mounts_->desummon(ability_seconds(now))) {
        spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: ServerDesummonMount: the mount dissolves and runs off, and is gone in {:.1f} s", name(), MountController::kDesummonClose);
    } else {
        ++mount_ignored_;
        spdlog::debug("[session] {}: ServerDesummonMount: no mount to desummon", name());
    }
    run_mounts(conn, now);
}

void PlayerSession::mount_damaged(Clock::time_point now) {
    if (!mounts_ || !abilities_) return;
    mounts_->on_damage(ability_seconds(now));
}

void PlayerSession::run_mounts(Connection& conn, Clock::time_point now) {
    if (!mounts_ || !abilities_) return;
    // stage 4, modelled: a mount left farther than MountDesummonDistanceCentimeters from its owner is desummoned (the Blueprints turn bEnableMountDistanceDesummon on; no data
    // or capture has the distance).  A server-side move of the player (a respawn: the player died, which desummons anyway) shows up the same way.
    if (!mounted() && mounts_->phase() == MountController::Phase::Out) {
        if (const world::WorldActor* m = mounts_->mount()) {
            const GoodPosition at = checker_.good();
            if (std::hypot(m->x - at.x, m->y - at.y) > desummon_distance_) {
                ++distance_desummons_;
                spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: the mount is {:.0f} cm from its owner (more than {:.0f}): it is desummoned", name(), std::hypot(m->x - at.x, m->y - at.y),
                            desummon_distance_);
                mounts_->desummon(ability_seconds(now));
            }
        }
    }
    MountController::Effects fx = mounts_->advance(ability_seconds(now));
    if (!fx.empty()) apply_mount_effects(conn, std::move(fx));
}

void PlayerSession::apply_mount_effects(Connection& conn, MountController::Effects&& fx) {
    // the pawn's ability item first (flushed with the next ability update), then what the PlayerController is told: retail's order in one packet
    if (!fx.ability.empty()) {
        if (pb_.apply_ability_change(fx.ability)) {
            abilities_dirty_ = true;
        } else {
            spdlog::warn("[session] {}: the Summon_Mount ability item cannot be applied to the pawn ({})", name(), pb_.error().empty() ? "no reason" : pb_.error());
        }
    }
    for (const int32_t id : fx.ends) {
        if (pending_ends_.size() < 64) pending_ends_.push_back(id);
    }
    flush_abilities(conn);
    if (fx.pet_changed && world_ && room_for(1)) {
        // The pawn's InteractInfo.MountablePet names the summoned mount (and 0 once it has closed), as retail's does in the packet of the Summon_Mount item [R: 871 packet 210,
        // 898 packets 158 and 601].  The client's bank-18 Mount slot, the dash, is resolved through it (exe: GetActionbarInstigator 0x6aba180 -> GetMountablePet): without it the
        // client sends nothing when the dash is pressed (stage 3 fix 3: the user's "the dash can't be used", worldd's log with no dash trigger at all).
        if (!interact_info_) interact_info_ = world_->mint_guid();
        const protocol::emit::FIntrepidNetworkGUID info = *interact_info_;
        const protocol::emit::FIntrepidNetworkGUID pet = fx.pet;
        spdlog::log(level_for(mount_logged_, kLoggedMounts), "[session] {}: the pawn's mountable pet {} (InteractInfo.MountablePet: what the client's dash button needs)", name(),
                    pet.ObjectId != 0 ? "is the mount (actor " + std::to_string(pet.ObjectId) + ")" : std::string("is none: the mount is gone"));
        queue("the pawn's mountable pet", [info, pet](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.ensure_interact_info(info)) return std::nullopt;
            return pb.pawn_mountable_pet(pet);
        }, conn, /*must_build=*/true);
    }
    if (fx.bar && room_for(1)) {
        const int64_t dash = fx.bar_ability;
        const int32_t slot = fx.bar_slot;
        queue("the mount action bar", [dash, slot](bootstrap::PlayerBootstrap& pb) { return pb.mount_bar(dash, slot); }, conn);
    }
    if (fx.persist) apply_persist(&conn, *fx.persist);
    for (const MountController::Callout& c : fx.callouts) {
        if (!room_for(1)) break;
        const int64_t unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const int64_t ticks = items::ticks_of_ms(unix_ms);
        const std::string text = c.text;
        const uint64_t key = c.key;
        queue("a callout", [text, ticks, key](bootstrap::PlayerBootstrap& pb) {
            return pb.callout_add(text, MountController::kCalloutType, MountController::kCalloutPointSize, ticks, key);
        }, conn);
    }
    for (const uint64_t key : fx.callouts_removed) {
        if (!room_for(1)) break;
        queue("a callout is taken off", [key](bootstrap::PlayerBootstrap& pb) { return pb.callout_remove(key); }, conn);
    }
}
// CoA x C:Y:B

// ============================================================================================================================
//  riding (the mounts milestone, stage 3: session/riding.h has the rules and the numbers)
// ============================================================================================================================

void PlayerSession::on_mount_bunch(Connection& conn, const InBunch& bunch, Clock::time_point now) {
    if (bunch.close || bunch.bits == 0 || !registry_ || !ride_) return;
    const world::WorldActor* m = registry_->find(ride_->actor_id());
    if (!m || !m->actor.obj.schema) return;
    const repl::Actor* actor = &m->actor;
    const auto resolve = [actor](const FIntrepidNetworkGUID& g) -> const schema::ClassSchema* {
        const repl::Subobject* sub = actor->find(g);
        return sub ? sub->obj.schema : nullptr;
    };
    repl::CodecOptions opts;
    opts.inline_objects = false;   // a rider's trigger names no target by path (the dash's Targets are the default)
    const auto parsed = repl::read_client_bunch(*m->actor.obj.schema, resolve, bunch.data.data(), bunch.bits, opts);
    if (!parsed) {
        if (++malformed_bunches_ <= 3) {
            spdlog::warn("[session] {}: a bunch of {} bits on the mount's channel {} does not read: ignored", name(), bunch.bits, bunch.channel);
        }
        return;
    }
    for (const repl::ClientRpc& rpc : parsed->rpcs) dispatch_mount(conn, rpc, bunch, now);
}

void PlayerSession::dispatch_mount(Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now) {
    const std::string_view n = rpc.call.rpc->name;
    if (rpc.on_actor) {
        if (n == "ServerMovePacked") return on_move(conn, rpc, bunch, now, /*on_mount=*/true);
    } else if (rpc.object == mount_ability_) {
        if (n == "ServerTriggerAbility") return on_ability_trigger(conn, rpc, bunch, now, /*on_mount=*/true);
        if (n == "RequestHitVerification_Server") {
            // the dash's hit: its confirmation went out with its effects, the verification has nothing more to ask [R: 906]
            ++hit_requests_;
            return;
        }
        if (n == "ServerNotifyHeldAbilityCancelled" || n == "ServerEndAbility") return;
    }
    if (unhandled_logged_.insert("mount:" + std::string(n)).second) spdlog::debug("[session] {}: ignoring the client's {} (the mount)", name(), n);
}

void PlayerSession::on_mount_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    const auto refuse = [&](const std::string& why) {
        ++mount_refused_;
        spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: getting on the mount is refused: {}", name(), why);
    };
    if (!mounts_ || !registry_ || !view_) return refuse("worldd has no mounts");
    if (a.owner == 0 || a.owner != static_cast<uint64_t>(pb_.setup().player_id)) return refuse("it is not this player's mount");
    if (mounts_->actor_id() != a.id) return refuse("it is not the mount the player has out");
    if (mounts_->phase() != MountController::Phase::Out) return refuse("the mount is not ready (it is being summoned, dissolves or died)");
    if (ride_ && ride_->actor_id() == a.id && ride_->dead()) return refuse("the mount is dead");
    if (view_->state_of(a.id) != world::ClientState::Awake) return refuse("the client does not have the mount open");
    if (!within_budget(ride_budget_, "a mount interaction", now)) return;
    const GoodPosition at = checker_.good();
    const double distance = std::hypot(a.x - at.x, a.y - at.y);
    if (distance > Ride::kInteractRangeCm || std::fabs(a.z - at.z) > 2.0 * Ride::kInteractRangeCm) {
        return refuse("the mount is " + std::to_string(static_cast<int>(distance)) + " cm away (at most " + std::to_string(static_cast<int>(Ride::kInteractRangeCm)) + ")");
    }
    if (!room_for(8)) return refuse("the connection is too busy");
    mount_up(conn, a, now);
}
// -- Cyb & C | O | A --

/// The NetGUID of a component of an actor of the registry: the actor's own server id and randomizer (they were minted together) with the component's ObjectId.
static FIntrepidNetworkGUID component_guid_of(const world::WorldActor& a, uint64_t object_id) {
    FIntrepidNetworkGUID g = a.actor.guid;
    g.ObjectId = object_id;
    return g;
}

bool PlayerSession::mount_up(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    const content::MountRow* row = mounts_ ? mounts_->book().find(mounts_->item_record()) : nullptr;
    if (!row || !world_ || !registry_ || !view_) return false;
    if (!ability_runtime(now)) return false;
    cancel_gather(&conn, "the player mounts", Interrupt::Mounted);   // Gathering stage 2: a rider harvests nothing
    const double t = ability_seconds(now);
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    const spawns::MountBook& book = mounts_->book();
    const spawns::DashBar dash = book.dash_bar(*row);
    const repl::Subobject* ability_sub = spawns::sub_of(a, "UAoCAbilityComponent");
    if (!ability_sub || a.npc.collision_cylinder == 0) {
        ++mount_refused_;
        spdlog::warn("[session] {}: the mount has no AbilityComponent or collision cylinder to ride on", name());
        return false;
    }
    // the mount's guids: its actor, its cylinder (the same server id and randomizer: minted in its own block), its AbilityComponent
    const FIntrepidNetworkGUID mount_guid = a.actor.guid;
    FIntrepidNetworkGUID cylinder = a.actor.guid;
    cylinder.ObjectId = a.npc.collision_cylinder;
    if (!ride_ || ride_->actor_id() != a.id) {
        Ride::Setup s;
        s.registry = registry_;
        s.data = &book.data();
        s.row = row;
        s.actor = a.id;
        s.run_speed = mounts_->run_speed(*row);
        Ride::fill_speeds(s, book, *row);
        s.world_time_at_zero = pb_.setup().world_time;
        s.roads = world_->roads().empty() ? nullptr : &world_->roads();
        s.dash_ability = dash.ability;
        s.dash_ready_at = dash_ready_at_;   // review A I3: the dash's 30 s belong to the rider, not to the mount actor of the moment
        const auto& cg = pb_.setup().character_guid;
        s.owner = repl::GuidValue{cg[0], cg[1], cg[2], cg[3]};
        ride_.emplace(std::move(s));
        mount_mesh_ = world_->mint_guid();
    }
    mount_guid_ = mount_guid;
    mount_cylinder_ = cylinder;
    mount_ability_ = ability_sub->guid;
    // the mount's own side: its owner, its rider's stats (the HUD of a possessed pawn reads them), its dash
    spawns::Possession p;
    p.player_controller = g.player_controller;
    p.player_state = g.player_state;
    p.rider_pawn = g.pawn;
    p.dash_ability = dash.ability;
    if (const repl::Subobject* stats = pb_.pawn().find(g.stats)) {
        const auto copy = [&](const char* field, std::vector<repl::ElementState>& out) {
            const schema::NetField* f = stats->obj.schema->field(field);
            const repl::FastArrayState* arr = f ? stats->obj.arrays.find(f->index) : nullptr;
            if (!arr) return;
            for (const repl::FastArrayItem& it : arr->items()) out.push_back(it.values);
        };
        copy("StatRepInt32Owner", p.owner_stats);
        copy("StatRepInt32OwnerProxy", p.owner_proxy_stats);
    }
    // the rider's pawn: it can name its mount (MountPtr: the InteractInfo component) and knows the dismount ability from now on [R: 910 packet 310]
    if (!interact_info_) interact_info_ = world_->mint_guid();
    if (!pb_.ensure_interact_info(*interact_info_)) {
        ++mount_refused_;
        spdlog::warn("[session] {}: the pawn has no InteractInfo to hold its mount: getting on is refused", name());
        return false;
    }
    if (const content::MountAbilityRow* dismount_ability = book.data().ability_with_role("dismount_elevated")) {
        if (std::find(known_.begin(), known_.end(), dismount_ability->ability) == known_.end()) {
            known_.push_back(dismount_ability->ability);
            if (std::find(records_.begin(), records_.end(), dismount_ability->ability) == records_.end()) records_.push_back(dismount_ability->ability);
            push_abilities();
        }
    }
    std::string error;
    ride_->set_gm_multiplier(gm_speed_, t);   // the GM travel tools: the mount's MoveSpeedMult starts at the GM's multiplier
    if (!ride_->begin(p, t, error)) {
        ++mount_refused_;
        spdlog::warn("[session] {}: getting on the mount failed: {}", name(), error);
        return false;
    }
    // the controlled actor is the mount now: its own checker, from where it stands (the client's TimeStamp restarts near 0)
    GoodPosition start;
    start.x = a.x;
    start.y = a.y;
    start.z = a.z;
    start.yaw_degrees = a.yaw_degrees;
    start.movement_mode = repl::kMoveWalking;
    checker_.rebase(ride_->limits(base_limits_, t), start);   // (review A I1: the pace bank and the credits are the session's, a new checker per possession refilled them)
    ride_->moved(a.x, a.y, a.z, a.yaw_degrees, t);   // a mount that stands on a road starts on it
    pending_fix_.reset();
    dismounted_valid_ = false;
    ++mount_ups_;
    // a mounted rider has no Sprint and no Stamina use (stage 3 fix 2: the user's run of 2026-10-08 sprinted on through the mount-up and drained to 0 mounted): a running Sprint ends
    // the way a Sprint ends (ClientEndAbility, MoveSpeedMult 1.0, the StaminaRegeneration item) before the possession goes out
    abilities_->set_mounted(true, t);
    take_ability_events();
    flush_abilities(conn);
    // retail's three packets within a millisecond [R: 910 packets 306, 307, 310]: the PlayerController, the mount, the pawn and the PlayerController again
    const int64_t dash_ability = dash.ability;
    const int32_t dash_slot = dash.slot;
    const FIntrepidNetworkGUID mesh = mount_mesh_;
    queue("the mount is possessed", [mount_guid, dash_ability, dash_slot](bootstrap::PlayerBootstrap& pb) { return pb.possess_mount(mount_guid, dash_ability, dash_slot); }, conn,
          /*must_build=*/true);
    {
        // the mount's InteractableComponent has the rider's client play the mounting [R: 910 packet 306, with the PlayerController's chain]
        const uint32_t channel = view_->channel_of(a.id);
        const FIntrepidNetworkGUID interactable = component_guid_of(a, a.npc.interact_component);
        const uint64_t id = a.id;
        queue("the mount's interaction animation", [this, id, interactable, channel](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            const world::WorldActor* m = registry_ ? registry_->find(id) : nullptr;
            if (!m || channel == 0) return std::nullopt;
            auto s = pb.interaction_animation_on(m->actor, interactable, static_cast<uint16_t>(spawns::kMountInput));
            if (s) s->channel = channel;
            return s;
        }, conn);
    }
    view_->update_now(conn, a.id);
    queue("the rider is attached to the mount", [mount_guid, mesh](bootstrap::PlayerBootstrap& pb) { return pb.pawn_mounted(mount_guid, mesh, 100); }, conn, /*must_build=*/true);
    queue("the controller follows the mount", [mount_guid, cylinder](bootstrap::PlayerBootstrap& pb) { return pb.controller_attach(mount_guid, cylinder); }, conn, /*must_build=*/true);
    spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: gets on {} (actor {}): its speed is {:.0f} cm/s at MoveSpeedMult 1, the move checker's limit {:.0f} cm/s", name(), row->item_name, a.id,
                mounts_ ? mounts_->run_speed(*row) : 0.0, ride_->speed_limit(1, t));
    return true;
}
// Cyb / C.O.A

std::optional<double> PlayerSession::ground_below(double x, double y, double z) const {
    const nav::AreaNav* nav = world_ ? world_->nav() : nullptr;
    double ground = 0;
    std::optional<double> below;
    // a flyer can be a hundred metres up: the search reaches far below, and 150 cm above (the world's own tolerance)
    if (nav && nav->ground_height(x, y, z, &ground, 60000.0, 150.0)) below = ground;
    // over a city node's town the baked ground is the pit under it (world/town_floor.h, about 50 m down): the town floor, when the flyer is above it, is the ground
    if (world_) {
        if (const auto town = world_->town_ground_at(x, y)) {
            const double floor = town->z + PlayerWorld::kBowlSlackCm;
            if (floor <= z + 150.0 && (!below || floor > *below)) below = floor;
        }
    }
    return below;
}

void PlayerSession::dismount(Connection& conn, Clock::time_point now, DismountWhy why) {
    if (!mounted()) return;
    const double t = ability_seconds(now);
    const GoodPosition g = checker_.good();
    const Ride::Place place = Ride::dismount_place(g.x, g.y, g.z, g.yaw_degrees);
    std::string error;
    // fix 2 (the user's rule, retail's): a dismounted mount STAYS where its rider left it, in the air or in the water, in its movement mode (5 flying, 4 swimming): it hovers or floats
    // and only the rider falls (its moves start in falling mode, below) or swims; an earlier draft let a flyer settle on the ground and an aquatic mount sink
    const bool in_the_air = g.movement_mode == repl::kMoveFlying;
    if (!ride_->end(t, g.x, g.y, g.z, g.yaw_degrees, g.movement_mode, error)) spdlog::warn("[session] {}: the mount does not let its rider go cleanly: {}", name(), error);
    if (in_the_air) {
        ++air_dismounts_;
        spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: the rider leaves its flying mount in the air: it falls from the detachment point, the mount hovers where it was", name());
    }
    // the pawn is the controlled actor again: its own checker, from where it lands (its clock restarts near 0 as well)
    GoodPosition start;
    start.x = place.x;
    start.y = place.y;
    start.z = place.z;
    start.yaw_degrees = place.yaw_degrees;
    start.movement_mode = repl::kMoveFalling;   // the rider leaps off [R: 912: its moves start in mode 3]
    checker_.rebase(base_limits_, start);
    pending_fix_.reset();
    dismounted_at_ = now;
    dismounted_valid_ = true;
    dismount_extra_sent_ = false;
    ++dismounts_;
    if (abilities_) {   // on foot again: the pawn's Sprint, Dodge and Rest are its own again
        abilities_->set_mounted(false, t);
        take_ability_events();
        flush_abilities(conn);
    }
    static const char* const kWhy[] = {"asked to", "its mount is desummoned", "another mount is summoned", "an ability needs it", "it died", "its mount died", "its mount is gone", "a GM teleport"};
    spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: gets off its mount ({}): at ({:.0f}, {:.0f}, {:.0f}) cm", name(), kWhy[static_cast<size_t>(why)], place.x, place.y, place.z);
    // retail's packet [R: 912 packet 139]: the PlayerController (restarted on its own pawn, attached to it), the pawn (detached beside the mount), the mount (released)
    const FIntrepidNetworkGUID pawn = pb_.setup().guids.pawn;
    const FIntrepidNetworkGUID cylinder = pb_.setup().guids.collision_cylinder;
    const repl::Vec3d at{place.x, place.y, place.z};
    const double yaw = place.yaw_degrees;
    if (why != DismountWhy::PlayerDied) {   // (a death's own frame restarts the pawn)
        queue("ClientRestart (the own pawn) and camera mode", [](bootstrap::PlayerBootstrap& pb) { return pb.restart_own_pawn(1); }, conn, /*must_build=*/true);
    }
    queue("the controller follows its own pawn", [pawn, cylinder](bootstrap::PlayerBootstrap& pb) { return pb.controller_attach(pawn, cylinder); }, conn, /*must_build=*/true);
    queue("the rider is detached from its mount", [at, yaw](bootstrap::PlayerBootstrap& pb) { return pb.pawn_dismounted(at, yaw); }, conn, /*must_build=*/true);
    if (view_ && ride_) view_->update_now(conn, ride_->actor_id());
}

void PlayerSession::on_dismount_request(Connection& conn, Clock::time_point now) {
    if (!within_budget(ride_budget_, "ServerDismountInteraction", now)) return;
    if (mounted()) return dismount(conn, now, DismountWhy::Request);
    // the second request of a dismount and a desummon in one datagram: retail's answer has two ClientRestarts [R: 893 packet 142]
    if (dismounted_valid_ && dismounted_at_ == now && !dismount_extra_sent_ && room_for(1)) {
        dismount_extra_sent_ = true;
        queue("ClientRestart (the own pawn) again", [](bootstrap::PlayerBootstrap& pb) { return pb.restart_own_pawn(1); }, conn, /*must_build=*/true);
        return;
    }
    spdlog::debug("[session] {}: ServerDismountInteraction while on foot: ignored", name());
}

void PlayerSession::run_ride(Connection& conn, Clock::time_point now) {
    if (!ride_ || !abilities_) return;
    if (!ride_->mount()) {
        // the mount left the world (desummoned and gone, the session's registry removed it): nothing is ridden, nothing is kept
        if (ride_->riding()) dismount(conn, now, DismountWhy::MountGone);
        ride_.reset();
        return;
    }
    if (!ride_->riding() && (!mounts_ || mounts_->actor_id() != ride_->actor_id())) {
        ride_.reset();   // the controller let the mount go (a desummon in its last moments): the ride is over
        return;
    }
    const Ride::Events ev = ride_->advance(ability_seconds(now));
    if (ev.dash_applied) {
        // the dash's reply [R: 906 packet 67]: the mount's Cooldowns, MoveSpeedMult and CurrentEffects go out with the PlayerController's hit confirmation
        if (view_) view_->update_now(conn, ride_->actor_id());
        bootstrap::HitConfirm h;
        h.record_guid = Ride::kDashHitRecord;
        h.source_type = content::kSourceTypeAbility;
        h.source_guid = ride_->dash_ability();
        h.source_is_actor = true;
        h.predicted_autonomous = true;
        h.instigator = mount_guid_;
        bootstrap::HitTargetOut target;
        target.component = mount_guid_;
        target.cylinder = mount_cylinder_;
        h.targets.push_back(std::move(target));
        if (pending_confirms_.size() >= kMaxPendingConfirms) pending_confirms_.pop_front();
        pending_confirms_.push_back(std::move(h));
        flush_abilities(conn);
    }
}
// C_Y_B and CoA

bool PlayerSession::mounted_trigger(Connection& conn, int64_t ability, int32_t instance, bool on_mount, Clock::time_point now) {
    const double t = ability_seconds(now);
    switch (ride_->trigger_dash(ability, t)) {
        case Ride::Dash::Started:
            ++dashes_;
            dash_ready_at_ = ride_->dash_ready_at();
            spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: the dash: its effects come in {:.0f} ms ({} s of MoveSpeedMult x{:.2f})", name(), Ride::kDashReplyDelay * 1000,
                        mounts_ && mounts_->book().data().dash() ? mounts_->book().data().dash()->duration : 0.0,
                        1.0 + (mounts_ && mounts_->book().data().dash() ? mounts_->book().data().dash()->value : 0.0));
            return false;
        case Ride::Dash::OnCooldown:
            ++dashes_refused_;
            spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: the dash is refused: it is cooling down (ready in {:.1f} s)", name(), std::max(0.0, ride_->dash_ready_at() - t));
            end_instance(conn, instance, on_mount);
            return false;
        case Ride::Dash::NotTheDash: break;
    }
    // what else a rider may do: the data's flags (the exe's bUsableWhileMounted and bAutoDismount)
    const content::MountAbilityRow* row = mounts_ ? mounts_->book().data().ability(ability) : nullptr;
    if (row && row->role == "summon") return true;   // Summon_Mount while riding: the mount is desummoned (on_summon_trigger gets the rider off first)
    if (row && row->auto_dismount) {
        dismount(conn, now, DismountWhy::Ability);
        return true;
    }
    if (row && row->usable_while_mounted && row->role != "dash") return true;
    ++mount_triggers_refused_;
    spdlog::log(level_for(ride_logged_, kLoggedMounts), "[session] {}: ability {} is refused while mounted (bUsableWhileMounted is off)", name(), ability);
    end_instance(conn, instance, on_mount);
    return false;
}

void PlayerSession::end_instance(Connection& conn, int32_t instance, bool on_mount) {
    if (!on_mount) {
        if (pending_ends_.size() < 64) pending_ends_.push_back(instance);
        flush_abilities(conn);
        return;
    }
    if (!registry_ || !ride_) return;
    const uint32_t channel = mount_channel();
    if (channel == 0) return;
    const uint64_t id = ride_->actor_id();
    const FIntrepidNetworkGUID ability = mount_ability_;
    queue("ClientEndAbility (the mount)", [this, id, ability, instance, channel](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        const world::WorldActor* m = registry_ ? registry_->find(id) : nullptr;
        if (!m) return std::nullopt;
        auto s = pb.end_ability_on(m->actor, ability, instance);
        if (s) s->channel = channel;
        return s;
    }, conn);
}

void PlayerSession::hit_on_mount(Connection& conn, const combat::IncomingHit& h, Clock::time_point now) {
    if (!ride_ || !pipeline_) return;
    ++mount_hits_;
    double damage = 0;
    if ((h.flags & stats::kHitMiss) == 0) {
        for (const combat::ResolvedMod& m : h.mods) {
            if (m.stat_guid == content::kGuidHealth && m.value < 0 && std::isfinite(m.value)) damage += -m.value;
        }
    }
    const Ride::Hurt r = ride_->hurt(damage);
    pipeline_->confirm_hit_on(h, ability_seconds(now), mount_guid_, mount_cylinder_, r.taken, r.died);
    if (r.died) mount_died(conn, now);
}

void PlayerSession::mount_died(Connection& conn, Clock::time_point now) {
    ++mount_deaths_;
    spdlog::info("[session] {}: its mount died under it: the rider is dismounted, the mount closes and its item is locked for {} s", name(), MountController::kLockSeconds);
    dismount(conn, now, DismountWhy::MountDied);
    if (mounts_ && abilities_) {
        const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        mounts_->die(ability_seconds(now), now_unix);
        run_mounts(conn, now);
        // review A M4: the lock and the Health 0 go to the saver now, like the player's own death's inventory: a worldd that stops before the next periodic save must not bring the dead mount back whole
        if (inventory_dirty_) hand_over_inventory("a mount's death", /*periodic=*/false);
    }
}

void PlayerSession::apply_persist(Connection* conn, const MountController::Persist& p) {
    const std::string guid = items::guid_hex(p.item_guid);
    const items::Outcome o = inventory_.update_mount(guid, p.health, p.lock_expiration, /*events=*/conn != nullptr && p.died);
    if (!o.ok) {
        spdlog::debug("[session] {}: the mount's item keeps nothing: {}", name(), o.refusal);
        return;
    }
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    if (!conn || o.events.empty() || !room_for(2)) return;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(o);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent (the mount's item)", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, *conn, /*must_build=*/true);
    }
}

std::optional<bootstrap::Send> PlayerSession::mount_ack(float time_stamp) {
    if (!ride_ || !registry_) return std::nullopt;
    const world::WorldActor* m = registry_->find(ride_->actor_id());
    const uint32_t channel = mount_channel();
    if (!m || channel == 0) return std::nullopt;
    auto s = bootstrap::PlayerBootstrap::move_ack_on(m->actor, time_stamp);
    if (s) s->channel = channel;
    return s;
}

std::optional<bootstrap::Send> PlayerSession::mount_correction(float time_stamp, const GoodPosition& good) {
    if (!ride_ || !registry_) return std::nullopt;
    const world::WorldActor* m = registry_->find(ride_->actor_id());
    const uint32_t channel = mount_channel();
    if (!m || channel == 0) return std::nullopt;
    const uint8_t mode = good.movement_mode == 0 ? repl::kMoveWalking : good.movement_mode;
    auto s = pb_.correction_on(m->actor, time_stamp, repl::Vec3d{good.x, good.y, good.z}, repl::Vec3d{}, mode);
    if (s) s->channel = channel;
    return s;
}

bool PlayerSession::set_current_value(int64_t record, float value) {
    if (!stats_ || !stats_->started() || abilities_failed_ || !std::isfinite(value)) return false;
    const stats::StatValue* v = stats_->find(record);
    if (!v || !v->held() || !pb_.has_stat_item(record)) return false;
    bootstrap::AbilityChange c;
    bootstrap::StatChange sc;
    sc.record = record;
    sc.value = value;
    sc.buckets = v->buckets();
    sc.base = v->base;
    sc.equipment = v->equip;
    c.stats.push_back(sc);
    if (!pb_.apply_ability_change(c)) return false;
    stats_->set_current(record, value);
    abilities_dirty_ = true;
    return true;
}

void PlayerSession::sync_ability_values() {
    if (pipeline_) pipeline_->sync_a7_values(pipeline_->now());
}

void PlayerSession::refresh_rank_mods() {
    if (stats_ && book_) {
        stats_->set_passives(book_->passives(wielded_line_));
        stats_->set_ranks(book_->held_ranks());
    }
    sync_ability_values();   // (the ranks changed what the formulas give Sprint's speed)
}
// CoA with C_Y_B

void PlayerSession::apply_gear_stats(const char* what) {
    if (!stats_ || !stats_->started()) return;
    refresh_rank_mods();   // plan D1, stage 2: the ranks the character has (and the weapon line in hand) are an input like the gear
    // plan D2: a class change takes the old class's effects off and resolves the new class's resource rule (its own stat changes go out with it)
    if (pipeline_ && pipeline_->resource_class() != stats_->class_enum()) pipeline_->refresh(pipeline_->now(), "the class changed", false);
    std::vector<stats::StatValue> changed;
    stats::CharacterStats::Next next;
    std::string error;
    // The new stats are only prepared: they become the session's once the pawn's arrays took them, so that the session's stats are
    // always what the client has (a refused change leaves both as they were, and the next gear change is a diff against that).
    if (!stats_->prepare(bootstrap::worn_items_of(inventory_), changed, next, error)) {
        ++stat_updates_failed_;
        spdlog::error("[session] {}: the stats cannot be computed for {} ({}): the client keeps the stats it has", name(), what, error);
        return;
    }
    if (next.faults != 0) {   // as the join does: a run that faulted has stats that are not the character's (a fault is a 0)
        ++stat_updates_failed_;
        spdlog::error("[session] {}: the stat program faulted {} time(s) for {} (a division by zero, a bad curve read or a value out of range): "
                      "its stats are not sent, the client keeps the stats it has", name(), next.faults, what);
        return;
    }
    if (changed.empty()) {
        stats_->commit(std::move(next));
        spdlog::debug("[session] {}: {} changes no stat", name(), what);
        return;
    }
    const std::optional<AbilityRuntime::GearSnapshot> runtime_before = abilities_ ? std::optional(abilities_->gear_snapshot()) : std::nullopt;
    // what the owner's arrays carry; a stat the ability runtime holds goes through it (Stamina is its own)
    bootstrap::AbilityChange change;
    size_t owner = 0, owner_proxy = 0, everyone = 0, without_item = 0;
    for (const stats::StatValue& v : changed) {
        if (v.float_array() || (v.replication != 1 && v.replication != 5 && v.replication != 7)) continue;
        // plan D1, stage 2: a class change lists stats the arrays were not laid out for (the new class's own resource): each gets an item in the
        // array its audience names (a fast-array add: the client takes a new item like a changed one)
        const bool new_item = !pb_.has_stat_item(v.guid);
        if (new_item) ++without_item;
        (v.replication == 1 ? owner : v.replication == 5 ? owner_proxy : everyone) += 1;
        // the GM travel tools: the GM's run speed multiplier stays on top of what the engine says of MoveSpeedMult
        const auto shown = v.guid == spawns::kStatMoveSpeedMult ? static_cast<decltype(v.value)>(v.value * gm_speed_) : v.value;
        if (!new_item && abilities_ && abilities_->tracks(v.guid)) {
            abilities_->gear_stat(change, v.guid, shown);
        } else if (!(abilities_ && v.guid == content::kGuidStamina)) {
            bootstrap::StatChange sc;
            sc.record = v.guid;
            sc.value = shown;
            sc.buckets = v.buckets();
            sc.base = v.base;
            sc.equipment = v.equip;
            sc.add_to_replication = new_item ? v.replication : 0;
            change.stats.push_back(sc);
        }
    }
    if (change.empty()) {
        stats_->commit(std::move(next));
        return;
    }
    if (!pb_.apply_ability_change(change)) {
        if (runtime_before) abilities_->restore_gear(*runtime_before);
        ++stat_updates_failed_;
        spdlog::error("[session] {}: the pawn does not take the stats of {} ({}): the client keeps the stats it has", name(), what,
                      pb_.error().empty() ? std::string("a stat has no item in its arrays") : pb_.error());
        return;
    }
    stats_->commit(std::move(next));
    ++stat_updates_;
    stat_items_changed_ += change.stats.size();
    spdlog::log(level_for(stat_logged_), "[session] {}: {} changes {} stat(s): {} Owner, {} OwnerProxy, {} EveryoneProxy{}", name(), what,
                change.stats.size(), owner, owner_proxy, everyone,
                without_item ? " (" + std::to_string(without_item) + " of them new to the client's arrays: added)" : std::string());
}

// ============================================================================================================================
//  using an item (plan D1)
// ============================================================================================================================

double PlayerSession::xp_add_mult() const {
    if (!stats_) return 0;
    const stats::StatValue* v = stats_->find(content::kGuidAdventuringXPAddMult);
    return v && std::isfinite(v->value) ? v->value : 0.0;
}
// built by C-O-A with C_Y_B

std::string PlayerSession::fresh_guid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (;;) {
        const uint64_t a = rng(), b = rng();
        const repl::GuidValue g{static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(a), static_cast<uint32_t>(b >> 32), static_cast<uint32_t>(b)};
        if (!(g.a | g.b | g.c | g.d)) continue;
        const std::string hex = items::guid_hex(g);
        bool taken = false;
        for (const svc::InventoryItem& it : inventory_.items()) taken = taken || it.instance_guid == hex;
        if (!taken) return hex;
    }
}

// What a level gained does to the character (plan D1, stage 2): the pawn's level, the stats, the ranks and abilities the level grants, the passives' effect items,
// the TempHealth mark.  `class_changed`: the stats are computed once for both.
void PlayerSession::finish_level_up(const leveling::XpResult& xp, bool class_changed, Clock::time_point now, bootstrap::AbilityChange& rank_effects,
                                    std::vector<repl::SkillOnActionbar>& skill_updates) {
    const bool leveled = xp.leveled();
    if (leveled) {
        if (!pb_.set_level(progress_.level)) spdlog::error("[session] {}: the pawn does not take level {}", name(), progress_.level);
        if (stats_) stats_->set_level(progress_.level);
        if (book_) {
            // the ranks the new level grants by itself: the Universal tree's and the class tree's
            const talents::Catalog& catalog = book_->catalog();
            std::vector<talents::RankRef> granted;
            const int from = xp.before.level, to = xp.after.level;
            book_->set_level(to);
            for (const talents::Tree* tree : {catalog.universal(), book_->class_tree()}) {
                if (!tree) continue;
                const std::vector<talents::RankRef> had = catalog.auto_ranks(*tree, from);
                for (const talents::RankRef& r : catalog.auto_ranks(*tree, to)) {
                    const bool before = std::any_of(had.begin(), had.end(), [&](const talents::RankRef& h) { return h.node->skill == r.node->skill; });
                    if (!before) granted.push_back(r);
                }
            }
            std::stable_sort(granted.begin(), granted.end(), [](const talents::RankRef& a, const talents::RankRef& b) {
                return a.node->level_requirement < b.node->level_requirement;
            });
            grant_ranks(granted, now, rank_effects, skill_updates);
            for (const talents::Tree* tree : {catalog.universal(), book_->class_tree()}) {
                if (tree && !pb_.set_tree_ranks(tree->guid, book_->list(*tree))) spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", name(), tree->name);
            }
        }
    }
    if (leveled || class_changed) {
        apply_gear_stats(leveled && class_changed ? "the new level and class" : leveled ? "the new level" : "the new class");
    }
    if (leveled) {
        if (!pb_.mark_temp_health()) spdlog::debug("[session] {}: no TempHealth item to mark", name());
    }
    if (!rank_effects.empty() && !pb_.apply_ability_change(rank_effects)) {
        spdlog::warn("[session] {}: the pawn does not take the granted passives' effect items ({})", name(), pb_.error().empty() ? "keys disagree" : pb_.error());
        for (const bootstrap::EffectItem& e : rank_effects.effects_added) {
            for (auto it = rank_effects_.begin(); it != rank_effects_.end();) it = it->second == e.effect_id ? rank_effects_.erase(it) : std::next(it);
            markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == e.effect_id; }), markers_.end());
        }
    }
    if (!rank_effects.empty()) abilities_dirty_ = true;
}

void PlayerSession::on_item_activated(Connection& conn, const repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now) {
    const char* what = "ServerItemUsed (a use)";
    const auto refuse = [&](const std::string& why) {
        ++uses_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    if (!db) return refuse("worldd has no content.db to read the item's actions from");
    use::Context ctx;
    ctx.class_enum = class_id_;
    ctx.progress = progress_;
    ctx.xp_add_mult = xp_add_mult();
    ctx.curve = curve;
    // Gathering stage 2: the professions, and the runner of a Profession reward's expression (made when the first one is asked for: it runs the stat graph once)
    ctx.professions = world_->professions();
    ctx.held = professions_;
    const abilities::Catalog* ability_catalog = world_->ability_catalog();
    std::shared_ptr<professions::XpRunner> runner;
    if (ctx.professions && ability_catalog && stats_ && stats_->started()) {
        const std::vector<stats::WornItem> worn = bootstrap::worn_items_of(inventory_);
        runner = std::make_shared<professions::XpRunner>();
        ctx.profession_xp = [this, runner, ability_catalog, worn](int64_t expr, const std::map<std::string, int>& levels, std::string& why) -> std::optional<double> {
            if (!*runner) {
                *runner = professions::make_xp_runner(*ability_catalog, *stats_, worn, why);
                if (!*runner) return std::nullopt;
            }
            return (*runner)(expr, levels, why);
        };
    }
    const use::Plan plan = use::plan_use(*db, item.item_record_id, ctx);
    if (!plan.ok) return refuse(plan.refusal);
    // what the use grants, as fresh instances (their kind decides where the inventory puts them: a resource item to a resource container, the rest to the Backpack)
    std::vector<svc::InventoryItem> fresh;
    {
        const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
        for (const use::Granted& g : plan.items) {
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = g.record;
            it.instance_guid = fresh_guid();
            it.quantity = g.quantity;
            it.quality = g.quality;
            it.durability = g.durability;
            it.created_at_ms = created;
            fresh.push_back(std::move(it));
        }
    }
    // the room: the grants need room in the storage of their kind; the consumed stack frees its slot only when it empties (tried on a copy)
    if (!fresh.empty()) {
        items::Inventory trial = inventory_;
        if (plan.consume) trial.consume(svc::ItemContainer::Backpack, use.slot, item.instance_guid, 1);
        if (!trial.can_grant(fresh, item_catalog())) {
            return refuse("the storages have no room for the use's " + std::to_string(plan.items.size()) + " item(s) (the bag has " +
                          std::to_string(static_cast<size_t>(items::kBackpackSlots) - inventory_.count(svc::ItemContainer::Backpack)) + " free slot(s)): nothing is used");
        }
    }
    const std::span<const content::AutoGrant> grants = world_->auto_grants();
    // the steps: the events, the XPTracker, the broadcast, the pawn's state, the class bar, and a skill update for each rank a level or a class grants
    const size_t steps = 1 + (plan.items.size() + 1) / bootstrap::kMaxEventsPerChain + 1 + 1 + 1 + 1 + grants.size() + 8 + plan.profession_changes.size();
    if (!room_for(steps)) return;
// (Cyb : C.O.A)

    // the inventory: the consumed stack, then the grants
    items::Outcome all;
    all.ok = true;
    if (plan.consume) {
        const items::Outcome c = inventory_.consume(svc::ItemContainer::Backpack, use.slot, item.instance_guid, 1);
        if (!c.ok) return refuse(c.refusal);
        all.events.insert(all.events.end(), c.events.begin(), c.events.end());
    }
    if (!fresh.empty()) {
        const items::Outcome g = inventory_.grant(std::move(fresh), item_catalog());
        if (!g.ok) {
            // the room was checked: a refusal here is a record the table refuses, not something the client can cause; the consumed stack stays consumed
            spdlog::error("[session] {}: the use's items are not granted ({}): the item was consumed and nothing came of it", name(), g.refusal);
        } else {
            all.events.insert(all.events.end(), g.events.begin(), g.events.end());
        }
    }
    const bool inventory_moved = !all.events.empty();
    if (inventory_moved) {
        inventory_dirty_ = true;
        inventory_changed_ = true;
    }

    // the progress: the XP, the levels it crosses and the ranks they grant
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (plan.adventuring_xp > 0 && curve) {
        xp = leveling::add_xp(*curve, grants, progress_, plan.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);

    // the professions (Gathering stage 2): the XP, the Promotes and the levels they cross
    const std::vector<professions::Change> profession_changes = plan.profession_changes.empty() ? std::vector<professions::Change>() : set_professions(plan.professions_after);
    const bool professions_changed = !profession_changes.empty();

    // the class
    bool class_changed = false;
    std::string class_name;
    std::optional<content::ClassRecord> new_class;
    if (plan.set_class) {
        std::string error;
        const char* cname = bootstrap::client_class_name(*plan.set_class);
        const auto cls = cname ? db->class_for_name(cname, error) : std::nullopt;
        if (!cls) {
            spdlog::error("[session] {}: the class {} is not in content.db ({}): the class stays", name(), *plan.set_class, error);
        } else if (!pb_.set_class(cls->guid)) {
            spdlog::error("[session] {}: the pawn does not take class {}: the class stays", name(), cls->name);
        } else {
            class_id_ = *plan.set_class;
            class_name = cls->name;
            new_class = *cls;
            if (stats_) stats_->set_class(class_id_);
            class_changed = true;
            progress_changed_ = true;
        }
    }
    // plan D1, stage 2: what the ranks do (the abilities, the passives' items, the bar) when the class and the level change
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    std::vector<std::pair<int, int>> cleared_slots;
    if (class_changed && book_) switch_class(*new_class, now, rank_effects, skill_updates, cleared_slots);
    finish_level_up(xp, class_changed, now, rank_effects, skill_updates);

    // what the client is told, in order: the storage events, the XPTracker, the level-up, the pawn's state, the granted skills
    ++uses_applied_;
    ++items_applied_;
    if (inventory_moved) {
        const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
        for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    if (xp.changed() || leveled || class_changed || professions_changed) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        const std::vector<ProfessionWire> wire = profession_wire(profession_changes);   // the professions that changed, in profession order
        queue("XPTracker", [total, debt, wire](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            for (const ProfessionWire& w : wire) {
                if (!pb.set_profession(w.leveling_type, w.xp, w.certification)) return std::nullopt;
            }
            return pb.leveling_update();
        }, conn, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn);
    }
    if (professions_changed) queue_profession_level_ups(conn, profession_changes);
    if (leveled || class_changed) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, /*must_build=*/true);
    }
    if (class_changed && book_) {
        // the old class's slots are sent cleared and the new class's skills placed: the whole bar as it is now, in one payload (a slot of UseType
        // None is a clear; retail's bar payloads hold them too). ClientClearActionbarsForInstigators is not used: no capture shows its shape.
        repl::ActionbarPayload payload = bar_payload();
        for (const auto& [bank, slot] : cleared_slots) {
            if (bar_.at(bank, slot)) continue;   // taken again by a skill of the new class
            repl::ActionbarSlot none;
            none.bank_index = bank;
            none.slot_index = slot;
            payload.slots.push_back(none);
        }
        queue("ClientReceiveActionbarSlotPayload", [payload](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(payload); }, conn, /*must_build=*/true);
    }
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, /*must_build=*/true);
    }

    std::string granted_names;
    for (const repl::SkillOnActionbar& u : skill_updates) {
        std::string skill = std::to_string(u.skill_id);
        if (book_) {
            for (const talents::Tree* t : book_->listed_trees()) {
                for (const talents::Node& n : t->nodes) {
                    if (n.skill == u.skill_id && t->guid == u.skill_tree_id) skill = n.skill_name;
                }
            }
        }
        granted_names += (granted_names.empty() ? "" : ", ") + skill;
    }
    std::string line = "used item " + std::to_string(item.item_record_id) + ": " + plan.summary;
    if (xp.changed()) line += "; Adventuring " + std::to_string(xp.after.xp) + " XP (debt " + std::to_string(xp.after.debt) + ")";
    if (leveled) line += ", level " + std::to_string(xp.before.level) + " -> " + std::to_string(xp.after.level);
    for (const professions::Change& c : profession_changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (!pf) continue;
        const professions::Book& pbook = *professions_.book();
        line += "; " + pf->name + " level " + std::to_string(c.before.level) + " -> " + std::to_string(c.after.level) + ", " + pbook.tier(c.before.tier).name + " -> " +
                pbook.tier(c.after.tier).name + ", " + std::to_string(c.after.xp) + " XP";
    }
    if (!granted_names.empty()) line += ", granted " + granted_names;
    if (class_changed) line += "; the class is " + class_name + " now (its tree, abilities and bar are sent live)";
    line += "; " + std::to_string(all.events.size()) + " storage event(s)";
    for (const std::string& n : plan.notes) line += "; " + n;
    spdlog::log(level_for(use_logged_), "[session] {}: {}", name(), line);
    if (xp.changed() || class_changed || professions_changed) {
        hand_over_progress(leveled ? "a level-up" : class_changed ? "a class change" : professions_changed && !xp.changed() ? "a profession changed" : "XP earned");
    }
}
// C | O | A and Cyb

std::vector<professions::Change> PlayerSession::set_professions(const professions::Professions& after) {
    std::vector<professions::Change> changes = professions::Professions::diff(professions_, after);
    if (changes.empty()) return changes;
    professions_ = after;
    progress_changed_ = true;
    if (stats_) stats_->set_profession_levels(professions_.levels_by_name());
    for (const professions::Change& c : changes) {
        if (c.leveled()) profession_levels_gained_ += static_cast<size_t>(c.after.level - c.before.level);
    }
    return changes;
}

std::vector<PlayerSession::ProfessionWire> PlayerSession::profession_wire(const std::vector<professions::Change>& changes) const {
    std::vector<ProfessionWire> wire;
    if (!professions_.valid()) return wire;
    const professions::Book& book = *professions_.book();
    for (const professions::Change& c : changes) {
        if (const professions::Profession* pf = book.find(c.profession)) wire.push_back({pf->leveling_type, c.after.xp, book.tier(c.after.tier).guid});
    }
    return wire;
}

void PlayerSession::queue_profession_level_ups(Connection& conn, const std::vector<professions::Change>& changes) {
    if (!professions_.valid()) return;
    // a profession's level-up is the XP item and BroadcastPlayLevelUpEffects on the pawn with the profession's leveling type as the category (retail's artisan and weapon levels)
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (!pf || !c.leveled()) continue;
        const int64_t category = pf->leveling_type;
        const int level = c.after.level;
        queue("BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(category, level); }, conn);
    }
}

void PlayerSession::apply_outcome(Connection& conn, const items::Outcome& outcome, const char* what) {
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    if (outcome.equipment_changed && book_ && world_ && world_->content()) {
        // plan D1, stage 2: the weapon line in hand follows what is worn (a weapon tree's passives apply while its line is in hand)
        const auto w = bootstrap::worn_weapon(*world_->content(), inventory_, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
        wielded_line_ = w ? w->line : 0;
    }
    if (outcome.equipment_changed) apply_gear_stats();   // before the replies are queued: they are built later, and carry the stats' deltas
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    // the storage events go first (the PlayerController's chain), then the pawn's visible gear: the order and the packet of the retail answer
    queue("ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn, /*must_build=*/true);
    if (outcome.equipment_changed) {
        const std::vector<bootstrap::HeldItem> held = bootstrap::held_items_for(inventory_);
        const std::vector<repl::AppearanceId> appearance = items::appearance_ids(inventory_);
        // the retail server marks the 23 held items once for each change of the Equipment storage its answer holds
        size_t marks = 0;
        for (const items::Event& e : outcome.events) marks += e.container == svc::ItemContainer::Equipment ? 1 : 0;
        queue("HeldItems and AppearanceIDs",
              [held, appearance, marks](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
                  if (!pb.apply_equipment(held, appearance, marks ? marks : 1)) return std::nullopt;
                  return pb.equipment_update();
              },
              conn, /*must_build=*/true);
    }
    spdlog::log(level_for(item_applied_logged_), "[session] {}: {}: {} storage event(s){}", name(), what, outcome.events.size(),
                outcome.equipment_changed ? ", the visible gear is updated" : "");
    for (const items::Event& e : outcome.events) {
        if (e.type == repl::StorageEventType::Add && e.container == svc::ItemContainer::Equipment && e.slot >= items::equip::ResourceBag0 && e.slot <= items::equip::ResourceBag2) {
            // Gathering stage 3: the container of a bag worn now is not open this session (the client lays its containers out at the login)
            spdlog::info("[session] {}: a resource bag is worn in Equipment slot {}: its container opens at the next login", name(), e.slot);
        }
    }
}
// (C Y B | CoA)

void PlayerSession::refuse_item_request(Connection& conn, const char* what, const std::string& why, Clock::time_point now) {
    ++items_refused_;
    spdlog::log(level_for(item_refused_logged_), "[session] {}: {} refused, nothing changed: {}", name(), what, why);
    // the client is told what is where, as at the join; a client that is refused over and over gets that a few times, then silence
    if (!resync_budget_.take(now) || !room_for(5)) return;
    std::vector<bootstrap::StorageKind> kinds = {bootstrap::StorageKind::Backpack, bootstrap::StorageKind::Equipment};
    for (int i = 0; i < items::kResourceContainers; ++i) {
        if (pb_.setup().resource_bags[static_cast<size_t>(i)].present) kinds.push_back(bootstrap::resource_kind(i));   // Gathering stage 3: a refused drag in a resource container too
    }
    for (const bootstrap::StorageKind k : kinds) {
        queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::Pawn); }, conn);
    }
}

void PlayerSession::on_item_used(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto use = repl::parse_item_used(rpc.call);
    if (!use) {
        if (!item_unreadable_logged_) {
            item_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerItemUsed does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(item_budget_, "ServerItemUsed", now) || !room_for(4)) return;
    if (!catalog_) return refuse_item_request(conn, "ServerItemUsed", "worldd has no item catalog", now);
    const auto storage = container_of_location(use->storage);
    if (!storage) return refuse_item_request(conn, "ServerItemUsed", "items of storage " + std::to_string(use->storage) + " are not used", now);
    // the mounts milestone: a mount item is neither worn nor consumed; the summon or the desummon is the packet's next call
    if (mounts_ && *storage == svc::ItemContainer::Backpack) {
        const svc::InventoryItem* item = inventory_.at(*storage, use->slot);
        if (item && item->instance_guid == items::guid_hex(use->expected_item) && mounts_->is_mount_item(item->item_record_id)) {
            const svc::InventoryItem copy = *item;
            return on_mount_item_used(copy);
        }
    }
    // plan D1: a bag item that content.db lists no slot for is used, not worn (the item must be where the client says, as for an equip)
    if (*storage == svc::ItemContainer::Backpack) {
        const svc::InventoryItem* item = inventory_.at(*storage, use->slot);
        if (item && item->instance_guid == items::guid_hex(use->expected_item)) {
            const auto info = catalog_->info(item->item_record_id);
            if (!info || !info->wearable()) {
                const svc::InventoryItem copy = *item;   // the inventory changes under the use
                return on_item_activated(conn, *use, copy, now);
            }
        }
    }
    const items::Outcome outcome = inventory_.use(*storage, use->slot, items::guid_hex(use->expected_item), use->requested_equip_slot, *catalog_);
    if (!outcome.ok) return refuse_item_request(conn, "ServerItemUsed", outcome.refusal, now);
    apply_outcome(conn, outcome, *storage == svc::ItemContainer::Backpack ? "an item is worn" : "an item is taken off");
}

void PlayerSession::on_storage_event_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto event = repl::parse_storage_event(rpc.call);
    if (!event) {
        if (!item_unreadable_logged_) {
            item_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerRequestStorageEvent does not read ({} parameter bits): ignored (further ones are not logged)", name(),
                         rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(item_budget_, "ServerRequestStorageEvent", now) || !room_for(4)) return;
    const char* what = "ServerRequestStorageEvent";
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    const auto container_of = [&](const protocol::emit::FIntrepidNetworkGUID& object) -> std::optional<svc::ItemContainer> {
        if (object == g.backpack) return svc::ItemContainer::Backpack;
        if (object == g.equipment) return svc::ItemContainer::Equipment;
        if (object == g.quest_storage) return svc::ItemContainer::Quest;
        for (int i = 0; i < items::kResourceContainers; ++i) {
            if (pb_.setup().resource_bags[static_cast<size_t>(i)].present && object == g.resource_storage[static_cast<size_t>(i)]) return items::resource_container(i);
        }
        return std::nullopt;
    };
    const auto type = static_cast<repl::StorageEventType>(event->type);
    if (type != repl::StorageEventType::Move && type != repl::StorageEventType::Swap) {
        // (a Remove is the client destroying an item by dragging it out, a Fill dragging a stack onto another: Gathering stage 3 refuses both; the backlog has them)
        return refuse_item_request(conn, what, "an event of type " + std::to_string(event->type) + " is not made (only a Move and a Swap of whole stacks)", now);
    }
    if (event->move_source.slots.size() != 1 || event->targets.size() != 1 || event->targets[0].slots.size() != 1) {
        return refuse_item_request(conn, what, "a move names one source slot and one target slot", now);
    }
    const auto from = container_of(event->move_source.object);
    const auto to = container_of(event->targets[0].object);
    if (!from || !to) return refuse_item_request(conn, what, "a container that is none of this character's storages", now);
    const repl::StorageEventSlot& src = event->move_source.slots[0];
    const repl::StorageEventSlot& dst = event->targets[0].slots[0];
    if (!(src.item_guid == event->item.guid)) return refuse_item_request(conn, what, "the item of the event is not the instance of its source slot", now);
    const std::string guid = items::guid_hex(src.item_guid);
    const items::Outcome outcome = type == repl::StorageEventType::Move
                                       ? inventory_.move(*from, src.slot, src.count, guid, *to, dst.slot, &item_catalog())
                                       : inventory_.swap(*from, src.slot, src.count, guid, *to, dst.slot, items::guid_hex(dst.item_guid), dst.count);
    if (!outcome.ok) return refuse_item_request(conn, what, outcome.refusal, now);
    apply_outcome(conn, outcome, type == repl::StorageEventType::Move ? "an item is moved in the bag" : "two items swap places in the bag");
}

void PlayerSession::on_combat_settings(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    constexpr std::string_view kPrefix = "CharacterInGameSettings.";
    std::vector<bootstrap::PropertyValue> values;
    if (rpc.call.decoded) {
        for (const std::string_view path : kCombatEchoPaths) {
            const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, path.substr(kPrefix.size()));
            if (!c) continue;
            const auto it = rpc.call.params.values.find(c->handle);
            if (it != rpc.call.params.values.end()) values.push_back({path, it->second});
        }
    }
    if (values.empty()) {
        spdlog::log(level_for(combat_empty_logged_), "[session] {}: the client's combat settings carry none of the {} values worldd echoes",
                    name(), kCombatEchoPaths.size());
        return;
    }
    if (!within_budget(combat_budget_, "ServerUpdateAccountCombatSettings", now) || !room_for(1)) return;
    spdlog::log(level_for(combat_logged_), "[session] {}: the client's combat settings: echoing {} of the {} values worldd sends", name(),
                values.size(), kCombatEchoPaths.size());
    queue("combat settings echo",
          [values = std::move(values)](bootstrap::PlayerBootstrap& pb) { return pb.combat_settings_echo(values); }, conn);
}

void PlayerSession::on_storage_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto it = rpc.call.params.values.find(1);
    const auto* container = rpc.call.decoded && it != rpc.call.params.values.end() ? it->second.get_if<FIntrepidNetworkGUID>() : nullptr;
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    std::optional<bootstrap::StorageKind> kind;
    if (container) {
        if (*container == g.backpack) kind = bootstrap::StorageKind::Backpack;
        else if (*container == g.equipment) kind = bootstrap::StorageKind::Equipment;
        else if (*container == g.quest_storage) kind = bootstrap::StorageKind::QuestStorage;
        else {
            for (int i = 0; i < items::kResourceContainers; ++i) {
                if (pb_.setup().resource_bags[static_cast<size_t>(i)].present && *container == g.resource_storage[static_cast<size_t>(i)]) kind = bootstrap::resource_kind(i);
            }
        }
    }
    if (!kind) {
        spdlog::debug("[session] {}: ServerRequestStorageData names a container worldd has no payload for", name());
        return;
    }
    if (!within_budget(storage_budget_, "ServerRequestStorageData", now) || !room_for(1)) return;
    const bootstrap::StorageKind k = *kind;
    queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::Pawn); }, conn);
}

// ============================================================================================================================
//  plan E1: the GM vendor
// ============================================================================================================================

const world::WorldActor* PlayerSession::open_vendor(std::string& why) {
    if (vendor_actor_ == 0) {
        why = "no vendor window is open";
        return nullptr;
    }
    const world::WorldActor* a = registry_ ? registry_->find(vendor_actor_) : nullptr;
    const GoodPosition at = checker_.good();
    if (!a) {
        why = "the vendor is gone";
    } else if (!a->npc.is_gm_vendor) {
        why = "the NPC is no GM vendor";
    } else if (std::hypot(a->x - at.x, a->y - at.y) > kInteractRangeCm) {
        why = "the vendor is " + std::to_string(static_cast<int>(std::hypot(a->x - at.x, a->y - at.y) / 100.0)) + " m away";
    } else {
        return a;
    }
    vendor_actor_ = 0;
    vendor_def_ = 0;
    return nullptr;
}
// -- C.O.A x C-Y-B --

void PlayerSession::on_interact(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_) return;
    const auto req = repl::parse_interact_request(rpc.call);
    if (!req) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerInteractWithInteractableComponent does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerInteractWithInteractableComponent", now)) return;
    const auto ignore = [&](const std::string& why) {
        ++interact_ignored_;
        spdlog::log(level_for(interact_logged_), "[session] {}: an interaction with component {} is ignored: {}", name(), req->interactable.ObjectId, why);
    };
    if (!registry_ || !view_) return ignore("the world has no actors");
    if (mounted()) return ignore("a rider does not interact (it is on its mount)");   // the mounts milestone, stage 3
    const world::WorldActor* a = registry_->find_by_component(req->interactable.ObjectId);
    if (!a) return ignore("it is no component of an actor of this world");
    if (a->kind == world::ActorKind::Mount) return on_mount_interact(conn, *a, now);   // getting on
    if (a->kind == world::ActorKind::Gatherable) return on_gather_interact(conn, *a, now);   // Gathering stage 2: a resource node
    if (view_->state_of(a->id) != world::ClientState::Awake) return ignore("the client does not have that actor open");
    const GoodPosition at = checker_.good();
    const double distance = std::hypot(a->x - at.x, a->y - at.y);
    if (distance > kInteractRangeCm) return ignore("the actor is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
    // Gathering stage 3, "Hunting": the interactable of a huntable creature starts its hunt, alive as it stands (or dead, if it was killed some other way), not a loot window
    if (a->kind == world::ActorKind::Npc && a->npc.interact_def != 0 && npc_world_ && npc_world_->gathering() && npc_world_->gathering()->hunt_kind(a->npc.interact_def)) {
        return on_hunt_interact(conn, *a, now);
    }
    if (npc_world_ && npc_world_->aftermath()) {
        if (const combat::Corpse* corpse = npc_world_->aftermath()->corpse_by_interactable(req->interactable.ObjectId)) return open_loot(conn, *corpse, *a, now);
    }
    const vendor::GmVendors* gm = world_ ? world_->gm_vendors() : nullptr;
    const vendor::GmVendor* def = (gm && a->npc.is_gm_vendor) ? gm->find(a->npc.vendor_guid) : nullptr;
    if (!def) return ignore("the actor " + a->npc.name + " has nothing to offer yet");
    if (!room_for(1)) return;
    vendor_actor_ = a->id;
    vendor_def_ = def->guid;
    ++vendor_opens_;
    const int64_t merch = def->guid;
    const FIntrepidNetworkGUID npc = a->actor.guid;
    spdlog::log(level_for(vendor_logged_), "[session] {}: opens the window of {} ({}, {} listing(s) in {} tab(s))", name(), def->label, def->name, def->listings, def->tabs);
    queue("ClientOpenVendor", [merch, npc](bootstrap::PlayerBootstrap& pb) { return pb.open_vendor(merch, npc); }, conn, /*must_build=*/true);
}

// The vendor window waits for this answer (vendor/node_tax.h): the exe's UVendorMenu::InitializeVendor sent the request, and only the table's
// arrival makes the window fill its listings, taxes and funds.  Retail (captures 716 and 1536) answers the next packet with the node's table.
void PlayerSession::on_node_tax_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto node = repl::parse_node_tax_request(rpc.call);
    ++tax_requests_;
    if (!node) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerGetNodeTaxTable does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(tax_budget_, "ServerGetNodeTaxTable", now) || !room_for(1)) return;
    if (dead_) return;   // the window closed with the death
    std::string why;
    const world::WorldActor* a = open_vendor(why);
    if (!a) {
        spdlog::log(level_for(tax_logged_), "[session] {}: the client asks for the taxes of node {} but has no vendor window open ({}): ignored", name(), *node, why);
        return;
    }
    ++tax_tables_sent_;
    spdlog::log(level_for(tax_logged_), "[session] {}: the window of {} asks for the taxes of node {}: sending the untaxed table ({} tags)", name(), a->npc.name, *node,
                vendor::untaxed_table(*node).entries.size());
    const int64_t asked = *node;
    queue("ClientSetNodeTaxTable", [asked](bootstrap::PlayerBootstrap& pb) { return pb.node_tax_table(vendor::untaxed_table(asked)); }, conn, /*must_build=*/true);
}

void PlayerSession::on_vendor_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto cart = repl::parse_vendor_purchase(rpc.call);
    if (!cart) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerHandleVendorPurchase does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerHandleVendorPurchase", now) || !room_for(6)) return;
    const char* what = "ServerHandleVendorPurchase";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const vendor::GmVendors* gm = world_ ? world_->gm_vendors() : nullptr;
    if (!db || !gm) return refuse("worldd has no GM vendors");
    if (dead_) return refuse("the character is dead: the vendor window closed with the death");
    std::string why;
    const world::WorldActor* a = open_vendor(why);
    if (!a) return refuse(why);
    const vendor::GmVendor* def = gm->find(vendor_def_);
    if (!def) return refuse("the open window's vendor def is unknown");
    const size_t free = static_cast<size_t>(items::kBackpackSlots) - inventory_.count(svc::ItemContainer::Backpack);
    const vendor::PurchasePlan plan = gm->plan_purchase(*db, *def, *cart, free);
    if (!plan.ok) return refuse(plan.refusal);
    std::vector<svc::InventoryItem> fresh;
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (const use::Granted& g : plan.stacks) {
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;
        it.item_record_id = g.record;
        it.instance_guid = fresh_guid();
        it.quantity = g.quantity;
        it.quality = g.quality;
        it.durability = g.durability;
        it.created_at_ms = created;
        // an instance guid must be new within this batch too (fresh_guid only looks at the inventory)
        for (const svc::InventoryItem& other : fresh) {
            if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
        }
        fresh.push_back(std::move(it));
    }
    const items::Outcome outcome = inventory_.grant(std::move(fresh), item_catalog());
    if (!outcome.ok) return refuse(outcome.refusal);
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++vendor_purchases_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::log(level_for(vendor_logged_), "[session] {}: bought {} (free): {} storage event(s)", name(), plan.summary, outcome.events.size());
}
// ~ Cyb - C-O-A ~

// ============================================================================================================================
//  plan E1, stage 3, part 2: kills and loot
// ============================================================================================================================

namespace {

/// The instance guid of the stack in slot `slot` of the corpse `npc` (the window and the events agree on it; it is not a guid the bag holds).
repl::GuidValue loot_guid(uint64_t npc, size_t slot) {
    uint64_t s = npc * 0x9E3779B97F4A7C15ull + slot * 0xBF58476D1CE4E5B9ull + 0x5851F42D4C957F2Dull;
    const auto next = [&s]() {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    repl::GuidValue g;
    const uint64_t a = next(), b = next();
    g.a = static_cast<uint32_t>(a);
    g.b = static_cast<uint32_t>(a >> 32);
    g.c = static_cast<uint32_t>(b);
    g.d = static_cast<uint32_t>(b >> 32) | 1u;   // never the zero guid
    return g;
}

}  // namespace

// ============================================================================================================================
//  plan E1, stage 3, part 2: the player's death
// ============================================================================================================================

namespace {

/// Whether an item record is a material or currency (the retail PvE death takes about half of each stack of those, not the gear): its gameplay tags
/// include Item.Resource... (the Glint certificates, reagents, raw materials).
bool is_material(const content::ContentDb& db, int64_t record) {
    std::string err;
    const auto gear = db.item_gear(record, err);
    if (!gear) return false;
    for (const std::string& t : gear->tags) {
        std::string lower;
        for (const char c : t) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.rfind("item.resource", 0) == 0) return true;
    }
    return false;
}

}  // namespace

void PlayerSession::check_death(Connection& conn, Clock::time_point now) {
    if (dead_ || !stats_ || !stats_->started()) return;
    const stats::StatValue* h = stats_->find(content::kGuidHealth);
    if (!h || h->value > 0) return;
    die(conn, now);
}

void PlayerSession::die(Connection& conn, Clock::time_point now) {
    dead_ = true;
    ++deaths_;
    cancel_gather(&conn, "the player died", Interrupt::Death);   // Gathering stage 2
    if (pipeline_) pipeline_->set_dead(true);
    // the mounts milestone, stage 3: the dead player is dismounted (the death frame below restarts its pawn) and its mount is desummoned
    if (mounted()) dismount(conn, now, DismountWhy::PlayerDied);
    if (mounts_ && abilities_) mounts_->desummon(ability_seconds(now));
    const GoodPosition at = checker_.good();
    death_x_ = at.x;
    death_y_ = at.y;
    death_z_ = at.z;
    loot_actor_ = 0;
    vendor_actor_ = 0;   // the windows close with the death: a purchase or a loot take from a dead character is refused (E1 review A4)
    // the frame of the retail captures: where it fell, the restart of the pawn, the pawn dead with no Health and no Mana
    set_current_value(content::kGuidMana, 0.0f);
    pb_.set_dead(true);
    const double x = at.x, y = at.y, z = at.z;
    queue("ClientSetLastDeathLocation", [x, y, z](bootstrap::PlayerBootstrap& pb) { return pb.death_location(x, y, z); }, conn, /*must_build=*/true);
    queue("ClientRestart (death)", [](bootstrap::PlayerBootstrap& pb) { return pb.death_restart(); }, conn, /*must_build=*/true);
    // what it costs: the experience debt and the materials of the bag (about half of each stack), which fall into a pile where it died
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    int64_t debt_added = 0;
    if (curve) {
        const leveling::Progress next = leveling::with_death_debt(*curve, progress_, 0.02);
        debt_added = next.debt - progress_.debt;
        progress_ = next;
        progress_changed_ = true;
    }
    std::vector<combat::LootStack> lost;
    items::Outcome all;
    all.ok = true;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const bool loss_on = !npc_world_ || npc_world_->aftermath_config().death_loss;
    if (db && loss_on) {
        for (const svc::InventoryItem& it : inventory_.items()) {
            // Gathering stage 3: the materials are in the resource containers now (the Backpack holds them only for a bag that had no room at the login)
            if ((it.container != svc::ItemContainer::Backpack && !items::is_resource(it.container)) || it.quantity <= 0 || !is_material(*db, it.item_record_id)) continue;
            const int32_t take = it.quantity == 1 ? 1 : (it.quantity + 1) / 2;
            const items::Outcome c = inventory_.consume(it.container, it.slot, it.instance_guid, take);
            if (!c.ok) continue;
            all.events.insert(all.events.end(), c.events.begin(), c.events.end());
            combat::LootStack s;
            s.item = it.item_record_id;
            s.quantity = take;
            s.quality = it.quality;
            lost.push_back(s);
        }
    }
    if (!all.events.empty()) {
        inventory_dirty_ = true;
        inventory_changed_ = true;
        ++items_applied_;
        const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
        for (size_t at2 = 0; at2 < events.size(); at2 += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at2),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at2 + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent (death)", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    queue("pawn state (death)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("death"); }, conn, /*must_build=*/true);
    if (curve) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        queue("XPTracker (death)", [total, debt](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            return pb.leveling_update();
        }, conn, /*must_build=*/true);
        hand_over_progress("a death");
    }
    // the pile
    pile_actor_ = 0;
    if (!lost.empty() && npc_world_ && npc_world_->aftermath()) {
        const std::array<uint32_t, 4>& g = pb_.setup().character_guid;
        std::string err;
        pile_actor_ = npc_world_->aftermath()->drop_pile(x, y, z, repl::GuidValue{g[0], g[1], g[2], g[3]}, pb_.setup().name, lost, npc_world_->world_clock(), err);
        if (pile_actor_ == 0) spdlog::warn("[session] {}: the pile of the death is not made: {} ({} stack(s) of materials are lost)", name(), err, lost.size());
    }
    // the bag goes to the saver with the death, not at the next periodic save: a crash within the minute would give the materials back to this
    // character while the pile's taker keeps them too (E1 review A5)
    if (inventory_dirty_) hand_over_inventory("a death", /*periodic=*/false);
    spdlog::log(level_for(death_logged_), "[session] {}: died at ({:.0f}, {:.0f}, {:.0f}): experience debt +{} (now {}), {} stack(s) of materials lost{}", name(), x, y, z, debt_added, progress_.debt,
                lost.size(), pile_actor_ != 0 ? " into a pile where it fell" : "");
    (void)now;
}
// -- C | O | A & Cyb --

void PlayerSession::on_respawn_choices_request(Connection& conn, const repl::ClientRpc&, Clock::time_point now) {
    if (!dead_) return;
    if (!within_budget(loot_budget_, "RequestServerRespawnChoices", now) || !room_for(1)) return;
    const int64_t record = kRespawnRecord;
    spdlog::log(level_for(death_logged_), "[session] {}: asks where it may come back: the nearest respawn point", name());
    queue("SendClientRespawnChoices", [record](bootstrap::PlayerBootstrap& pb) {
        const int64_t list[1] = {record};
        return pb.respawn_choices(std::span<const int64_t>(list, 1), record);
    }, conn, /*must_build=*/true);
}

void PlayerSession::on_respawn_choice(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!dead_) return;
    if (!within_budget(loot_budget_, "SendServerRespawnChoice", now) || !room_for(4)) return;
    const auto it = rpc.call.params.values.find(1);
    const int64_t* chosen = it == rpc.call.params.values.end() ? nullptr : it->second.get_if<int64_t>();
    if (!chosen || *chosen != kRespawnRecord) {
        // a record the session did not offer: the choices again, marked invalid
        queue("SendClientRespawnChoices", [](bootstrap::PlayerBootstrap& pb) {
            const int64_t list[1] = {kRespawnRecord};
            return pb.respawn_choices(std::span<const int64_t>(list, 1), kRespawnRecord, /*previous_choice_invalid=*/true);
        }, conn, /*must_build=*/true);
        return;
    }
    const PlayerWorld::RespawnSpot spot = npc_world_ ? npc_world_->respawn_spot(death_x_, death_y_) : PlayerWorld::RespawnSpot{death_x_, death_y_, death_z_, 0, false, "where it fell"};
    // alive again: full Health and Mana, the pawn not dead, the character at the spot (the move checker moves with it, the client is snapped)
    dead_ = false;
    ++respawns_;
    if (pipeline_) pipeline_->set_dead(false);
    pb_.set_dead(false);
    const stats::StatValue* hp = stats_ ? stats_->find(content::kGuidMaxHealth) : nullptr;
    const stats::StatValue* mp = stats_ ? stats_->find(content::kGuidMaxMana) : nullptr;
    if (hp) set_current_value(content::kGuidHealth, hp->value);
    if (mp) set_current_value(content::kGuidMana, mp->value);
    checker_.teleport(spot.x, spot.y, spot.z, spot.yaw);
    const float ts = checker_.good().has_time_stamp ? checker_.good().time_stamp : 0.0f;
    // The death's ClientRestart restarted the client's pawn, and with it its movement clock (the TimeStamp of every move starts again near 0): the checker's last good TimeStamp is
    // from the old clock, and every move below it would be dropped silently as TimeStampNotRising for as long as the old clock had run (the user's death of 2026-10-10: 66 s frozen
    // at the shrine, then snapped back).  The respawn forgets it; the correction below still names the old stamp.
    checker_.restart_clock();
    const uint8_t mode = checker_.good().movement_mode == 0 ? repl::kMoveWalking : checker_.good().movement_mode;
    queue("pawn state (respawn)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("respawn"); }, conn, /*must_build=*/true);
    queue("respawn teleport", [ts, spot, mode](bootstrap::PlayerBootstrap& pb) { return pb.correction(ts, repl::Vec3d{spot.x, spot.y, spot.z}, repl::Vec3d{}, mode); }, conn,
          /*must_build=*/true);
    spdlog::log(level_for(death_logged_), "[session] {}: comes back at {} ({:.0f}, {:.0f}, {:.0f}) with full Health and Mana", name(), spot.name, spot.x, spot.y, spot.z);
    (void)now;
}

void PlayerSession::apply_kill(Connection& conn, const combat::KillReward& k, Clock::time_point now) {
    ++kills_rewarded_;
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    const std::span<const content::AutoGrant> grants = world_ ? world_->auto_grants() : std::span<const content::AutoGrant>();
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (curve && k.adventuring_xp > 0) {
        xp = leveling::add_xp(*curve, grants, progress_, k.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (leveled) finish_level_up(xp, false, now, rank_effects, skill_updates);
    if (xp.changed() || leveled) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        queue("XPTracker", [total, debt](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            return pb.leveling_update();
        }, conn, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn);
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, /*must_build=*/true);
    }
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, /*must_build=*/true);
    }
    spdlog::log(level_for(use_logged_), "[session] {}: killed {} (level {}): Adventuring +{} XP{}{} (now {} XP, debt {}){}", name(), k.name.empty() ? k.blueprint : k.name, k.level,
                k.adventuring_xp, k.weapon_xp > 0 ? ", weapon lines +" + std::to_string(k.weapon_xp) + " (not modelled)" : "", leveled ? ", level " + std::to_string(xp.after.level) : "",
                progress_.xp, progress_.debt, k.loot ? "; there is loot" : "");
    if (xp.changed()) hand_over_progress(leveled ? "a level-up" : "XP earned");
}
// -- Cyb x C | O | A --

void PlayerSession::open_loot(Connection& conn, const combat::Corpse& corpse, const world::WorldActor& actor, Clock::time_point now) {
    const auto ignore = [&](const std::string& why) {
        ++loot_refused_;
        spdlog::log(level_for(loot_logged_), "[session] {}: the loot of {} is not opened: {}", name(), corpse.name, why);
    };
    if (!within_budget(loot_budget_, "a loot interaction", now)) return;
    if (!corpse.parts || corpse.remaining() == 0) return ignore("there is nothing left in it");
    if (!corpse.is_pile && !corpse.rights.count(pb_.setup().guids.pawn.ObjectId)) return ignore("the player did not fight it");
    const GoodPosition at = checker_.good();
    if (std::hypot(actor.x - at.x, actor.y - at.y) > kInteractRangeCm) return ignore("it is " + std::to_string(static_cast<int>(std::hypot(actor.x - at.x, actor.y - at.y) / 100.0)) + " m away");
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || !room_for(2)) return;
    repl::StoragePayload data;
    data.maximum_items = static_cast<int32_t>(corpse.loot.size());
    data.client_retrieve_only = true;
    const repl::GuidValue me{pb_.setup().character_guid[0], pb_.setup().character_guid[1], pb_.setup().character_guid[2], pb_.setup().character_guid[3]};
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (size_t i = 0; i < corpse.loot.size(); ++i) {
        repl::StorageSlot slot;
        const combat::LootStack& s = corpse.loot[i];
        if (!s.taken) {
            std::string err;
            const auto rec = db->item(s.item, err);
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = s.item;
            it.instance_guid = items::guid_hex(loot_guid(corpse.npc, i));
            it.quantity = static_cast<int32_t>(std::min<int64_t>(s.quantity, 1 << 20));
            it.quality = s.quality;
            it.durability = rec ? static_cast<float>(std::floor(rec->max_durability + 0.5)) : 0.0f;
            it.created_at_ms = created;
            slot.item = items::to_instance(it);
            slot.slot = static_cast<int32_t>(i);
            slot.check_loot_permission = true;
            slot.loot_permission.push_back(me);
        }
        data.storage.push_back(std::move(slot));
    }
    const FIntrepidNetworkGUID loot_actor_guid = actor.actor.guid;
    loot_actor_ = corpse.npc;
    ++loot_opens_;
    spdlog::log(level_for(loot_logged_), "[session] {}: opens the loot of {} ({} stack(s))", name(), corpse.name, corpse.remaining());
    queue("ClientReceiveLootStoragePayload", [loot_actor_guid, data](bootstrap::PlayerBootstrap& pb) { return pb.loot_payload(loot_actor_guid, data); }, conn, /*must_build=*/true);
}

void PlayerSession::on_loot_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto events = repl::parse_loot_events(rpc.call);
    if (!events) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerRequestLootStorageEvents does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(loot_budget_, "ServerRequestLootStorageEvents", now)) return;
    const auto refuse = [&](const std::string& why) {
        ++loot_refused_;
        spdlog::log(level_for(loot_logged_), "[session] {}: a loot request is refused: {}", name(), why);
    };
    combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!after || !db || !registry_) return refuse("the world has no loot");
    if (loot_actor_ == 0) return refuse("no loot window is open");
    const combat::Corpse* corpse = after->corpse_of(loot_actor_);
    const world::WorldActor* actor = registry_->find(loot_actor_);
    if (!corpse || !corpse->parts || !actor) {
        loot_actor_ = 0;
        return refuse("the corpse is gone");
    }
    const GoodPosition at = checker_.good();
    if (std::hypot(actor->x - at.x, actor->y - at.y) > kInteractRangeCm) {
        loot_actor_ = 0;
        return refuse("the corpse is too far");
    }
    if (!corpse->is_pile && !corpse->rights.count(pb_.setup().guids.pawn.ObjectId)) return refuse("the player may not take from it");
    if (events->empty() || events->size() > kMaxLootTakes) return refuse(events->empty() ? "no events" : "too many events");
    if (!room_for(4)) return;

    // what the client asks for, checked event by event: the container is the corpse's, the slot holds a stack that is still there, the item is that stack
    std::vector<size_t> slots;
    for (const repl::ItemStorageEvent& e : *events) {
        if (e.move_source.object.ObjectId != corpse->parts->container || e.move_source.slots.size() != 1) return refuse("an event names another container");
        const repl::StorageEventSlot& s = e.move_source.slots[0];
        if (s.slot < 0 || static_cast<size_t>(s.slot) >= corpse->loot.size()) return refuse("an event names a slot that is none");
        const size_t slot = static_cast<size_t>(s.slot);
        if (corpse->loot[slot].taken) return refuse("a stack was taken already");
        if (!e.item.has_item || e.item.record_id != static_cast<uint64_t>(corpse->loot[slot].item) || !(e.item.guid == loot_guid(corpse->npc, slot))) {
            return refuse("an event's item is not the stack of its slot");
        }
        if (std::find(slots.begin(), slots.end(), slot) != slots.end()) return refuse("a slot twice");
        slots.push_back(slot);
    }
    // the bag takes each stack or leaves it (a full bag leaves the rest in the corpse)
    std::vector<bootstrap::LootRemoval> removed;
    items::Outcome all;
    all.ok = true;
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    size_t bag_full = 0;
    for (const size_t slot : slots) {
        const combat::LootStack* stack = after->take(corpse->npc, slot);
        if (!stack) continue;
        const auto rec = [&] {
            std::string err;
            return db->item(stack->item, err);
        }();
        const int32_t max_stack = rec ? std::max(1, rec->max_stack_size) : 1;
        std::vector<svc::InventoryItem> fresh;
        int64_t left = stack->quantity;
        while (left > 0) {
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = stack->item;
            it.instance_guid = fresh_guid();
            for (const svc::InventoryItem& other : fresh) {
                if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
            }
            it.quantity = static_cast<int32_t>(std::min<int64_t>(left, max_stack));
            it.quality = stack->quality;
            it.durability = rec ? static_cast<float>(std::floor(rec->max_durability + 0.5)) : 0.0f;
            it.created_at_ms = created;
            left -= it.quantity;
            fresh.push_back(std::move(it));
        }
        // Gathering stage 3: a stack goes where its kind says (a pile mixes both kinds) and stays in the corpse when that storage has no room for it
        const items::Outcome got = inventory_.grant(std::move(fresh), item_catalog());
        if (!got.ok) {
            after->put_back(corpse->npc, slot);
            ++bag_full;
            continue;
        }
        // the container's side: the stack leaves its slot (the same instance guid the window showed)
        svc::InventoryItem shown;
        shown.container = svc::ItemContainer::Backpack;
        shown.item_record_id = stack->item;
        shown.instance_guid = items::guid_hex(loot_guid(corpse->npc, slot));
        shown.quantity = static_cast<int32_t>(std::min<int64_t>(stack->quantity, 1 << 20));
        shown.quality = stack->quality;
        shown.durability = rec ? static_cast<float>(std::floor(rec->max_durability + 0.5)) : 0.0f;
        shown.created_at_ms = created;
        removed.push_back({static_cast<int32_t>(slot), items::to_instance(shown)});
        all.events.insert(all.events.end(), got.events.begin(), got.events.end());
        ++loot_taken_;
    }
    if (bag_full > 0) spdlog::log(level_for(loot_logged_), "[session] {}: the bag has no room for {} stack(s): they stay in the corpse", name(), bag_full);
    if (removed.empty()) return;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    // retail's order: the removal from the loot container, the add to the bag, and the window closes when nothing is left
    const FIntrepidNetworkGUID container = corpse->parts->container_guid;
    queue("ClientReceiveStorageEvent (loot)", [container, removed](bootstrap::PlayerBootstrap& pb) { return pb.loot_removed(container, removed); }, conn, /*must_build=*/true);
    const std::vector<bootstrap::ItemEvent> adds = item_events_for(all);
    for (size_t at2 = 0; at2 < adds.size(); at2 += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(adds.begin() + static_cast<std::ptrdiff_t>(at2),
                                                     adds.begin() + static_cast<std::ptrdiff_t>(std::min(adds.size(), at2 + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::log(level_for(loot_logged_), "[session] {}: takes {} stack(s) from {}: {} storage event(s) into the bag", name(), removed.size(), corpse->name, all.events.size());
    if (corpse->remaining() == 0) {
        const FIntrepidNetworkGUID loot_actor_guid = actor->actor.guid;
        queue("ClientRemoveListenerFromStorage", [loot_actor_guid](bootstrap::PlayerBootstrap& pb) { return pb.loot_listener_removed(loot_actor_guid); }, conn, /*must_build=*/true);
        after->emptied(corpse->npc, npc_world_->world_clock());
        loot_actor_ = 0;
    }
}
// ~ C.O.A - C Y B ~

void PlayerSession::on_client_load(Connection& conn, Clock::time_point now) {
    if (!within_budget(load_budget_, "ServerRequestClientLoad", now) || !room_for(8)) return;
    spdlog::log(level_for(load_logged_), "[session] {}: the client asks for its load: storage payloads, group and action bar", name());
    std::vector<bootstrap::StorageKind> kinds = {bootstrap::StorageKind::Backpack, bootstrap::StorageKind::Equipment, bootstrap::StorageKind::QuestStorage};
    for (int i = 0; i < items::kResourceContainers; ++i) {
        if (pb_.setup().resource_bags[static_cast<size_t>(i)].present) kinds.push_back(bootstrap::resource_kind(i));   // Gathering stage 3: the container of every worn bag
    }
    for (const bootstrap::StorageKind k : kinds) {
        queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::PlayerController); },
              conn);
    }
    queue("ClientUpdateGroup", [](bootstrap::PlayerBootstrap& pb) { return pb.empty_group(); }, conn);
    if (book_) {
        // plan D1, stage 2: the bar the character arranged, or the default (the weapon bank and the class's skills), once, as retail's
        const repl::ActionbarPayload payload = bar_payload();
        queue("ClientReceiveActionbarSlotPayload", [payload](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(payload); }, conn);
    } else {
        queue("ClientReceiveActionbarSlotPayload", [](bootstrap::PlayerBootstrap& pb) { return pb.default_action_bar(); }, conn);
    }
}


// ============================================================================================================================
//  talents, the action bar and regeneration (plan D1, stage 2)
// ============================================================================================================================

double PlayerSession::session_seconds(Clock::time_point now) {
    if (!origin_set_) {
        origin_ = now;
        origin_set_ = true;
    }
    return std::chrono::duration<double>(now - origin_).count();
}

repl::ActionbarPayload PlayerSession::bar_payload() const {
    repl::ActionbarPayload p;
    p.slots = bar_.wire_slots();   // the weapon bank three times, as retail lists it
    p.auto_granted = false;
    return p;
}

bool PlayerSession::push_abilities() {
    if (pipeline_) pipeline_->set_known(known_);   // plan D2: what the character may trigger follows what the client was told it knows
    std::vector<bootstrap::AbilityRecord> records;
    records.reserve(records_.size());
    for (const int64_t g : records_) records.push_back({g, content::kTypeAbilityRecord});
    if (!pb_.set_abilities(known_, records)) {
        spdlog::debug("[session] {}: the pawn does not take the ability lists yet", name());
        return false;
    }
    return true;
}

void PlayerSession::rebuild_ability_lists() {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!book_ || !db) return;
    const talents::AbilityLists lists =
        talents::join_abilities(*book_, bootstrap::general_unstuck(), combo_, bootstrap::carried_combos(*db, inventory_), bootstrap::static_known_tail(),
                                bootstrap::carried_item_abilities(world_->mounts() ? &world_->mounts()->data() : nullptr, inventory_));
    known_ = lists.known;
    records_ = lists.records;
}

void PlayerSession::sync_item_abilities(Connection& conn) {
    if (!world_ || !world_->mounts() || !phase_in_done_ || !join_.empty() || finished_ || failed_ || abilities_failed_) return;
    const std::vector<int64_t> carried = bootstrap::carried_item_abilities(&world_->mounts()->data(), inventory_);
    std::vector<int64_t> missing;
    for (const int64_t a : carried) {
        if (std::find(records_.begin(), records_.end(), a) == records_.end()) missing.push_back(a);
    }
    if (missing.empty() || !room_for(1)) return;   // (no room now: the next tick tries again)
    for (const int64_t a : missing) records_.push_back(a);
    if (!push_abilities()) {
        for (const int64_t a : missing) records_.erase(std::remove(records_.begin(), records_.end(), a), records_.end());
        return;
    }
    ++item_ability_syncs_;
    spdlog::log(level_for(item_ability_logged_),
                "[session] {}: {} item ability record(s) added to ReplicatedRecordsToLoad (a mount item is carried: the client needs Summon_Mount loaded to send the summon)", name(),
                missing.size());
    queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("item abilities"); }, conn);
}

void PlayerSession::add_rank_effect(const talents::RankRef& ref, Clock::time_point now, bootstrap::AbilityChange& effects) {
    const talents::Rank& rank = *ref.rank;
    if (rank.effect == 0 || rank_effects_.count(rank.guid)) return;
    // a weapon rank's passive applies while the weapon of its line is in hand (retail sent no item for a line that was not wielded)
    if (ref.tree->kind == talents::Kind::Weapon && ref.tree->weapon_line != wielded_line_) return;
    bootstrap::EffectItem e;
    e.effect_id = next_passive_effect_id_++;
    e.effect_guid = rank.effect;
    e.start_time = bootstrap::effect_clock(pb_.setup().world_time + session_seconds(now));
    e.duration = rank.effect_infinite ? std::numeric_limits<float>::max() : static_cast<float>(rank.effect_duration);
    e.tick_time = 0;
    e.stacks = 1;
    effects.effects_added.push_back(e);
    rank_effects_[rank.guid] = e.effect_id;
    // a marker effect (0.1 s of a weapon rank, 1 s of a Universal rank) is ended by the server
    if (!rank.effect_infinite) {
        markers_.push_back({now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(rank.effect_duration)), e.effect_id});
    }
}
// CoA captures, C-Y-B code

void PlayerSession::remove_rank_effect(int64_t rank_guid, bootstrap::AbilityChange& effects) {
    const auto it = rank_effects_.find(rank_guid);
    if (it == rank_effects_.end()) return;
    const int64_t key = it->second;
    rank_effects_.erase(it);
    markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == key; }), markers_.end());
    // an item this same change adds and that is not on the pawn yet: it never was
    const auto added = std::find_if(effects.effects_added.begin(), effects.effects_added.end(), [&](const bootstrap::EffectItem& e) { return e.effect_id == key; });
    if (added != effects.effects_added.end()) {
        effects.effects_added.erase(added);
        return;
    }
    effects.effects_removed.push_back(key);
}

void PlayerSession::grant_ranks(const std::vector<talents::RankRef>& granted, Clock::time_point now, bootstrap::AbilityChange& effects,
                                std::vector<repl::SkillOnActionbar>& updates) {
    bool lists_changed = false;
    for (const talents::RankRef& r : granted) {
        if (r.rank->ability != 0) {
            if (std::find(known_.begin(), known_.end(), r.rank->ability) == known_.end()) {
                known_.push_back(r.rank->ability);   // an append, as retail's: the live order is not the join's
                lists_changed = true;
            }
            if (std::find(records_.begin(), records_.end(), r.rank->ability) == records_.end()) {
                records_.push_back(r.rank->ability);
                lists_changed = true;
            }
        }
        add_rank_effect(r, now, effects);
        if (r.node->auto_hotbar) {
            if (r.rank->ability != 0 && bar_.place_skill(r.rank->ability, r.rank->guid)) progress_changed_ = true;   // a design choice (action_bar.h)
            repl::SkillOnActionbar u;
            u.skill_id = r.node->skill;
            u.skill_tree_id = r.tree->guid;
            u.rank_id = 0;
            u.is_refunding = false;
            u.use_hint = true;
            updates.push_back(u);
        }
    }
    if (lists_changed) push_abilities();
}

void PlayerSession::switch_class(const content::ClassRecord& cls, Clock::time_point now, bootstrap::AbilityChange& effects,
                                 std::vector<repl::SkillOnActionbar>& updates, std::vector<std::pair<int, int>>& cleared) {
    (void)updates;   // the whole bar goes in one payload (the caller), not a skill update per rank
    const talents::Tree* old_tree = book_->class_tree();
    std::set<int64_t> old_ranks;
    if (old_tree) {
        for (const talents::RankRef& r : book_->listed(*old_tree)) {
            old_ranks.insert(r.rank->guid);
            remove_rank_effect(r.rank->guid, effects);
        }
    }
    book_->set_class(cls.guid);
    const talents::Tree* new_tree = book_->class_tree();
    // the trees: the old class's list leaves the client's map, the new one's comes (each class keeps its bought ranks under its own tree)
    if (old_tree && old_tree != new_tree) pb_.set_tree_ranks(old_tree->guid, {});
    std::vector<talents::RankRef> listed;
    if (new_tree) {
        pb_.set_tree_ranks(new_tree->guid, book_->list(*new_tree));
        listed = book_->listed(*new_tree);
    }
    // the bar: the old class's skills leave, the new class's are placed
    for (const auto& slot : bar_.remove_skills(old_ranks)) cleared.push_back(slot);
    for (const talents::RankRef& r : listed) {
        add_rank_effect(r, now, effects);
        if (r.node->auto_hotbar && r.rank->ability != 0) bar_.place_skill(r.rank->ability, r.rank->guid);
    }
    // the abilities: the join's lists for the new class (the old class's go, the new class's come)
    rebuild_ability_lists();
    push_abilities();
    progress_changed_ = true;
}

void PlayerSession::refuse_purchase(Connection& conn, const repl::SkillBuild& build, const std::string& why, Clock::time_point now) {
    ++talents_refused_;
    spdlog::log(level_for(talent_refused_logged_), "[session] {}: ServerPurchaseSkillBuild ({} change(s)) refused, nothing changed: {}", name(), build.changes.size(), why);
    // the client is corrected: the unchanged list of each tree the request names (and the class tree when it names none we know), under the
    // resync budget
    if (!resync_budget_.take(now) || !room_for(1)) return;
    const talents::Catalog& catalog = book_->catalog();
    std::set<int64_t> trees;
    for (const repl::SkillRankChange& c : build.changes) {
        if (trees.size() >= 4) break;
        if (catalog.tree(c.tree.guid)) trees.insert(c.tree.guid);
    }
    if (trees.empty()) {
        if (const talents::Tree* ct = book_->class_tree()) trees.insert(ct->guid);
    }
    bool any = false;
    for (const int64_t t : trees) {
        const talents::Tree* tree = catalog.tree(t);
        if (!tree) continue;
        const std::vector<int64_t> list = book_->list(*tree);
        if (list.empty()) continue;
        if (!pb_.resend_tree(t)) pb_.set_tree_ranks(t, list);   // no item yet: it is added
        any = true;
    }
    if (any) queue("XPTracker", [](bootstrap::PlayerBootstrap& pb) { return pb.leveling_update(); }, conn);
}

void PlayerSession::on_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!book_) return;
    const auto build = repl::parse_purchase_skill_build(rpc.call);
    if (!build) {
        if (!talents_unreadable_logged_) {
            talents_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerPurchaseSkillBuild does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(talent_budget_, "ServerPurchaseSkillBuild", now)) return;
    // the answer's steps: the pawn's update, the XPTracker and a skill update for each bought rank; a request that cannot be answered whole is refused
    const size_t purchases = build->purchases();
    if (purchases + 3 > kMaxPendingReplies) return refuse_purchase(conn, *build, "more ranks than one reply can announce", now);
    if (!room_for(purchases + 3)) return;
    const talents::Plan plan = book_->plan(*build);
    if (!plan.ok) return refuse_purchase(conn, *build, plan.refusal, now);

    // ---- the state: what the ranks take, what they give
    bootstrap::AbilityChange effects;
    std::vector<repl::SkillOnActionbar> updates;
    bool lists_changed = false;
    bool bar_changed = false;
    for (const talents::Change& c : plan.changes) {
        if (c.purchase) continue;
        remove_rank_effect(c.ref.rank->guid, effects);
        if (c.ref.rank->ability != 0) {
            const int64_t a = c.ref.rank->ability;
            known_.erase(std::remove(known_.begin(), known_.end(), a), known_.end());
            records_.erase(std::remove(records_.begin(), records_.end(), a), records_.end());
            lists_changed = true;
        }
        if (!bar_.remove_skills({c.ref.rank->guid}).empty()) bar_changed = true;   // the client clears its own bar on a refund
    }
    book_->apply(plan);
    for (const talents::Change& c : plan.changes) {
        if (!c.purchase) continue;
        if (c.ref.rank->ability != 0) {
            const int64_t a = c.ref.rank->ability;
            if (std::find(known_.begin(), known_.end(), a) == known_.end()) known_.push_back(a);   // an append: the live order is not the join's
            if (std::find(records_.begin(), records_.end(), a) == records_.end()) records_.push_back(a);
            lists_changed = true;
        }
        add_rank_effect(c.ref, now, effects);
        if (c.ref.node->auto_hotbar) {
            if (c.ref.rank->ability != 0 && bar_.place_skill(c.ref.rank->ability, c.ref.rank->guid)) bar_changed = true;   // a design choice (action_bar.h)
            repl::SkillOnActionbar u;
            u.skill_id = c.ref.node->skill;
            u.skill_tree_id = c.ref.tree->guid;
            u.rank_id = 0;
            u.is_refunding = false;
            u.use_hint = true;
            updates.push_back(u);
        }
    }
    (void)bar_changed;
    for (const int64_t t : plan.trees) {
        const talents::Tree* tree = book_->catalog().tree(t);
        if (tree && !pb_.set_tree_ranks(t, book_->list(*tree))) spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", name(), tree->name);
    }
    if (lists_changed) push_abilities();
    if (!effects.empty()) {
        if (!pb_.apply_ability_change(effects)) {
            spdlog::warn("[session] {}: the pawn does not take the rank effects' items ({}): the ranks stand without them", name(),
                         pb_.error().empty() ? "keys disagree" : pb_.error());
            for (const bootstrap::EffectItem& e : effects.effects_added) {
                for (auto it = rank_effects_.begin(); it != rank_effects_.end();) it = it->second == e.effect_id ? rank_effects_.erase(it) : std::next(it);
                markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == e.effect_id; }), markers_.end());
            }
        }
    }
    apply_gear_stats("the talents");   // the ranks' constant mods are an input of the stats
// C:Y:B + CoA

    // ---- what the client is told, in the retail order: the pawn's update (abilities, effects, stats), the XPTracker's lists, the skill updates
    ++talents_applied_;
    // The reply goes out purchase_delay_ later (retail's 84 ms median): the state above is applied now, the steps are built when they go.
    queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("talents"); }, conn, now);
    queue_later("XPTracker", [](bootstrap::PlayerBootstrap& pb) { return pb.leveling_update(); }, conn, now, /*must_build=*/true);
    for (const repl::SkillOnActionbar& u : updates) {
        queue_later("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, now, /*must_build=*/true);
    }
    // the client reports each skill it puts on its bar with a ServerSetActionbarSlotUseData, all in one frame: they are not the bar budget's
    bar_allow_ = static_cast<uint32_t>(std::min<size_t>(kBarPurchaseAllowance, static_cast<size_t>(bar_allow_) + updates.size()));
    size_t bought = 0, refunded = 0;
    for (const talents::Change& c : plan.changes) (c.purchase ? bought : refunded) += 1;
    spdlog::log(level_for(talent_logged_), "[session] {}: ServerPurchaseSkillBuild: {} rank(s) bought, {} refunded{}; {} effect item(s) added, {} removed; KnownAbilities {}",
                name(), bought, refunded, build->refund_only() ? " (a respec)" : "", effects.effects_added.size(), effects.effects_removed.size(), known_.size());
    progress_changed_ = true;
    hand_over_progress("a talent change");
}

void PlayerSession::on_set_slot(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!book_) return;
    const auto slot = repl::parse_set_actionbar_slot(rpc.call);
    if (!slot) {
        if (!talents_unreadable_logged_) {
            talents_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerSetActionbarSlotUseData does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (bar_allow_ > 0) --bar_allow_;   // a Set a purchase made the client send
    else if (!within_budget(bar_budget_, "ServerSetActionbarSlotUseData", now)) return;
    const std::optional<std::pair<int, int>> server_slot = bar_.first_free();   // where the server puts a bought skill, for the log
    std::string why;
    if (!bar_.set(*slot, &why)) {
        ++bar_refused_;
        spdlog::log(level_for(bar_refused_logged_), "[session] {}: ServerSetActionbarSlotUseData (bank {}, slot {}) refused, nothing changed: {}", name(), slot->bank_index,
                    slot->slot_index, why);
        return;   // retail answers none, and nothing here is wrong with the client's own bar
    }
    // a skill is on the class pages once: the client moved it, not copied it
    if (slot->use.use_type == repl::ActionbarUseType::Skill && slot->use.skill_rank_id != 0 && slot->bank_index < actionbar::kClassBanks) {
        bar_.remove_other_copies(slot->use.skill_rank_id, slot->bank_index, slot->slot_index);
    }
    ++bar_sets_;
    // a probe of the D1 draft (the bar animation): every Set at info, with what the server's own placement would have been
    ++bar_logged_;
    spdlog::info("[session] {}: the client set action bar bank {} slot {} (use type {}, ability {}, rank {}): stored; the server's first free class slot was {}", name(),
                 slot->bank_index, slot->slot_index, static_cast<unsigned>(slot->use.use_type), slot->use.ability_id, slot->use.skill_rank_id,
                 server_slot ? std::to_string(server_slot->first) + "/" + std::to_string(server_slot->second) : std::string("none"));
    progress_changed_ = true;
    hand_over_progress("the action bar");
}

void PlayerSession::on_set_ranged(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!book_ || !world_ || !world_->content()) return;
    const auto use = repl::parse_set_use_ranged_weapon(rpc.call);
    if (!use) return;
    if (!within_budget(item_budget_, "ServerSetUseRangedWeapon", now) || !room_for(2)) return;
    if (*use == use_ranged_) return;
    use_ranged_ = *use;
    ++weapon_swaps_;
    // E1 stage 3 fix: the pawn's CombatInfo.bIsUsingRangedWeapon follows the request (retail 403, every swap: an 18-bit UPlayerCombatInfo block in the
    // same chain as the AbilityComponent's, the StatsComponent's and OwnerInfo's); the client's weapon in hand follows this flag, not KnownAbilities
    if (!pb_.set_using_ranged_weapon(use_ranged_)) spdlog::warn("[session] {}: the pawn does not take CombatInfo.bIsUsingRangedWeapon", name());
    const auto weapon = bootstrap::worn_weapon(*world_->content(), inventory_, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
    if (!weapon) {
        spdlog::info("[session] {}: the weapon set is the {} one now and nothing is worn in its slot: the combo stays", name(), use_ranged_ ? "ranged" : "melee");
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("weapon swap"); }, conn);
        return;
    }
    // KnownAbilities: the old combo leaves, the new one is the last element; the records: the new one is the last (the old one stays carried)
    if (combo_ != 0) known_.erase(std::remove(known_.begin(), known_.end(), combo_), known_.end());
    known_.erase(std::remove(known_.begin(), known_.end(), weapon->combo), known_.end());
    known_.push_back(weapon->combo);
    records_.erase(std::remove(records_.begin(), records_.end(), weapon->combo), records_.end());
    records_.push_back(weapon->combo);
    combo_ = weapon->combo;
    wielded_line_ = weapon->line;
    push_abilities();
    pb_.set_weapon_skill_line(weapon->line, weapon->line_name);
    apply_gear_stats("the weapon swap");   // a weapon tree's constant mods apply while its line is in hand
    spdlog::info("[session] {}: the {} weapon set: combo {} is the last known ability, weapon line {}", name(), use_ranged_ ? "ranged" : "melee", weapon->combo,
                 weapon->line_name);
    queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("weapon swap"); }, conn);
}

void PlayerSession::run_timers(Connection& conn, Clock::time_point now) {
    if (failed_ || finished_ || !join_.empty()) return;   // nothing before the join is on the wire
    // the replies whose time has come, in the order they were asked for
    while (!delayed_.empty() && delayed_.front().due <= now) {
        Delayed d = std::move(delayed_.front());
        delayed_.pop_front();
        queue(d.step.what, std::move(d.step.build), conn, d.step.must_build);
    }
    // marker effects end by themselves
    if (!markers_.empty()) {
        bootstrap::AbilityChange c;
        for (auto it = markers_.begin(); it != markers_.end();) {
            if (it->due > now) {
                ++it;
                continue;
            }
            c.effects_removed.push_back(it->key);
            for (auto r = rank_effects_.begin(); r != rank_effects_.end();) r = r->second == it->key ? rank_effects_.erase(r) : std::next(r);
            it = markers_.erase(it);
        }
        if (!c.empty()) {
            if (pb_.apply_ability_change(c)) abilities_dirty_ = true;
            else spdlog::warn("[session] {}: the pawn does not take the end of {} marker effect(s) ({})", name(), c.effects_removed.size(), pb_.error().empty() ? "keys disagree" : pb_.error());
        }
    }
    if (abilities_dirty_) flush_abilities(conn);
}

// ============================================================================================================================
//  the GM travel tools (session/gm_travel.h)
// ============================================================================================================================

namespace {
// [C | Y | B - CoA]

/// The value of the RPC parameter named `name`, null when the call was not decoded or the parameter is absent (a parameter at its default is not sent).
const repl::Value* rpc_value(const repl::ClientRpc& rpc, std::string_view name) {
    if (!rpc.call.decoded || !rpc.call.rpc) return nullptr;
    const schema::RepCommand* c = repl::rpc_command(*rpc.call.rpc, name);
    if (!c) return nullptr;
    const auto it = rpc.call.params.values.find(c->handle);
    return it == rpc.call.params.values.end() ? nullptr : &it->second;
}

/// A numeric parameter, whatever its width: 0 when absent.
double number_value(const repl::Value* v) {
    if (!v) return 0.0;
    if (const auto* i = v->get_if<int64_t>()) return static_cast<double>(*i);
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<double>(*u);
    if (const auto* d = v->get_if<double>()) return *d;
    if (const auto* f = v->get_if<float>()) return static_cast<double>(*f);
    return 0.0;
}

std::string text_value(const repl::Value* v) {
    if (!v) return {};
    if (const auto* s = v->get_if<std::string>()) return *s;
    return {};
}

const char* gm_refusal(PlayerSession::GmMoveResult::Status s) {
    switch (s) {
        case PlayerSession::GmMoveResult::Status::Dead: return "the player is dead";
        case PlayerSession::GmMoveResult::Status::OutsideWorld: return "outside the world";
        case PlayerSession::GmMoveResult::Status::Busy: return "the reply queue is full";
        case PlayerSession::GmMoveResult::Status::Moved: break;
    }
    return "moved";
}

}  // namespace

PlayerSession::GmMoveResult PlayerSession::gm_teleport(Connection& conn, double x, double y, double z, Clock::time_point now, const std::string& by) {
    GmMoveResult r;
    r.from = last_position();
    r.to = r.from;
    if (dead_) {
        r.status = GmMoveResult::Status::Dead;
        return r;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !base_limits_.world.contains(x, y, z)) {
        r.status = GmMoveResult::Status::OutsideWorld;
        return r;
    }
    // a dismount queues four steps (the restart, the controller, the detached pawn, the mount's update), the correction one more
    if (!room_for(mounted() ? 6 : 2)) {
        r.status = GmMoveResult::Status::Busy;
        return r;
    }
    if (mounted()) {
        dismount(conn, now, DismountWhy::GmTeleport);   // the ride is over; the mount stays where it was and desummons by distance (run_mounts)
        r.dismounted = true;
    }
    // the path a respawn uses: the checker moves with the player (the credits start over, the TimeStamp state stays) and the client is snapped there
    const double yaw = checker_.good().yaw_degrees;
    checker_.teleport(x, y, z, yaw);
    pending_fix_.reset();   // a correction a refused move left would put the client back where it was
    const float ts = checker_.good().has_time_stamp ? checker_.good().time_stamp : 0.0f;
    constexpr uint8_t mode = repl::kMoveFalling;   // the client lands and walks again by itself
    queue("GM teleport", [ts, x, y, z, mode](bootstrap::PlayerBootstrap& pb) { return pb.correction(ts, repl::Vec3d{x, y, z}, repl::Vec3d{}, mode); }, conn, /*must_build=*/true);
    ++gm_teleports_;
    r.to = checker_.good();
    spdlog::info("[gm] {}: teleported by {} from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}){}", name(), by, r.from.x, r.from.y, r.from.z, x, y, z,
                 r.dismounted ? "; dismounted first" : "");
    if (position_changed(/*with_yaw=*/false)) hand_over("a GM teleport", /*periodic=*/false);   // (a crash must not take the player back)
    return r;
}
// C_Y_B with C.O.A

double PlayerSession::gm_limit_scale(Clock::time_point now) const {
    double m = gm_speed_;
    if (now < gm_grace_until_) m = std::max(m, gm_grace_speed_);   // the client learns of a lower speed a trip later
    return std::max(1.0, m);
}

void PlayerSession::scale_pawn_limits(Clock::time_point now) {
    const double s = gm_limit_scale(now);
    checker_.set_speed_limits(base_limits_.max_speed * s, base_limits_.dash_speed * s);
}

void PlayerSession::apply_gm_speed(Connection* conn, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);   // (first: it starts the session's ability clock)
    const double t = ability_seconds(now);
    // the pawn's MoveSpeedMult stat: the one the Sprint's effect raises (the client moves at 600 cm/s times it).  The base the Sprint ends on is the GM's, so a Sprint in progress
    // keeps its +0.2 on top and the multiplier survives the end of it.
    const stats::StatValue* engine = stats_ ? stats_->find(spawns::kStatMoveSpeedMult) : nullptr;
    const double base = engine ? static_cast<double>(engine->value) : 1.0;
    if (!pb_.has_stat_item(spawns::kStatMoveSpeedMult)) {
        spdlog::warn("[gm] {}: the pawn has no MoveSpeedMult stat to carry the run speed: the move checker's limits scale, the client does not move faster", name());
    } else {
        bootstrap::AbilityChange change;
        if (runtime && runtime->tracks(spawns::kStatMoveSpeedMult)) {
            runtime->gear_stat(change, spawns::kStatMoveSpeedMult, base * gm_speed_);
        } else {
            bootstrap::StatChange sc;
            sc.record = spawns::kStatMoveSpeedMult;
            sc.value = static_cast<float>(base * gm_speed_);
            change.stats.push_back(sc);
        }
        if (!change.empty()) {
            if (pb_.apply_ability_change(change)) {
                abilities_dirty_ = true;
            } else {
                spdlog::warn("[gm] {}: the pawn does not take the run speed ({})", name(), pb_.error().empty() ? std::string("its stat has no item") : pb_.error());
            }
        }
    }
    // the mount (the same stat, the one the road's and the dash's effects raise), and the checker's limits
    if (ride_) ride_->set_gm_multiplier(gm_speed_, t);
    if (!mounted()) scale_pawn_limits(now);
    if (conn && abilities_dirty_) flush_abilities(*conn);
}

PlayerSession::GmSpeedChange PlayerSession::gm_set_speed(Connection& conn, double multiplier, Clock::time_point now, const std::string& by) {
    GmSpeedChange r;
    r.previous = gm_speed_;
    const double m = std::isfinite(multiplier) ? std::clamp(multiplier, 0.5, 10.0) : 1.0;
    if (m < gm_speed_) {
        // a lower speed: the checker keeps the old limit for a moment, while the client still moves at the old speed
        gm_grace_speed_ = std::max(gm_speed_, now < gm_grace_until_ ? gm_grace_speed_ : 0.0);
        gm_grace_until_ = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(Ride::kSpeedGraceSeconds));
    }
    gm_speed_ = m;
    apply_gm_speed(&conn, now);
    r.now = gm_speed_;
    spdlog::info("[gm] {}: run speed x{:g} (was x{:g}) by {}{}", name(), gm_speed_, r.previous, by, mounted() ? "; riding: the mount's MoveSpeedMult follows" : "");
    return r;
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

void PlayerSession::on_gm_teleport_xyz(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const double x = number_value(rpc_value(rpc, "X")), y = number_value(rpc_value(rpc, "Y")), z = number_value(rpc_value(rpc, "Z"));
    const double radius = number_value(rpc_value(rpc, "OptionalRadius"));
    char params[160];
    std::snprintf(params, sizeof params, "X=%.0f Y=%.0f Z=%.0f OptionalRadius=%g%s", x, y, z, radius, rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerTeleportXYZ", params, now) || !rpc.call.decoded) return;
    const GmMoveResult r = gm_teleport(conn, x, y, z, now, "ServerTeleportXYZ");
    if (r.status == GmMoveResult::Status::Moved) ++gm_rpcs_answered_;
    else spdlog::warn("[gm] {}: ServerTeleportXYZ to ({:.0f}, {:.0f}, {:.0f}) is refused ({})", name(), x, y, z, gm_refusal(r.status));
}

void PlayerSession::on_gm_teleport_key(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string key = text_value(rpc_value(rpc, "DestinationLookupKey"));
    const double radius = number_value(rpc_value(rpc, "OptionalRadius"));
    char params[256];
    std::snprintf(params, sizeof params, "DestinationLookupKey=\"%.100s\" OptionalRadius=%g%s", key.c_str(), radius, rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerTeleport", params, now) || !rpc.call.decoded) return;
    const world::Places places = world_->places();
    const world::Place* place = places.find(key);
    if (!place) {
        spdlog::warn("[gm] {}: ServerTeleport \"{}\": worldd knows no such place (the city nodes and \"login\"; worldctl places lists them)", name(), log_text(key));
        return;
    }
    const GmMoveResult r = gm_teleport(conn, place->x, place->y, world_->place_height(*place), now, "ServerTeleport");
    if (r.status == GmMoveResult::Status::Moved) ++gm_rpcs_answered_;
    else spdlog::warn("[gm] {}: ServerTeleport \"{}\" is refused ({})", name(), log_text(key), gm_refusal(r.status));
}
// C O A x Cyb

void PlayerSession::on_gm_teleport_player(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string names = text_value(rpc_value(rpc, "CharacterNames"));
    const double x = number_value(rpc_value(rpc, "X")), y = number_value(rpc_value(rpc, "Y")), z = number_value(rpc_value(rpc, "Z"));
    char params[256];
    std::snprintf(params, sizeof params, "CharacterNames=\"%.100s\" X=%.0f Y=%.0f Z=%.0f%s", names.c_str(), x, y, z, rpc.call.decoded ? "" : "; the parameters did not decode");
    if (!gm_rpc_admitted("ServerTeleportPlayer", params, now) || !rpc.call.decoded) return;
    size_t moved = 0;
    if (GmPort* port = world_ ? world_->gm_port() : nullptr) {
        moved = port->teleport_characters(names, x, y, z, "ServerTeleportPlayer by " + name(), now);
    } else if (iequals(names, name())) {   // (no port: a world with only this session)
        moved = gm_teleport(conn, x, y, z, now, "ServerTeleportPlayer").status == GmMoveResult::Status::Moved ? 1 : 0;
    }
    if (moved > 0) ++gm_rpcs_answered_;
    else spdlog::warn("[gm] {}: ServerTeleportPlayer \"{}\" moved no one", name(), log_text(names));
}

// ============================================================================================================================
//  Nodes 1, stage 2: the world knows its nodes
// ============================================================================================================================

void PlayerSession::note_node_request(std::deque<int64_t>& wait, std::set<int64_t>& queued, int64_t node, const char* what) {
    if (node == 0) {
        // the client's own "Requesting Node Data with invalid Node ID" is a request it does not send; a zero from elsewhere has nothing to answer
        spdlog::debug("[nodes] {}: {} for node 0: ignored", name(), what);
        return;
    }
    if (queued.count(node)) return;   // its answer is already waiting
    if (wait.size() >= kMaxNodeRequests) {
        ++node_requests_dropped_;
        if (!node_overflow_logged_) {
            node_overflow_logged_ = true;
            spdlog::warn("[nodes] {}: {} requests are waiting for their answers: further ones are dropped (said once)", name(), wait.size());
        }
        return;
    }
    queued.insert(node);
    wait.push_back(node);
}

void PlayerSession::on_node_summary_request(const repl::ClientRpc& rpc) {
    ++node_requests_;
    const auto node = repl::parse_node_summary_request(rpc.call);
    if (!node) {
        if (!node_unreadable_logged_) {
            node_unreadable_logged_ = true;
            spdlog::warn("[nodes] {}: a ServerRequestNodeSummary does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    note_node_request(node_summary_wait_, node_summary_queued_, *node, "ServerRequestNodeSummary");
}

void PlayerSession::on_siege_summary_request(const repl::ClientRpc& rpc) {
    ++node_requests_;
    const auto node = repl::parse_siege_summary_request(rpc.call);
    if (!node) {
        if (!node_unreadable_logged_) {
            node_unreadable_logged_ = true;
            spdlog::warn("[nodes] {}: a ServerRequestCurrentSiegeSummary does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    note_node_request(siege_wait_, siege_queued_, *node, "ServerRequestCurrentSiegeSummary");
}

void PlayerSession::on_siege_cooldowns_request() {
    ++node_requests_;
    cooldowns_pending_ = true;
}

// The citizenship window (ServerSetEssentialCitizenshipWindowData -> ClientPopulateCitizenshipData): its struct, FEssentialCitizenshipWindowData, is a NetSerialize struct
// whose layout is not decoded (33 to 219 kbit in the captures; evidence.md section 4.4).  A default cannot be written without it (UE always sends a struct parameter), and a
// guessed payload could make the client drop the connection, so the request is logged and not answered: the citizenship panel of the node window stays empty until the
// citizenship milestone decodes the struct.
void PlayerSession::on_citizenship_window_request() {
    ++citizenship_requests_;
    if (citizenship_logged_) return;
    citizenship_logged_ = true;
    spdlog::info("[nodes] {}: the client asks for the citizenship window's data (ServerSetEssentialCitizenshipWindowData): not answered, the struct's layout is not decoded yet "
                 "(the citizenship milestone); further ones are not logged", name());
}
// C.Y.B with CoA

void PlayerSession::run_nodes(Connection& conn) {
    if (finished_ || failed_ || !join_.empty()) return;   // (the join includes the pawn's second state: the client's world exists, as retail's answers assume)
    const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
    const world::NodeWire* wire = world_ ? world_->node_wire() : nullptr;
    // room for a reply, leaving the others theirs (an answer burst of 98 must not starve a purchase's reply): no log when it is full, the next tick tries again
    const auto has_room = [this] { return !failed_ && replies_.size() + delayed_.size() + 8 <= kMaxPendingReplies; };

    // the registry's levels changed: a delta of the GameState's node maps
    if (pb_.mode() == bootstrap::GameStateMode::Tier3 && nodes && wire && nodes->version() != node_maps_version_ && has_room()) {
        node_maps_version_ = nodes->version();
        auto maps = std::make_shared<protocol::repl::NodeStateMaps>(wire->maps(*nodes));
        ++node_map_updates_;
        spdlog::info("[nodes] {}: the node levels changed (registry version {}): sending the GameState's node maps ({})", name(), node_maps_version_, nodes->describe_levels());
        queue("GameState node maps", [maps](bootstrap::PlayerBootstrap& pb) { return pb.node_maps_update(*maps); }, conn, /*must_build=*/false);
    }
    if (cooldowns_pending_ && has_room()) {
        cooldowns_pending_ = false;
        ++siege_cooldowns_sent_;
        queue("ClientReceiveAllCurrentNodeSiegeCooldowns", [](bootstrap::PlayerBootstrap& pb) { return pb.siege_cooldowns(); }, conn, /*must_build=*/true);
    }
    const auto unknown = [this](int64_t node, const char* what) {
        if (node_unknown_logged_) return;
        node_unknown_logged_ = true;
        spdlog::warn("[nodes] {}: the client asked {} for node {}, which worldd does not know: not answered (further ones are not logged)", name(), what, node);
    };
    // retail's order: every siege summary first, then every node summary, each in the order the client asked
    while (!siege_wait_.empty() && has_room()) {
        const int64_t node = siege_wait_.front();
        siege_wait_.pop_front();
        siege_queued_.erase(node);
        const auto s = nodes && wire ? wire->siege(node, *nodes) : std::nullopt;
        if (!s) {
            // a record that is no node of a map (the dungeons, the jungle) gets no siege answer, as in retail; a guid worldd does not know is said once
            if (nodes && nodes->by_guid(node)) spdlog::debug("[nodes] {}: no siege summary for {}: it is no node of a map (retail does not answer it either)", name(), node);
            else unknown(node, "for its siege summary");
            continue;
        }
        ++siege_summaries_sent_;
        queue("ClientReceiveSiegeSummary", [s](bootstrap::PlayerBootstrap& pb) { return pb.siege_summary(*s); }, conn, /*must_build=*/true);
    }
    while (siege_wait_.empty() && !node_summary_wait_.empty() && has_room()) {
        const int64_t node = node_summary_wait_.front();
        node_summary_wait_.pop_front();
        node_summary_queued_.erase(node);
        const int64_t unix_now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto s = nodes && wire ? wire->summary(node, *nodes, unix_now) : std::nullopt;
        if (!s) {
            unknown(node, "for its summary");
            continue;
        }
        ++node_summaries_sent_;
        if (node_summaries_sent_ == 1 || node_summaries_sent_ == 49) {
            spdlog::info("[nodes] {}: node summary #{}: {} at level {} with {} plots", name(), node_summaries_sent_,
                         nodes->by_guid(node) ? nodes->by_guid(node)->display_name : std::to_string(node), s->level, s->plots.size());
        }
        queue("ClientReceiveNodeSummary", [s](bootstrap::PlayerBootstrap& pb) { return pb.node_summary(*s); }, conn, /*must_build=*/true);
    }
}

void PlayerSession::on_gm_node_rpc(const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string rpc_name = rpc.call.rpc->name;
    ++node_gm_rpcs_;
    char params[256];
    std::snprintf(params, sizeof params, "%zu parameter bits%s", rpc.call.param_bits, rpc.call.decoded ? "" : "; the parameters did not decode");
    world::NodeControl* control = world_ ? world_->node_control() : nullptr;
    const std::string by = rpc_name + " by " + name();
    const auto admitted = [&](const std::string& shown) { return gm_rpc_admitted(rpc_name.c_str(), shown, now) && rpc.call.decoded; };
    const auto unavailable = [&] { spdlog::warn("[gm] {}: {}: the node service is unavailable (worldd runs without a citynode connection)", name(), rpc_name); };
    if (rpc_name == "ServerSetNodeLevel") {
        const auto g = repl::parse_gm_set_node_level(rpc.call);
        const std::string shown = g ? "InNode=\"" + g->node.substr(0, 80) + "\" InLevel=" + std::to_string(g->level) + " InCulture=" + std::to_string(g->culture) +
                                          " bDisableVassalshipSystem=" + (g->disable_vassalship ? "1" : "0")
                                    : std::string(params);
        if (!admitted(shown) || !g) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->set_level(g->node, g->level, g->disable_vassalship, by);
    } else if (rpc_name == "ServerAddNodeExperience") {
        const auto g = repl::parse_gm_add_node_experience(rpc.call);
        const std::string shown = g ? "InNode=\"" + g->node.substr(0, 80) + "\" Experience=" + std::to_string(g->experience) + " InCulture=" + std::to_string(g->culture) +
                                          " bUseRawXP=" + (g->raw_xp ? "1" : "0")
                                    : std::string(params);
        if (!admitted(shown) || !g) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->add_experience(g->node, g->experience, by);
    } else if (rpc_name == "ServerGMGetNodeData") {
        const auto g = repl::parse_gm_get_node_data(rpc.call);
        const std::string shown = g ? "NodeName=\"" + g->substr(0, 80) + "\"" : std::string(params);
        if (!admitted(shown) || !g) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->node_data(*g, by);
    } else if (rpc_name == "ServerGMWhatTimeIsIt") {
        if (!admitted(params)) return;
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->clock(by);
    } else if (rpc_name == "ServerGMFastForwardRealTime") {
        int64_t seconds = 0;
        if (rpc.call.decoded) {
            if (const repl::Value* v = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "FastForwardDuration")) {
                if (const repl::Ticks* t = v->get_if<repl::Ticks>()) seconds = t->ticks / 10000000;   // an FTimespan is 100 ns ticks
            }
        }
        if (!admitted("FastForwardDuration=" + std::to_string(seconds) + " s")) return;
        if (seconds <= 0) {
            spdlog::warn("[gm] {}: ServerGMFastForwardRealTime of {} s: nothing to skip", name(), seconds);
            return;
        }
        if (!control) return unavailable();
        ++gm_rpcs_answered_;
        control->advance_clock(seconds, by);
    } else {
        // every other ServerGM* call (mayors, citizens, sieges, relics ...) has its milestone: logged, so a test of the shipping client's console shows what it sends
        gm_rpc_admitted(rpc_name.c_str(), params, now);
        spdlog::log(level_for(node_logged_, 20),
                    "[gm] {}: {} is not implemented in Nodes 1 (only ServerSetNodeLevel, ServerAddNodeExperience, ServerGMGetNodeData, ServerGMWhatTimeIsIt and "
                    "ServerGMFastForwardRealTime reach the node service): ignored",
                    name(), rpc_name);
    }
}

}}  // namespace aoc::worldd
