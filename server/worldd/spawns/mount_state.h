// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/mount_state.h  (the mounts milestone, stage 3: riding)
//
//  What riding writes into a mount's actor of the world's registry (world/actor_registry.h, ActorKind::Mount): who possesses it, the stats and abilities its
//  rider's connection is given, the effects and cooldowns the dash and the roads put on it, and its Health.  The mount is one replicated actor: the actor view
//  (session/actor_view.h) sends its owner's connection whatever changed, on the mount's own channel.  Every function here changes the actor only; the caller
//  runs them inside `ActorRegistry::modify` so that the view hears of it.
//
//  Getting on, as the retail server wrote it [R: 910 packet 307, 871 packet 302]:
//    the actor            RemoteRole 2 (autonomous), Owner = the PlayerController, PlayerState, Controller = the PlayerController
//    StatsComponent       the rider's owner stat arrays (StatRepInt32Owner 114 items in 910 and OwnerProxy 140: the stats of the rider as its connection holds
//                         them: the HUD of a possessed pawn reads them).  Capture 910 also names the rider in StatRepInt32ProxyOnly.DelegateWrapper; the capture of
//                         871 (four days later) does not and has RiderPtr in the InteractInfo instead, and the replicator cannot write the custom-delta member
//                         (RepState::set refuses it): the mount is told its rider by RiderPtr alone
//    AbilityComponent     KnownAbilities = [the mount's dash]
//    InteractInfo         RiderPtr = the rider's pawn
//    InteractableComponent  InputEffectStatuses [input 2: status 134217732, was interacted with, by the rider's pawn] and CurrentInputStates [input 2: state 0]
//  Getting off [R: 912 packet 139]: RemoteRole 1, no Owner, PlayerState or Controller, RiderPtr 0, CurrentInputStates [input 2: state 2, the
//  owner's guid], and the mount's ReplicatedMovement where it stands (a possessed mount has none: the retail server sends its owner no movement).
// ============================================================================
#pragma once

#include "bootstrap/ability_replication.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/replicator.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::spawns {

namespace repl = ::aoc::protocol::repl;

/// The mount's subobject of a class ("UAoCStatsComponent", "UMountComponent" ...); nullptr when it has none.
repl::Subobject*       sub_of(world::WorldActor& actor, const char* cls);
const repl::Subobject* sub_of(const world::WorldActor& actor, const char* cls);

/// ReplicatedMovement (cm x 100, velocity in whole cm/s, the yaw as one byte), its timestamp, CurrentMovementInput, and the open's own location and rotation, as the
/// retail server writes a running mount (the desummon's run: session/mounts.cpp) and a mount left where it stands.
void write_motion(world::WorldActor& actor, double x, double y, double z, double yaw_degrees, double vx, double vy, bool moving, double timestamp);

/// The InputEffectStatus value of a mount's interaction [R: 910, 871: 134217732].
inline constexpr uint64_t kInputEffectStatus = 134217732;
/// The interaction input of a mount (Mount_Interaction is input 2 of its InteractableComponent's four).
inline constexpr int kMountInput = 2;

/// What possessing a mount writes (see the header).
struct Possession {
    protocol::emit::FIntrepidNetworkGUID player_controller, player_state, rider_pawn;
    int64_t                              dash_ability = 0;     // the mount's KnownAbilities
    std::vector<repl::ElementState>      owner_stats;          // the rider's StatRepInt32Owner items (each as the array holds it)
    std::vector<repl::ElementState>      owner_proxy_stats;    // its StatRepInt32OwnerProxy items
};
bool possess(world::WorldActor& mount, const Possession& p, std::string& error);

/// What getting off writes: the owner's guid goes back into the interaction state (the open's: input 2 state 2, its guid).
bool release(world::WorldActor& mount, const repl::GuidValue& owner, std::string& error);
// ~ C.Y.B - C-O-A ~

/// An EveryoneProxy stat of the mount (its StatsComponent's StatRepInt32EveryoneProxy item of `record`): its value, or nullopt when there is no such item.
std::optional<double> stat_value(const world::WorldActor& mount, int64_t record);
/// Sets it; false when the mount has no such item.
bool set_stat_value(world::WorldActor& mount, int64_t record, float value);

/// A CurrentEffects item with the rider's guid as its Instigator and Source; the item's array id, or nullopt when the array does not take it.
std::optional<int32_t> add_effect(world::WorldActor& mount, const repl::GuidValue& instigator, const bootstrap::EffectItem& item);
bool                   remove_effect(world::WorldActor& mount, int32_t array_id);
/// A Cooldowns item on the mount's AbilityComponent.
std::optional<int32_t> add_cooldown(world::WorldActor& mount, const bootstrap::CooldownItem& item);
bool                   remove_cooldown(world::WorldActor& mount, int32_t array_id);

/// RemoteViewPitch16 of the mount: the rider's view pitch (the move's control rotation pitch, a 16-bit angle) [R: 871, 959, 899, 883]. False when the actor has no such property.
bool set_view_pitch(world::WorldActor& mount, uint16_t pitch16);

/// The interactable component takes input (CanInputActivate for its four inputs) or not.
bool set_interaction_open(world::WorldActor& mount, bool open);

/// UNPCCombatInfo.bIsDead of the mount.
bool set_dead(world::WorldActor& mount, bool dead);

}  // namespace aoc::worldd::spawns
