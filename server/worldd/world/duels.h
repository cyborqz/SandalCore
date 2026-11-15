// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/duels.h  (PvP, slice 1, step 3: duels)
//
//  The world's duels, shared by the sessions of their two players (on the endpoint thread).  As the client has it (its duel code, read for PvP: the
//  APlayerCharacter duel RPCs, ADuelCenter's OnRep_DuelState, the /duel command):
//    the request   ServerRequestDuel(name): the book spawns the duel's banner, an ADuelCenter (BP_DuelCenter_C) halfway between the two, whose replicated
//                  Duel = {InstanceGuid, Participants [{Instigator 0, A}, {Recipient 1, B}], SyncBeginTime -1, bIsActive false} is what shows the recipient
//                  "Accept {A}'s Duel Request?" (OnRep_DuelState); there is no client RPC for it
//    the answer    ServerAcceptDuel / ServerDeclineDuel (Duel.InstanceGuid); unanswered, it lapses (the window's own timer declines it)
//    the countdown SyncBeginTime goes from -1 to the moment the fight begins (the synchronized clock): the client shows "Duel Beginning" and counts down
//    the fight     bIsActive true; both pawns hold the Duel alignment {InstanceGuid} (Duel -> Duel is Hostile: they harm each other without the force flag, and
//                  nobody is flagged for it); a blow that would kill leaves 1 Health and ends the duel (WinLoss)
//    the end       ServerForfeitDuel (the client sends the instigator's guid, which is the duel's), straying past the banner's radius for too long (Flee:
//                  each participant's ExitRadiusTimestamp marks when he left), the longest duel (TimeOut), a logout, a death or another player's harm
//                  (System); the banner goes, the alignments go, both get their Health back
//  The duel's guid is its instigator's character guid (so the forfeit's guid and the instance guid are one).
//  The numbers (kDuelSettings) are the book's own [modelled]: GameCombatDuelSettings replicates in the game state, but the retail server never sent it, so
//  the client's are its C++ defaults, which are not read yet.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/subobject.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc::protocol::schema { struct ClassSchema; }

namespace aoc::worldd::world {

class ActorRegistry;

using DuelGuid = std::array<uint32_t, 4>;

/// GameCombatDuelSettings as the book plays it [modelled: the client's C++ defaults are not read].
struct DuelSettings {
    double start_distance_cm = 3000;     // MinimumStartDistance: the two must be this close to start (the exe's "Out of range to duel")
    double radius_cm = 4000;             // MinimumDistanceToCenter: how far from the banner a participant may go
    double flee_seconds = 10;            // DistanceForfeitCountdownTimer: how long outside the radius before he loses (Flee)
    double countdown_seconds = 5;        // the countdown (Toast_Duel_Countdown_DuelBegin's CountdownDuration is 5)
    double max_seconds = 600;            // MaxDuelDuration (TimeOut)
    double answer_seconds = 30;          // how long a request waits for its answer
    double request_cooldown = 10;        // DuelRequestCooldown: after a decline, before the same pair again
    double health_recovery = 1.0;        // EndOfDuelHealthRecoveryPercentage: the share of MaxHealth both are given back at the end
};

/// EDuelResult.
enum class DuelResult : uint8_t { WinLoss = 0, Forfeit = 1, Flee = 2, TimeOut = 3, System = 4 };

const char* duel_result_name(DuelResult r);

/// What a session is told about a duel of its player's (DuelBook::take_notices).
struct DuelNotice {
    enum class Kind : uint8_t {
        Sent,       // his request went to `other` (the instigator)
        Asked,      // `other` asks him (the recipient: his client shows the window from the banner)
        Declined,   // `other` declined, or the request lapsed (`text` says which)
        Counting,   // `other` accepted: the countdown runs (both)
        Started,    // the fight began (both): the Duel alignment
        Ended,      // the duel ended (both): `result`, `won`, `text`
    };
    Kind        kind = Kind::Sent;
    DuelGuid    duel{};
    std::string other;          // the other participant's name
    DuelResult  result = DuelResult::System;
    bool        won = false;    // Ended: he won (WinLoss, Forfeit, Flee: the other lost); false for both in a TimeOut or a System end
    bool        fought = false; // Ended: the fight had begun (the end of a countdown or a request is no duel's result)
    std::string text;           // what his client is told (a callout)
};

class DuelBook {
public:
    enum class Phase : uint8_t { Asked, Counting, Fighting };
    struct Duel {
        DuelGuid    id{};
        DuelGuid    a{}, b{};          // the instigator (Role 0) and the recipient (Role 1)
        std::string a_name, b_name;
        Phase       phase = Phase::Asked;
        double      asked_at = 0, begins_at = -1, began_at = -1;
        double      cx = 0, cy = 0, cz = 0;   // the banner
        uint64_t    banner = 0;               // its actor (0: none, a book without a registry)
        double      a_out_since = -1, b_out_since = -1;   // the world time each left the radius (< 0: inside)
    };

    /// `registry`: where the banners go (null: none are made, the book still keeps the duels); `mint`: actor guids; `world_time_offset`: the synchronized
    /// clock is the world time plus it (bootstrap::effect_clock).
    DuelBook(ActorRegistry* registry, std::function<protocol::emit::FIntrepidNetworkGUID()> mint, DuelSettings settings = {});

    /// A player's session joins and leaves the book (by character guid, his name and his pawn's actor: where he stands is the registry's).  Leaving ends his
    /// duel (System) and drops his requests.
    void connect(const DuelGuid& player, const std::string& name, uint64_t pawn);
    void disconnect(const DuelGuid& player, double now);
    bool connected(const DuelGuid& player) const { return players_.count(player) != 0; }

    /// ServerRequestDuel: `from` asks the player named `name` (case as chat names compare).  "" on success, else what his client is told.
    std::string request(const DuelGuid& from, const std::string& name, double now);
    /// ServerAcceptDuel / ServerDeclineDuel by the recipient, for the duel `duel` (its InstanceGuid).  False when it is no request of his.
    bool accept(const DuelGuid& by, const DuelGuid& duel, double now);
    bool decline(const DuelGuid& by, const DuelGuid& duel, double now);
    /// ServerForfeitDuel: he gives his duel up (the request or the countdown is called off; a fight is lost).  `duel` is what the client sent (the instigator's
    /// guid); his own duel is ended whatever it says.
    bool forfeit(const DuelGuid& by, double now);
    /// A blow of his duel partner's would have killed him (the session left him 1 Health): he loses (WinLoss).
    void defeated(const DuelGuid& loser, double now);
    /// He died of something else, or another player harmed him or his partner: the duel ends (System).
    void interrupted(const DuelGuid& player, const std::string& why, double now);

    /// The requests' lapse, the countdown's end, the radius and the longest duel.
    void tick(double now);

    /// His duel (any phase), or null.
    const Duel* duel_of(const DuelGuid& player) const;
    /// His duel's key for the PvP rules (combat/pvp.h Side::duel) while it is fought; 0 otherwise.
    uint64_t fighting_key(const DuelGuid& player) const;
    /// His duel's guid while it is fought (the Duel alignment's instance), or nothing.
    std::optional<DuelGuid> fighting(const DuelGuid& player) const;
    /// His partner's pawn while the duel is fought (0: none).
    uint64_t partner_pawn(const DuelGuid& player) const;

    /// What his session is to tell him, taken.
    std::vector<DuelNotice> take_notices(const DuelGuid& player);

    size_t duels() const { return duels_.size(); }
    const DuelSettings& settings() const { return settings_; }
    /// The banner's schema (ADuelCenter) and archetype (BP_DuelCenter_C's default object).
    static const protocol::schema::ClassSchema* banner_schema();
    static const protocol::repl::ExportChain& banner_archetype();
    static constexpr const char* kBannerArchetype = "/Game/GameBlueprints/PC/BP_DuelCenter.Default__BP_DuelCenter_C";

private:
    struct Player {
        std::string name;
        uint64_t    pawn = 0;
        std::map<DuelGuid, double> declined;   // the instigators whose request he declined, and when (DuelRequestCooldown)
    };

    bool where(const DuelGuid& player, double& x, double& y, double& z) const;
    void spawn_banner(Duel& d);
    void update_banner(const Duel& d);
    void tell(const DuelGuid& player, DuelNotice n);
    /// The duel ends: both are told, the banner goes.  `loser`: who lost (null: nobody).
    void end(std::map<DuelGuid, Duel>::iterator it, DuelResult result, const DuelGuid* loser, const std::string& why);
    std::map<DuelGuid, Duel>::iterator find_of(const DuelGuid& player);

    ActorRegistry*                                        registry_;
    std::function<protocol::emit::FIntrepidNetworkGUID()> mint_;
    DuelSettings                                          settings_;
    std::map<DuelGuid, Player>                            players_;
    std::map<DuelGuid, Duel>                              duels_;     // by its guid (the instigator's)
    std::map<DuelGuid, std::vector<DuelNotice>>           notices_;
    size_t                                                logged_ = 0;
};

/// The key of a duel's guid for the PvP rules (never 0).
uint64_t duel_key(const DuelGuid& g);

}  // namespace aoc::worldd::world
