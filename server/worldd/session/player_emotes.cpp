// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_emotes.cpp
//
//  The emote commands: /sit, /wave and the rest of the game's emote list (step 5 of the social track).
//
//  The client sends ServerPlayEmote(EmoteName) with the typed word and no slash.  The word is looked up in the game's list (content/emote_commands.h) without regard to case:
//    - a word that is not on the list is refused, the refusal is logged and the client is sent nothing;
//    - /sit is the Rest (General_Rest, a toggle).  The first one sits the character down through the ability runtime (session/abilities.h): the ability item with the Rest montage and, two
//      seconds on, the Resting effect.  A second /sit, while the Rest runs, ends it and plays the montage's end section (SitEnd).  In combat, on a mount or dead the character does not sit: the
//      refusal is logged.  A Rest that runs can always be ended;
//    - every other word plays its ability through the generic ability runtime (session/ability_pipeline.h), which applies the record's cooldown and cost (Emote_Wave: a one-second
//      cooldown and one charge, no Mana or Stamina).  An ability the content tables do not hold is logged and nothing is played.
//  A refusal sends the client nothing: it starts no instance of its own for an emote, so there is none for a ClientEndAbility to end (TriggerInfo::by_server).
//  Calls over the budget (kEmoteBurst, then one every kEmoteInterval) are dropped and counted.
//
//  The other players see the emotes.  The pawn they are shown (session/player_presence.cpp) has an AbilityComponent (bootstrap/observer_actor.cpp), and the emote ability items of this
//  player's own pawn (the Rest's, Emote_Wave's ...) are written to its ReplicatedActiveAbilities: note_emote_items keeps the list when the ability changes are applied, run_emotes brings the
//  shown pawn in line with it once a tick (in tick_logic, before the view passes read the registry).  Every view that holds the pawn, within the 150 m of the view, sends its client the
//  update as it sends any actor's; the montage's export rides with it.  Only emotes are shown: the Sprint, the Dodge and the combat abilities are not.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/constants.h"
#include "content/emote_commands.h"
#include "protocol/repl/rpc_fields.h"

#include <spdlog/spdlog.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// The word as the log shows it: printable ASCII only (anything else is a question mark) and at most 40 characters, so that a hostile client's word is not copied whole into the log.
std::string shown(const std::string& word) {
    constexpr size_t kMaxShown = 40;
    std::string out;
    for (size_t i = 0; i < word.size() && i < kMaxShown; ++i) {
        const unsigned char c = static_cast<unsigned char>(word[i]);
        out += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    if (word.size() > kMaxShown) out += "...";
    return out;
}

}  // namespace

void PlayerSession::emote_refusal(const std::string& word, const char* why) {
    ++emotes_refused_;
    spdlog::log(level_for(emote_refusals_logged_), "[emote] {}: \"{}\" is refused: {}", name(), shown(word), why);
}

void PlayerSession::on_play_emote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(emote_budget_, "ServerPlayEmote", now)) {
        ++emotes_dropped_;
        return;
    }
    if (!rpc.call.decoded) {
        ++emotes_refused_;
        if (++emote_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[emote] {}: a ServerPlayEmote does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    const repl::Value* value = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "EmoteName");
    const std::string* text = value ? value->get_if<std::string>() : nullptr;
    const std::string word = text ? *text : std::string();
    const content::EmoteCommand* emote = content::find_emote_command(word);
    if (!emote) {
        ++emotes_refused_;
        spdlog::log(level_for(emote_refusals_logged_), "[emote] {}: \"{}\" is not an emote: nothing is played", name(), shown(word));
        return;
    }
    if (dead_) return emote_refusal(word, "the character is dead");
    if (emote->ability == content::kGuidRest) return emote_sit(conn, word, now);
    emote_play(conn, emote->ability, std::string(emote->name), word, now);
}

void PlayerSession::emote_sit(Connection& conn, const std::string& word, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime) return emote_refusal(word, "abilities are not running for this session");
    cancel_gather(&conn, "a sit was asked for");   // (the same channel cancel an ordinary ability trigger does)
    const double sec = ability_seconds(now);
    if (runtime->resting()) {   // a second /sit: the character stands up
        runtime->end_rest(sec);
        ++emotes_played_;
        spdlog::log(level_for(emote_logged_), "[emote] {}: \"{}\" ends the Rest", name(), shown(word));
        take_ability_events();
        flush_abilities(conn);
        return;
    }
    TriggerInfo ask;
    ask.by_server = true;
    const Refusal ran = runtime->trigger(content::kGuidRest, next_emote_instance(), sec, ask);
    take_ability_events();
    if (ran != Refusal::None) {
        emote_refusal(word, refusal_name(ran));
    } else {
        ++emotes_played_;
        spdlog::log(level_for(emote_logged_), "[emote] {}: \"{}\" sits the character down", name(), shown(word));
    }
    flush_abilities(conn);
}

void PlayerSession::emote_play(Connection& conn, int64_t ability, const std::string& ability_name, const std::string& word, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return emote_refusal(word, "the generic ability runtime is not running for this session");
    if (!pipeline_->handles(ability)) {
        ++emotes_refused_;
        spdlog::log(level_for(emote_refusals_logged_), "[emote] {}: \"{}\" is the game's emote {}, but the content tables hold no ability for it: nothing is played", name(), shown(word),
                    ability_name);
        return;
    }
    const abilities::AbilityDef* def = world_ && world_->ability_catalog() ? world_->ability_catalog()->ability(ability) : nullptr;
    if (mounted() && def && !def->row.usable_while_mounted) return emote_refusal(word, "the character is on a mount");
    TriggerInfo ask;
    ask.by_server = true;
    const Refusal ran = pipeline_->trigger(ability, next_emote_instance(), ability_seconds(now), ask);
    if (ran == Refusal::None) {
        ++emotes_played_;
        spdlog::log(level_for(emote_logged_), "[emote] {}: \"{}\" plays {}", name(), shown(word), ability_name);
    } else {
        ++emotes_refused_;   // (the pipeline's refusal event says why: take_ability_events logs it)
    }
    take_ability_events();
    flush_abilities(conn);
}

// ---- the emotes the other players see -----------------------------------------------------------------------------------------------------------------------------

namespace {

/// Whether two items are shown to the other players alike: everything of the item the other players are sent (its Targets and its instigator are theirs to resolve, not the owner's).
bool shown_alike(const bootstrap::ActiveAbilityItem& a, const bootstrap::ActiveAbilityItem& b) {
    return a.ability_guid == b.ability_guid && a.server_instance_id == b.server_instance_id && a.montage_path == b.montage_path && a.montage_guid == b.montage_guid &&
           a.montage_group == b.montage_group && a.play_rate == b.play_rate && a.montage_position == b.montage_position && a.may_jump_to_position == b.may_jump_to_position &&
           a.branch_predicted == b.branch_predicted && a.animation_counter == b.animation_counter && a.mesh_yaw == b.mesh_yaw;
}

/// The montage of an item with its export, when the server knows its id (the same lookup the owner's item is made with).
std::optional<bootstrap::MontageRef> montage_of(const bootstrap::ActiveAbilityItem& a) {
    return a.montage_guid != 0 ? bootstrap::montage_for(a.montage_path, a.montage_guid) : bootstrap::montage_for(a.montage_path);
}

/// The AbilityComponent of an actor of the registry, or null.
repl::Subobject* ability_component_of(world::WorldActor& a) {
    for (repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UAoCAbilityComponent") return &s;
    }
    return nullptr;
}

/// An element of ReplicatedActiveAbilities as another player's client is shown it: the 17 members of the owner's item (bootstrap.cpp's ability_state), but the caster's collision cylinder is
/// no target (the other players have no such object: the cylinder is the owner's) and the instigator is the pawn they are shown.
repl::ElementState shown_item_state(const bootstrap::ActiveAbilityItem& a, const protocol::emit::FIntrepidNetworkGUID& pawn, const protocol::emit::FIntrepidNetworkGUID& montage) {
    repl::ElementState es;
    auto& v = es.values;
    v[1] = repl::Value::i64(a.ability_guid);
    v[2] = repl::Value::i64(a.server_instance_id);
    v[3] = repl::Value(repl::ArrayValue{});   // Targets: none
    v[4] = repl::Value(repl::ArrayValue{});   // TargetTransforms: none
    v[5] = repl::Value::boolean(false);       // bPvpForceFlag
    v[6] = repl::Value::i64(0);               // ReplicatedProgressInfo.NumDisruptiveHits
    v[7] = repl::Value::f32(0);               // .TotalDisruptionSeconds
    v[8] = repl::Value::f32(0);               // .CastDuration
    v[9] = repl::Value::boolean(a.branch_predicted);
    v[10] = repl::Value(repl::NameValue::plain(a.montage_group));
    v[11] = repl::Value(montage);
    v[12] = repl::Value::f32(a.play_rate);
    v[13] = repl::Value::f32(a.montage_position);
    v[14] = repl::Value::boolean(a.may_jump_to_position);
    v[15] = repl::Value::i64(a.animation_counter);
    v[16] = repl::Value::u64(a.mesh_yaw);
    v[17] = repl::Value(pawn);
    return es;
}

}  // namespace

void PlayerSession::note_emote_items(const bootstrap::AbilityChange& change) {
    bool touched = false;
    for (const int64_t instance : change.abilities_removed) touched = emote_items_.erase(instance) > 0 || touched;
    for (const bootstrap::ActiveAbilityItem& a : change.abilities_added) {
        if (!content::is_emote_ability(a.ability_guid)) continue;
        emote_items_[a.server_instance_id] = a;
        touched = true;
    }
    for (const bootstrap::ActiveAbilityItem& a : change.abilities_changed) {
        const auto it = emote_items_.find(a.server_instance_id);
        if (it == emote_items_.end()) continue;
        it->second = a;
        touched = true;
    }
    if (touched) emote_dirty_ = true;
}

void PlayerSession::run_emotes(Connection& conn, Clock::time_point now) {
    (void)conn;
    (void)now;
    if (!emote_dirty_ || !registry_ || world_pawn_ == 0) return;   // (before the pawn the others are shown is in the world, the items wait here)
    emote_dirty_ = false;
    std::vector<int64_t> gone, changed, fresh;   // the instances to take off the shown pawn, to change on it, and to put on it
    for (const auto& [instance, shown] : emote_seen_) {
        const auto held = emote_items_.find(instance);
        if (held == emote_items_.end()) gone.push_back(instance);
        else if (!shown_alike(shown.item, held->second)) changed.push_back(instance);
    }
    for (const auto& [instance, item] : emote_items_) {
        if (!emote_seen_.count(instance)) fresh.push_back(instance);
    }
    if (gone.empty() && changed.empty() && fresh.empty()) return;
    bool failed = false;
    const bool done = registry_->modify(world_pawn_, world::Change::State, [&](world::WorldActor& actor) {
        repl::Subobject* comp = ability_component_of(actor);
        repl::FastArrayState* array = comp ? comp->obj.arrays.get(*comp->obj.schema, "ReplicatedActiveAbilities") : nullptr;
        if (!array) {
            failed = true;
            return;
        }
        const protocol::emit::FIntrepidNetworkGUID pawn = actor.actor.guid;
        const auto guid_of = [](const bootstrap::ActiveAbilityItem& item) {
            const auto montage = montage_of(item);
            return montage ? montage->guid : protocol::emit::FIntrepidNetworkGUID{};
        };
        for (const int64_t instance : gone) {
            array->remove(emote_seen_.at(instance).element);
            array->mark_array_dirty();   // (a removal moves the array's key by two, as the owner's pawn does)
            emote_seen_.erase(instance);
        }
        for (const int64_t instance : changed) {
            ShownEmote& shown = emote_seen_.at(instance);
            const bootstrap::ActiveAbilityItem& item = emote_items_.at(instance);
            if (array->change(shown.element, shown_item_state(item, pawn, guid_of(item)))) shown.item = item;
            else failed = true;
        }
        for (const int64_t instance : fresh) {
            const bootstrap::ActiveAbilityItem& item = emote_items_.at(instance);
            const auto element = array->add(shown_item_state(item, pawn, guid_of(item)));
            if (element) emote_seen_[instance] = ShownEmote{*element, item};
            else failed = true;
        }
        // the montages' exports ride with the AbilityComponent's block until each connection has acknowledged them: one chain for each montage the shown items name
        actor.ability_exports.clear();
        std::set<uint64_t> named;
        for (const auto& [instance, shown] : emote_seen_) {
            const auto montage = montage_of(shown.item);
            if (montage && named.insert(montage->guid.ObjectId).second) actor.ability_exports.push_back(montage->chain);
        }
    });
    if (!done || failed) {
        if (++emote_shown_failures_ <= kLoggedPerKind) {
            spdlog::warn("[emote] {}: the pawn the other players are shown does not take the emote items ({}): they do not see this player's emotes", name(),
                         done ? "an item does not fit its array" : "the pawn is not in the world");
        }
    }
}

/// The client call of the emote feature: ServerPlayEmote, a call of the PlayerController (session/rpc_table.h).
void PlayerSession::register_emotes_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerPlayEmote", [](PlayerSession& s, const RpcContext& c) { s.on_play_emote(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
