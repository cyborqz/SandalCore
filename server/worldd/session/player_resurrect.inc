// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_resurrect.inc: the Resurrect, a dead player brought back in place (Social, seeing other players: their death)
//  Included at the end of session/player_combat.cpp; its declarations are in session/members/resurrect.inc.
//
//  Another player's Resurrection is a cast of the Cleric's (ServerTriggerAbility at the dead ally: the ability's own hit names a ResurrectRecordBase and reaches
//  only the dead, session/ability_resurrect.h) and the client's confirmation of it (ServerResurrect(CharacterName), the generated schema's call).  The
//  Cleric's session checks that a cast of its own names that dead player, that he is no enemy and still in the cast's reach, and then hands the dead player's
//  session the record's values.  That session asks its own player (ClientPromptResurrect(InstigatorGuid), the Cleric's character GUID; both of the game's records
//  say to ask); his answer (ServerPromptedResurrectResponse(Response)) revives him in place with the record's shares of his maxima, or leaves him dead.  A prompt
//  waits a minute and belongs to the death it was made in: one nobody answered, or one left over from a life he respawned from, asks nothing of the next Cleric.
//
//  The revive is the same pawn: the client's own OnResurrect_Local follows CombatInfo.bIsDead 0, and the other players are shown the bit and the new bars by
//  the pawn they hold (session/player_shown.cpp: sync_shown_death and sync_shown_stats, in one update at that session's next tick).
//
//  Modelled: no recording of a resurrect exists; the three calls are the schema's and their parameters the capture-less reading of the data (the ratios, the
//  prompt).  The prompt waits a minute for the answer (no recording shows a timeout).
//  (This file is included inside namespace aoc::worldd: the headers it uses are included at the top of session/player_combat.cpp.)
// ============================================================================

namespace {

/// The name a ServerResurrect carries (nullopt when the call does not decode).
std::optional<std::string> resurrect_call_name(const repl::ClientRpc& rpc) {
    if (!rpc.call.decoded) return std::nullopt;
    const repl::Value* value = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "CharacterName");
    const std::string* text = value ? value->get_if<std::string>() : nullptr;
    return text ? *text : std::string();
}

/// True when `a` and `b` are the same name without regard to the case of A to Z (the chat directory looks a name up the same way; a character's name is
/// not case-sensitive in the game's calls).
bool resurrect_same_name(const std::string& a, const std::string& b) {
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return fold(x) == fold(y); });
}

}  // namespace

PlayerSession::ResurrectOffer PlayerSession::resurrect_offer_of(int64_t record, const bootstrap::PlayerSetup& from) {
    const abilities::ResurrectValues values = abilities::resurrect_values(record);
    ResurrectOffer offer;
    offer.record = record;
    offer.health_ratio = values.health_ratio;
    offer.mana_ratio = values.mana_ratio;
    offer.stamina_ratio = values.stamina_ratio;
    const auto& g = from.character_guid;
    offer.instigator_character = {g[0], g[1], g[2], g[3]};
    offer.instigator_name = from.name;
    return offer;
}

void PlayerSession::on_server_resurrect(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(quiet_budget_, "ServerResurrect", now)) return;
    ++resurrect_requests_;
    const auto refuse = [&](const std::string& why) {
        ++resurrect_refusals_;
        spdlog::log(level_for(resurrect_logged_), "[session] {}: a ServerResurrect is refused: {}", name(), why);
    };
    const std::optional<std::string> sent = resurrect_call_name(rpc);
    if (!sent) return refuse("the call does not read");
    if (sent->empty()) return refuse("no character is named");
    if (dead_) return refuse("the Cleric is dead");
    if (!npc_world_ || !npc_world_->combat()) return refuse("this world has no fights");
    ChatEntry* entry = ChatEntry::find(world_, *sent);
    if (!entry || !entry->self) return refuse(resurrect_same_name(*sent, name()) ? std::string("that is the Cleric himself") : *sent + " is not online");
    PlayerSession& target = *entry->self;
    if (&target == this) return refuse("that is the Cleric himself");
    if (!target.dead_) return refuse(target.name() + " is alive");
    // the cast: the trigger spent the ability's Mana, started its cooldown and checked the target's side, life state and reach, and its window is the cast's
    const uint64_t target_pawn = target.pb_.setup().guids.pawn.ObjectId;
    if (!pipeline_) return refuse("the Cleric's abilities are not running");
    int64_t record = pipeline_->resurrect_cast_for(target_pawn);
    if (record == 0) {
        // modelled: no recording shows the client's order, so a player whose client sent only the confirmation starts the cast here, with the trigger's own
        // checks (the ability known, the Mana, the cooldown, the side, the life state and the reach); the server's own instance id, as an emote's
        const abilities::Catalog* catalog = world_ ? world_->ability_catalog() : nullptr;
        int64_t ability = 0;
        for (const int64_t known : known_) {
            const abilities::AbilityDef* def = catalog ? catalog->ability(known) : nullptr;
            if (def && abilities::resurrect_hit_of(*catalog, *def)) {
                ability = known;
                break;
            }
        }
        if (ability == 0) return refuse("the Cleric knows no Resurrection");
        TriggerInfo info;
        info.by_server = true;
        info.target_count = 1;
        info.target_actors = {target_pawn};
        info.input_x = 1.0;
        if (pipeline_->trigger(ability, next_emote_instance(), ability_seconds(now), info) != Refusal::None) return refuse("the Resurrection cast is refused right now");
        take_ability_events();
        flush_abilities(conn);
        record = pipeline_->resurrect_cast_for(target_pawn);
        if (record == 0) return refuse("the Resurrection cast does not take him");
    }
    // the reach and the side again: he may have been moved since the cast
    combat::PlayerPort* port = npc_world_->combat()->find_player(target_pawn);
    if (!combat_side_ || !port) return refuse("the two are not in the world's fights");
    const combat::TargetInfo ti = npc_world_->combat()->inspect(target_pawn, *combat_side_);
    if (ti.relation == combat::Relation::Hostile) return refuse(target.name() + " is an enemy");
    if (ti.surface_distance_cm > npc_world_->combat()->config().player_reach_cm + npc_world_->combat()->data().activation_leeway_cm()) {
        return refuse(target.name() + " is out of the cast's reach");
    }
    if (target.resurrect_offer_live(now)) return;   // (the prompt he has not answered yet: a second ask changes nothing)
    ResurrectOffer offer = resurrect_offer_of(record, pb_.setup());
    offer.instigator = pb_.setup().guids.pawn.ObjectId;   // (the Cleric's pawn, as the hit's own request names it)
    ++resurrect_prompts_sent_;
    spdlog::log(level_for(resurrect_logged_), "[session] {}: asks {} to take the resurrection of {} ({} % Health, {} % Mana)", name(), target.name(), offer.instigator_name,
                static_cast<int>(offer.health_ratio * 100.0), static_cast<int>(offer.mana_ratio * 100.0));
    target.offer_resurrect(offer, now);
}

void PlayerSession::offer_resurrect(const ResurrectOffer& offer, Clock::time_point now) {
    if (!dead_) return;
    if (resurrect_offer_live(now)) return;   // (a prompt the player has not answered yet: the second ask changes nothing)
    if (offer.instigator == resurrect_declined_by_ && now < resurrect_declined_until_) return;   // (the cast he just turned down: its other call does not ask him again)
    resurrect_offer_ = offer;
    resurrect_offer_->life = deaths_;
    resurrect_offer_->expires = now + std::chrono::seconds(60);   // (modelled: no recording shows the prompt's timeout)
    ++resurrect_prompts_;
    Connection* conn = chat_entry_.connection;
    if (!conn) return;
    const std::array<uint32_t, 4> instigator = offer.instigator_character;
    queue("ClientPromptResurrect", [instigator](bootstrap::PlayerBootstrap& pb) { return pb.resurrect_prompt(instigator); }, *conn);
    spdlog::log(level_for(resurrect_logged_), "[session] {}: is asked whether {} raises it", name(), offer.instigator_name);
}

void PlayerSession::revive_in_place(const ResurrectOffer& offer, Clock::time_point now) {
    resurrect_offer_.reset();
    if (!dead_) return;
    dead_ = false;
    ++resurrections_;
    if (pipeline_) pipeline_->set_dead(false);
    pb_.set_dead(false);
    // The death's ClientRestart restarted the client's pawn and with it the TimeStamp of its moves (near 0 again): the checker's last good TimeStamp is from the old clock, and every
    // move below it would be dropped silently until the new clock passed it.  The place stays (the revive is where he fell).
    checker_.restart_clock();
    const stats::StatValue* hp = stats_ ? stats_->find(content::kGuidMaxHealth) : nullptr;
    const stats::StatValue* mp = stats_ ? stats_->find(content::kGuidMaxMana) : nullptr;
    if (hp) set_current_value(content::kGuidHealth, static_cast<float>(hp->value * offer.health_ratio));
    if (mp) set_current_value(content::kGuidMana, static_cast<float>(mp->value * offer.mana_ratio));
    abilities_dirty_ = true;   // (CombatInfo.bIsDead 0 goes out even when a bar's value did not move)
    // the Stamina the record gives back (A7's own bar; the record's share of its maximum)
    if (abilities_ && offer.stamina_ratio > 0.0) {
        bootstrap::AbilityChange c;
        const double want = abilities_->max_stamina() * offer.stamina_ratio;
        if (want > abilities_->stamina()) abilities_->add_stamina(c, want - abilities_->stamina());
        if (!c.empty() && pb_.apply_ability_change(c)) abilities_dirty_ = true;
    }
    spdlog::log(level_for(resurrect_logged_), "[session] {}: comes back in place, raised by {} ({} % Health, {} % Mana)", name(), offer.instigator_name,
                static_cast<int>(offer.health_ratio * 100.0), static_cast<int>(offer.mana_ratio * 100.0));
}

void PlayerSession::on_prompted_resurrect_response(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!within_budget(quiet_budget_, "ServerPromptedResurrectResponse", now)) return;
    const repl::Value* value = rpc.call.decoded ? repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Response") : nullptr;
    const bool* answer = value ? value->get_if<bool>() : nullptr;
    if (!answer) return;
    if (!resurrect_offer_) return;   // (an answer to no prompt: a repeat, or a forged one)
    if (!resurrect_offer_live(now)) {
        // the prompt's minute is over, or the death it asked about is (he respawned, and may have died again): nothing to answer
        const bool ran_out = dead_ && resurrect_offer_->life == deaths_;
        resurrect_offer_.reset();
        if (ran_out) ++resurrect_declines_;
        spdlog::log(level_for(resurrect_logged_), "[session] {}: the resurrection prompt {}: his answer raises nobody", name(), ran_out ? "ran out before the answer" : "was for a death he is not in");
        return;
    }
    const ResurrectOffer offer = *resurrect_offer_;
    if (!*answer) {
        resurrect_offer_.reset();
        ++resurrect_declines_;
        resurrect_declined_by_ = offer.instigator;
        resurrect_declined_until_ = now + std::chrono::seconds(5);   // (the cast's own request window: the same cast's other call does not ask again)
        spdlog::log(level_for(resurrect_logged_), "[session] {}: turns the resurrection of {} down and stays dead", name(), offer.instigator_name);
        return;
    }
    revive_in_place(offer, now);
}

void PlayerSession::take_resurrect_request(Clock::time_point now) {
    if (!pipeline_) return;
    const AbilityPipeline::ResurrectRequest r = pipeline_->take_resurrect();
    if (r.target == 0 || r.record == 0) return;
    combat::PlayerPort* port = npc_world_ && npc_world_->combat() ? npc_world_->combat()->find_player(r.target) : nullptr;
    if (!port) return;
    ChatEntry* entry = ChatEntry::joined(world_, port->character_guid());
    if (!entry || !entry->self || entry->self == this) return;
    PlayerSession& target = *entry->self;
    if (!target.dead_) return;
    if (target.resurrect_offer_live(now)) return;
    ResurrectOffer offer = resurrect_offer_of(r.record, pb_.setup());
    offer.instigator = pb_.setup().guids.pawn.ObjectId;
    ++resurrect_prompts_sent_;
    target.offer_resurrect(offer, now);
}

/// The client calls of the Resurrect: the Cleric's confirmation of his cast, and the dead player's answer (session/rpc_table.h).
void PlayerSession::register_resurrect_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerResurrect", [](PlayerSession& s, const RpcContext& c) { s.on_server_resurrect(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerPromptedResurrectResponse", [](PlayerSession& s, const RpcContext& c) { s.on_prompted_resurrect_response(c.conn, c.rpc, c.now); });
}
