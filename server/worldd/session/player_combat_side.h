// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_combat_side.h
//
//  The player's side of the area's fights: what the world's fights see of a joined player (a Fighter and a PlayerPort).  Defined here because both
//  the combat file and the ability file of the player session use it.
// ============================================================================
#pragma once

#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "session/stealth_state.h"
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

namespace aoc { namespace worldd {

using protocol::emit::FIntrepidNetworkGUID;

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
    void receive_player_hit(const combat::PlayerIncomingHit& hit, std::vector<double>& effective) override { s_.receive_player_hit(hit, effective); }
    void receive_player_effects(const std::vector<effects::Applied>& effects, uint64_t instigator, double now) override { s_.receive_player_effects(effects, instigator, now); }
    void receive_player_shield(double amount, double seconds, double now) override { s_.receive_player_shield(amount, seconds, now); }
    void remove_player_effects(const abilities::HitDef& hit, double now) override { s_.remove_player_effects(hit, now); }
    bool in_party_with(uint64_t actor) const override { return s_.in_party_with(actor); }
    void set_in_combat(bool in_combat, double) override { wants_in_combat = in_combat; }
    std::array<uint32_t, 4> character_guid() const override { return s_.pb_.setup().character_guid; }
    FIntrepidNetworkGUID pawn_guid() const override { return s_.pb_.setup().guids.pawn; }
    FIntrepidNetworkGUID cylinder_guid() const override { return s_.pb_.setup().guids.collision_cylinder; }
    void receive_kill(const combat::KillReward& kill, double) override {
        if (kills.size() < 64) kills.push_back(kill);
    }
    void confirm_effect_hit(const combat::EffectHitNote& note, double) override {
        if (effect_notes.size() < 256) effect_notes.push_back(note);
    }
    void hit_dealt(const combat::DealtHit& hit, double) override {
        if (dealt.size() < 64) dealt.push_back(hit);
    }
    void hit_from_pet(int64_t hit_guid, int64_t ability, double) override {
        if (pet_hits.size() < 16) pet_hits.emplace_back(hit_guid, ability);
    }
    void effect_refused(const combat::EffectRefusal& refusal, double) override {
        if (effect_refusals.size() < 64) effect_refusals.push_back(refusal);
    }
    bool holds_effect(int64_t effect_guid) const override { return s_.pipeline_ && s_.pipeline_->stacks_of(effect_guid) > 0; }
    combat::StealthState stealth() const override { return stealth_state_of(s_.pipeline_ ? &*s_.pipeline_ : nullptr, s_.stats_ ? &*s_.stats_ : nullptr); }
    // abilities between players (combat/combat_players.cpp): the party is the key two members share; another player's Resurrect is asked at the session's tick
    uint64_t group_key() const override { return reinterpret_cast<uintptr_t>(s_.party_group_); }
    void prompt_resurrect(const std::array<uint32_t, 4>& by, double) override { resurrect_by = by; }
    // PvP (session/player_pvp.cpp): the flag changes at once (the rules of the next hit read it), the client hears of it at the session's tick
    bool pvp_side(combat::pvp::Side& out, bool& force) const override {
        s_.pvp_side_of(out, force);
        return true;
    }
    void pvp_flagged(uint64_t target, uint8_t intention, double now) override {
        if (s_.pvp_mark_flagged(now) && pvp_notices.size() < 16) pvp_notices.emplace_back(target, intention);
    }
    void set_in_pvp_combat(bool on, double) override { wants_pvp_combat = on; }
    void pvp_kill(const combat::PvpKill& kill, double) override {
        if (pvp_kills.size() < 16) pvp_kills.push_back(kill);
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
    double facing_degrees() const override { return s_.checker_.good().yaw_degrees; }   // the control rotation's yaw, as the client's last good move gave it
    bool alive() const override {
        const stats::StatValue* h = s_.stats_ ? s_.stats_->find(content::kGuidHealth) : nullptr;
        return h && h->value > 0;
    }
    const std::vector<int64_t>& alignments() const override { return aligns_; }
    stats::CharacterStats& stats() override { return *s_.stats_; }
    std::vector<stats::WornItem> worn() const override { return bootstrap::worn_items_of(s_.inventory_); }

    std::vector<combat::IncomingHit> inbox;
    std::vector<combat::KillReward>  kills;
    std::vector<combat::EffectHitNote> effect_notes;   // the hits of the effects this player put on creatures, to confirm to his client
    std::vector<combat::DealtHit>      dealt;          // the hits of his pet and his reactions on creatures, to confirm to his client
    std::vector<std::pair<int64_t, int64_t>> pet_hits; // the hits of his pet that land on him: (hit record, ability)
    std::vector<combat::EffectRefusal> effect_refusals; // A3: the crowd controls of his (or his pet's) hits a mob refused, to tell his client
    bool                             wants_in_combat = false;
    std::optional<std::array<uint32_t, 4>> resurrect_by; // who just raised this dead player (abilities between players)
    std::vector<std::pair<uint64_t, uint8_t>> pvp_notices;   // PvP: the first flagging's (the other player's pawn, the intention)
    std::vector<combat::PvpKill>     pvp_kills;
    bool                             wants_pvp_combat = false;

private:
    PlayerSession&       s_;
    std::vector<int64_t> aligns_;
};

}}  // namespace aoc::worldd
