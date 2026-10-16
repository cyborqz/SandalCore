// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  foliage/foliage_cells.h  (Gathering stage 3, part "Foliage gathering")
//
//  How the clients learn which painted trees and plants are cut: one AIntrepidStateIFA per foliage owner (a 25,600 cm cell), held in the world's actor registry
//  (world/actor_registry.h, ActorKind::Foliage) so that every connection's ActorView opens, updates and closes it with the rest of the world.  The map the client paints is its own
//  (the instances are in its level); this actor only lists the DEVIATIONS: a UIntrepidFoliageStateComponent for each IntrepidGatherableFISMC_<n> of the owner, and in it
//  `IndexedStates`, a fast array of {Key = the instance's index, States = its EFoliageState} for the instances that are not at their painted state [R: capture_foliage.h].
//
//  The actor.  EVERY cell has one from the start (make_all), cut or not.  The client drops a foliage hit unless the cell's AIntrepidStateIFA and the instance's component exist with
//  bInitialized (the user's test 1: when an actor was made only at a cell's first cut, no untouched tree, plant or stone had a prompt); retail sends them at login, 7 cells right after the
//  pawn [R: capture 1198, packets 244 to 313].  Its open is retail's, to the bit [R: 1198's seven, 1428 pkt 139 and 388: 520 bits; 1463 pkt 835, 592 bits]: AuthServerIDReplicated,
//  ReplicatedMovement (an AActor's whole numbers: the cell's centre, ((cx + 0.5) * 25,600, (cy + 0.5) * 25,600), z the middle of its instances' lowest and highest), RemoteRole 1, Role 4
//  and FoliageName, then a component for every FISMC of the owner (FoliageComponentUUID, bInitialized) in byte order, the one that changed carrying its IndexedStates (an untouched
//  component has none: 283 bits).  A harvest is an item {Key, States 3 (Empty)}; regrowth changes the item (0 Infancy, 1 Juvenile) and an instance back at its painted state is DELETED
//  from the array [R: 1428 pkt 139's deletes].
//
//  Who hears.  The actor is a snapshot-only actor of the view: every connection with the cell's centre within the snapshot ring (about 350 m) is sent it, opened and closed with Dormancy
//  265 ms later [R: capture 1463's open to its close], and again whenever a change moves its version.  The view keeps the shadow of each component's array between opens, so a
//  re-open sends only what changed since the connection last had it (retail's re-opens carry the one changed component's delta and, for the deleted instance, a delete); a connection
//  that never had the cell is sent the whole list, its Empty instances as 11 (the login's snapshot, capture 1198: 620 of 948 items; a live harvest is 3) unless they were cut within the last
//  kLiveWindow seconds (then 3: the client plays the tree's fall for a harvest it is watching, and the first harvest of a cell is its actor's first open) [modelled: 11 is read from one capture].
// ============================================================================
#pragma once

#include "foliage/foliage_map.h"
#include "foliage/foliage_store.h"
#include "protocol/emit/intrepid_netguid.h"
#include "world/actor_registry.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::foliage {

/// [R: capture 1198's login snapshot] how an Empty instance is listed to a connection that never had its cell: Empty (3) with the bit 8.
inline constexpr uint32_t kEmptyInFirstOpen = 11;
/// [R: capture 1463, the open to its Dormancy close] how long a state actor stays open.
inline constexpr std::chrono::milliseconds kStateActorOpen{265};
/// [modelled] an Empty instance cut this recently is still listed as 3 (a live harvest) to a connection's first look at its cell.
inline constexpr double kLiveWindow = 10.0;

class FoliageCells {
public:
    using Minter = std::function<protocol::emit::FIntrepidNetworkGUID()>;

    /// `registry` and `map` must outlive it.  `mint` gives the actors' and components' NetGUIDs (worldd's allocator).
    FoliageCells(world::ActorRegistry& registry, const FoliageMap& map, Minter mint);

    /// The store's clock, in seconds: what the changes that follow are stamped with (the Empty instances of a first look are listed as 3 or 11 by their age).
    void set_now(double seconds) { now_ = seconds; }

    /// Makes the state actor of every cell of the map, every component initialized and its IndexedStates empty (nothing is cut): what a client needs to find any instance (see above).
    /// Returns how many were made (the cells that had none).  The view sends one to a connection when the cell's centre is within the snapshot ring.
    size_t make_all();

    /// Shows the changes to the clients: sets, changes or deletes the instance's item in its cell's actor (made here when make_all did not).  Returns how many cell actors were made by
    /// this call.  A row the map does not have is skipped (and counted in skipped()).
    size_t apply(const std::vector<Change>& changes);
// Cyb / C-O-A

    /// The registry id of an owner's actor (0: none yet).
    uint64_t actor_of(uint16_t owner) const { return owner < owners_.size() ? owners_[owner].actor : 0; }
    size_t   actors() const;
    uint64_t applied() const { return applied_; }
    uint64_t skipped() const { return skipped_; }
    /// The state the instance's item holds now (nullopt: it has none: the instance is at its painted state, or its cell has no actor).
    std::optional<uint32_t> shown_state(uint32_t row) const;
    /// The items the cell's actor holds (tests, the log).
    size_t items_of(uint16_t owner) const;

    /// The actor's FoliageName as the client says it: the owner's name.
    static const char* class_name() { return "AIntrepidStateIFA"; }

private:
    struct Owner {
        uint64_t actor = 0;                  // the registry's id (0: none yet)
        std::vector<size_t> components;      // component index -> index into the actor's subobjects
        size_t items = 0;
    };
    bool make_actor(uint16_t owner);
    /// The first-open form of the owner's actor (a copy made for a connection that never had it): its Empty instances older than kLiveWindow are listed as kEmptyInFirstOpen.
    void first_open_form(protocol::repl::Actor& actor, uint16_t owner) const;

    world::ActorRegistry& registry_;
    const FoliageMap&     map_;
    Minter                mint_;
    std::vector<Owner>    owners_;
    std::unordered_map<uint32_t, int32_t> item_of_row_;   // row -> the item's ReplicationID in its component's array
    std::unordered_map<uint32_t, uint32_t> state_of_row_; // row -> the state its item holds
    std::unordered_map<uint32_t, double>   changed_at_;   // row -> when its item last changed (the store's seconds)
    double                                 now_ = 0;
    uint64_t applied_ = 0, skipped_ = 0;
};

}  // namespace aoc::worldd::foliage
