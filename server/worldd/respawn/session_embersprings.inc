// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  respawn/session_embersprings.inc  (Embersprings)
//
//  PlayerSession's Embersprings members, included at the end of session/player_session.cpp (inside namespace aoc::worldd::session): the character's places (loaded at the
//  join, saved with the progress), the discovery by coming close, the use of a shrine (the prompt, the home), and the death window's choices and the respawn.  The rules are
//  respawn/embersprings.h's.
// ============================================================================

namespace {

/// A Choices that offers one fixed definition: an area with no Embersprings (an older content.db, a test's circle) keeps the single choice it always had.
constexpr int64_t kFallbackRecord = 6064632337807835941ll;

}  // namespace

const respawn::ShrineBook* PlayerSession::shrine_book() const {
    if (!npc_world_) return nullptr;
    const respawn::ShrineBook* book = npc_world_->shrines();
    return book && !book->empty() ? book : nullptr;
}

void PlayerSession::init_attunements(const svc::Character& character) {
    respawn::Attunements a;
    for (const int32_t n : character.attunements) a.discover(n);   // (a number out of range, or one twice, is left out)
    a.home = character.home_attunement >= 1 && character.home_attunement <= respawn::kMaxPlaceNumber ? character.home_attunement : 0;
    a.home_last_used_ms = std::max<int64_t>(0, character.home_last_used_ms);
    attunements_ = std::move(a);
    // a character with nothing on its list joins with the pawn's default state (the first discovery is an update)
    if (attunements_.discovered.empty() && attunements_.home == 0 && attunements_.home_last_used_ms <= 0) return;
    // the first state of the pawn carries them: the last use on the client's synchronized clock (the saved moment is wall time)
    const double join_clock = bootstrap::effect_clock(pb_.setup().world_time);
    const double used = attunements_.home_last_used_ms > 0 ? join_clock - static_cast<double>(wall_ms() - attunements_.home_last_used_ms) / 1000.0 : 0.0;
    if (!pb_.set_attunements(respawn::blocks_of(attunements_), attunements_.home, used)) {
        spdlog::warn("[session] {}: the pawn does not take the character's {} discovered place(s): the client is not told of them", pb_.setup().name, attunements_.discovered.size());
        return;
    }
    if (!attunements_.discovered.empty() || attunements_.home != 0) {
        spdlog::info("[session] {}: {} discovered Emberspring(s) on the character's list, home {}, sent with the join", pb_.setup().name, attunements_.discovered.size(),
                     attunements_.home != 0 ? std::to_string(attunements_.home) : std::string("none"));
    }
}

void PlayerSession::send_attunements(Connection& conn, Clock::time_point now) {
    const double used = attunements_.home_last_used_ms > 0 ? sync_now(now) - static_cast<double>(wall_ms() - attunements_.home_last_used_ms) / 1000.0 : 0.0;
    if (!pb_.set_attunements(respawn::blocks_of(attunements_), attunements_.home, used)) {
        spdlog::warn("[session] {}: the pawn does not take the character's places: the client is not told of the change", name());
        return;
    }
    progress_changed_ = true;   // (saved with the progress: at once, a discovery or a home is worth keeping when the server stops)
    hand_over_progress("the Embersprings changed");
    queue("pawn state (attunements)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("attunements"); }, conn, /*must_build=*/true);
}

void PlayerSession::run_embersprings(Connection& conn, Clock::time_point now) {
    if (!phase_in_done_ || dead_ || finished_ || failed_) return;
    const respawn::ShrineBook* book = shrine_book();
    if (!book) return;
    const GoodPosition g = checker_.good();
    for (const respawn::Shrine* s : book->reaching(g.x, g.y, g.z)) {
        if (attunements_.has(s->number)) continue;
        if (!room_for(6)) return;   // (the replies wait: the next tick finds the shrine again)
        if (!attunements_.discover(s->number)) continue;
        ++discoveries_;
        spdlog::log(level_for(shrine_logged_, 40), "[embersprings] {}: discovers the Emberspring {} (number {}, {:.0f} m from it; {} of the character's now)", name(), s->name, s->number,
                    std::hypot(s->x - g.x, s->y - g.y) / 100.0, attunements_.discovered.size());
        send_attunements(conn, now);
        const std::string line = respawn::discovery_line(s->name);
        queue("ClientReceiveChatMessages", [line](bootstrap::PlayerBootstrap& pb) { return pb.chat_line(line, respawn::kChatCategory); }, conn, /*must_build=*/false);
    }
}

void PlayerSession::on_shrine_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    const auto ignore = [&](const std::string& why) {
        ++interact_ignored_;
        spdlog::log(level_for(shrine_logged_, 40), "[embersprings] {}: a click on the Emberspring {} is ignored: {}", name(), a.shrine.name, why);
    };
    if (view_->state_of(a.id) != world::ClientState::Awake) return ignore("the client does not have that Emberspring open");
    const GoodPosition at = checker_.good();
    const double dx = a.x - at.x, dy = a.y - at.y, dz = a.z - at.z;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance >= respawn::kUseRangeCm) return ignore("it is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away (it answers within " + std::to_string(static_cast<int>(respawn::kUseRangeCm / 100.0)) + " m)");
    if (combat_flag_) return ignore("the player is in combat (the interaction is not allowed in combat)");
    if (a.shrine.place == 0) return ignore("it stands for no place");
    if (!within_budget(vendor_budget_, "an Emberspring", now) || !room_for(2)) return;
    pending_attune_ = a.shrine.place;
    ++shrine_uses_;
    spdlog::log(level_for(shrine_logged_, 40), "[embersprings] {}: uses the Emberspring {} ({:.0f} m away): the prompt to make it the home", name(), a.shrine.name, distance / 100.0);
    const int64_t place = a.shrine.place;
    queue("SendClientPromptForHomeAttunement", [place](bootstrap::PlayerBootstrap& pb) { return pb.attune_prompt(place); }, conn, /*must_build=*/true);
}

void PlayerSession::on_home_attunement(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_) return;
    const auto it = rpc.call.params.values.find(1);
    const bool* yes = it == rpc.call.params.values.end() ? nullptr : it->second.get_if<bool>();
    const int64_t place = pending_attune_;
    pending_attune_ = 0;   // (the pending place is used up in every case)
    if (!yes || !*yes || place == 0) return;
    if (!within_budget(vendor_budget_, "RequestServerSetHomeAttunement", now) || !room_for(4)) return;
    const respawn::ShrineBook* book = shrine_book();
    const respawn::Shrine* s = book ? book->by_place(place) : nullptr;
    if (!s || s->number <= 0) {
        spdlog::log(level_for(shrine_logged_, 40), "[embersprings] {}: said yes to a place this world does not hold: nothing changes", name());
        return;
    }
    if (!attunements_.has(s->number)) {
        // a place the character has not discovered yet is discovered by this, and that is all (the home is made by the next use)
        attunements_.discover(s->number);
        ++discoveries_;
        spdlog::log(level_for(shrine_logged_, 40), "[embersprings] {}: discovers the Emberspring {} by attuning to it", name(), s->name);
        send_attunements(conn, now);
        const std::string line = respawn::discovery_line(s->name);
        queue("ClientReceiveChatMessages", [line](bootstrap::PlayerBootstrap& pb) { return pb.chat_line(line, respawn::kChatCategory); }, conn, /*must_build=*/false);
        return;
    }
    const bool change = attunements_.home != s->number;
    const FIntrepidNetworkGUID guid = [&] {
        const world::WorldActor* a = registry_ ? registry_->find(s->actor) : nullptr;
        return a ? a->actor.guid : FIntrepidNetworkGUID{};
    }();
    if (change) {
        attunements_.home = s->number;
        ++homes_set_;
        spdlog::log(level_for(shrine_logged_, 40), "[embersprings] {}: the Emberspring {} is the home now", name(), s->name);
        send_attunements(conn, now);
    }
    const int64_t place_record = s->place;
    queue("ClientNotifyHomeAttune and SendClientHomeAttuneResult", [guid, place_record, change](bootstrap::PlayerBootstrap& pb) { return pb.home_attuned(guid, place_record, change); }, conn, /*must_build=*/true);
}

void PlayerSession::send_respawn_choices(Connection& conn, bool previous_invalid, bool home_on_cooldown) {
    const respawn::ShrineBook* book = shrine_book();
    if (!book) {   // no Emberspring in this area: the one choice it always had
        queue("SendClientRespawnChoices", [previous_invalid](bootstrap::PlayerBootstrap& pb) {
            const int64_t list[1] = {kFallbackRecord};
            return pb.respawn_choices(std::span<const int64_t>(list, 1), kFallbackRecord, previous_invalid);
        }, conn, /*must_build=*/true);
        return;
    }
    const respawn::Choices c = respawn::choices_for(*book, attunements_, death_x_, death_y_);
    const std::vector<int64_t> closest = c.closest;
    const int64_t home = c.home;
    queue("SendClientRespawnChoices", [closest, home, previous_invalid, home_on_cooldown](bootstrap::PlayerBootstrap& pb) {
        return pb.respawn_choices(std::span<const int64_t>(closest.data(), closest.size()), home, previous_invalid, home_on_cooldown);
    }, conn, /*must_build=*/true);
}

void PlayerSession::on_respawn_choices_request(Connection& conn, const repl::ClientRpc&, Clock::time_point now) {
    if (!dead_) return;
    if (!within_budget(loot_budget_, "RequestServerRespawnChoices", now) || !room_for(1)) return;
    const respawn::ShrineBook* book = shrine_book();
    if (book) {
        const respawn::Choices c = respawn::choices_for(*book, attunements_, death_x_, death_y_);
        const respawn::Shrine* near = c.closest.empty() ? nullptr : book->by_respawn(c.closest[0]);
        const respawn::Shrine* home = book->home_of(attunements_);
        spdlog::log(level_for(death_logged_), "[session] {}: asks where it may come back: nearest discovered Emberspring {}, home {}{}", name(), near ? near->name : std::string("none"),
                    home ? home->name : std::string("none"), respawn::home_on_cooldown(attunements_, wall_ms()) ? " (waiting for its cooldown)" : "");
    } else {
        spdlog::log(level_for(death_logged_), "[session] {}: asks where it may come back: the nearest respawn point", name());
    }
    send_respawn_choices(conn, false, false);
}

void PlayerSession::on_respawn_choice(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!dead_) return;
    if (!within_budget(loot_budget_, "SendServerRespawnChoice", now) || !room_for(4)) return;
    const auto it = rpc.call.params.values.find(1);
    const int64_t* chosen = it == rpc.call.params.values.end() ? nullptr : it->second.get_if<int64_t>();
    const respawn::ShrineBook* book = shrine_book();
    const respawn::Shrine* shrine = nullptr;
    PlayerWorld::RespawnSpot spot;
    if (!book) {
        // no Emberspring in this area: the one choice it always had, the nearest respawn point of the data or the login spot
        if (!chosen || *chosen != kFallbackRecord) return send_respawn_choices(conn, true, false);
        spot = npc_world_ ? npc_world_->respawn_spot(death_x_, death_y_) : PlayerWorld::RespawnSpot{death_x_, death_y_, death_z_, 0, false, "where it fell"};
    } else {
        const respawn::Choices c = respawn::choices_for(*book, attunements_, death_x_, death_y_);
        const int64_t now_ms = wall_ms();
        switch (respawn::judge(c, chosen ? *chosen : 0, attunements_, now_ms)) {
            case respawn::Pick::Invalid:
                spdlog::log(level_for(death_logged_), "[session] {}: chose a respawn point the window did not offer: the choices again, marked", name());
                return send_respawn_choices(conn, true, false);
            case respawn::Pick::HomeOnCooldown:
                spdlog::log(level_for(death_logged_), "[session] {}: chose its home, which waits {:.0f} s more: refused, the choices again", name(), respawn::home_cooldown_left(attunements_, now_ms));
                return send_respawn_choices(conn, false, true);
            case respawn::Pick::Accepted:
                break;
        }
        shrine = book->by_respawn(*chosen);
        if (!shrine) return send_respawn_choices(conn, true, false);
        const uint64_t seed = respawn::mix64(std::hash<std::string>{}(character_id_) ^ (static_cast<uint64_t>(respawns_ + 1) * 0x9E3779B97F4A7C15ull));
        const respawn::Spot at = respawn::spot_at(*shrine, seed);
        spot = PlayerWorld::RespawnSpot{at.x, at.y, npc_world_ ? npc_world_->spawn_height(at.x, at.y, at.z) : at.z, at.yaw, true, "the Emberspring " + shrine->name};
        // the home respawn is used up when it is the place the character comes back at (the chosen definition or the nearest one that is the home too)
        if (shrine->number > 0 && shrine->number == attunements_.home) {
            attunements_.home_last_used_ms = now_ms;
            ++home_uses_;
            send_attunements(conn, now);
        }
    }
    finish_respawn(conn, spot, now);
}

void PlayerSession::on_plain_respawn(Connection& conn, Clock::time_point now) {
    if (!dead_) return;
    if (!within_budget(loot_budget_, "Respawn", now) || !room_for(4)) return;
    const respawn::ShrineBook* book = shrine_book();
    // The window's button sends the plain respawn whenever no place is picked, whether the window lists places or not, and retail's Respawn restarts the player at the game mode's
    // default start whatever the choices were: so it is always the login spot, where a new character begins, and never an Emberspring (modelled: no client data names retail's
    // default start).  An area without Embersprings keeps the older rule: the nearest respawn point of the data, else the login spot.
    if (book) {
        const respawn::Choices c = respawn::choices_for(*book, attunements_, death_x_, death_y_);
        if (!c.closest.empty() || c.home != 0)
            spdlog::log(level_for(death_logged_), "[session] {}: asks for the plain respawn while the window lists places: the login spot, as retail's default start", name());
    }
    const PlayerWorld::RespawnSpot spot = npc_world_ ? npc_world_->respawn_spot(death_x_, death_y_, /*nearest_point=*/book == nullptr) : PlayerWorld::RespawnSpot{death_x_, death_y_, death_z_, 0, false, "where it fell"};
    finish_respawn(conn, spot, now);
}

void PlayerSession::finish_respawn(Connection& conn, const PlayerWorld::RespawnSpot& spot, Clock::time_point now) {
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
