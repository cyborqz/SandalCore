// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/town_floor.h  (Nodes 1, test 2: the five city nodes' bowls)
//
//  The ground worldd bakes (dist/content/area_nav.bin, the client's own landscape) lies about 50.6 m BELOW the floor of the five city nodes' towns (Winstead, Joeva, New Aela, Halcyon,
//  Miraleth): the landscape under a town is a pit, and the town's level instance stands over it at the height of the land around.  Retail's people stand 50.6 m over the baked terrain (the
//  median of 500 to 600 NPCs per town; the artisan outposts' are +1 m), so a query of the baked ground in a bowl answers a height 50 m under every player and every NPC there.
//
//  This is the small correction for the places that have a PLAYER to put somewhere (a GM teleport, a login, the position saved for a rider in the air).  It is data-driven, from what
//  worldd already loads: the heights the town's people stand at (the layout's, retail's to 1-168 cm on the 9 anchors the tests hold).
//
//    a person of a town      one NPC the node's manager made: where it stands (x, y), the baked ground under it (`ground`) and its own height (`z`).  A person stands IN A BOWL when `z`
//                            is more than kBowlCm over the ground.
//    the town's lift         the median of those persons' heights over the ground (50.6 m at Joeva).  The persons that stand within kSameCm of it are the town's floor: the others are
//                            on a building's upper floor, a tower, or the wall of the pit.
//    in the town             a point is in the town's bowl when one of those persons stands within kReachCm of it (horizontally) and the baked ground under the point is within kReliefCm of
//                            the ground under that person (on the wall of the pit, and beyond the rim, the ground changes by more: nobody is lifted out of the open land round a town).
//                            There the town's floor is the baked ground under the point plus the lift (retail: z = 0.986 terrain + 5,247 cm, r 0.99; on the real ground, against retail's 9 anchors
//                            at Joeva it is within 2 m for 7 and 4 and 12 m for the two on a tower or a terrace).  On the real ground no NPC that stands on the open land (+1 m: 10 to 35 per
//                            town) is taken for the bowl, and 67 to 76 % of the 500 m round a node's place is within the 75 m reach of a person of the floor (the rest of the pit keeps the baked ground).
//
//  Not a navmesh: the people of a bowl stand where the data puts them and do not walk (npc/npc_world.h).  A GM teleport (PlayerWorld::ground_at), a login (lift_out_of_bowl) and the
//  position saved for a rider in the air (PlayerSession::ground_below) are corrected by this height and nothing else is.  The follow-ups (docs/backlog.md): the correction in every other
//  ground query (a mount's summon and landing, a respawn), a navmesh on the town floor so the people can walk.
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace aoc::worldd::world {

class TownFloor {
public:
    /// A person this far over the baked ground (cm) stands in a bowl (an open-world NPC is +1 m, a town's +50 m).
    static constexpr double kBowlCm = 1500.0;
    /// A person is on the town's floor when its height over the ground is within this of the town's lift.
    static constexpr double kSameCm = 300.0;
    /// A point is in the town when a person of its floor stands within this of it (horizontally).
    static constexpr double kReachCm = 7500.0;
    /// ... and the baked ground under the point is within this of the ground under that person.
    static constexpr double kReliefCm = 1000.0;
// C Y B + C.O.A

    struct Person {
        double x = 0, y = 0;
        double ground = 0;   // the baked ground under it
        double z = 0;        // its own height (the data's)
    };
    struct Floor {
        int64_t node = 0;    // the node whose town it is
        double  lift = 0;    // the town's floor over the baked ground (cm)
    };

    /// Replaces the people of node `node`.  A node none of whose people stand in a bowl has no town (an earlier one is dropped).
    void set(int64_t node, const std::vector<Person>& people);
    /// The town's floor at (x, y), whose baked ground is `ground`; none when the point is in no bowl.
    std::optional<Floor> at(double x, double y, double ground) const;

    /// Towns with a floor, and the persons that make up one node's floor (0 for none).
    size_t towns() const { return towns_.size(); }
    size_t floor_persons(int64_t node) const;
    /// The lift of one node's town (nullopt: no town).
    std::optional<double> lift_of(int64_t node) const;

private:
    struct Town {
        double              lift = 0;
        std::vector<Person> persons;   // the floor's
        double              x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // their bounds
    };
    std::map<int64_t, Town> towns_;
};

}  // namespace aoc::worldd::world
