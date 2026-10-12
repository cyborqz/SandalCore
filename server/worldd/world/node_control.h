// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/node_control.h  (Nodes 1, stage 2)
//
//  What the client's GM console may ask of the node state: the calls of the citynode service the shipping client's own GM RPCs map onto.
//
//    ServerSetNodeLevel(InNode, InLevel, InCulture, bDisableVassalshipSystem)   ->  SetNodeLevel
//    ServerAddNodeExperience(InNode, Experience, InCulture, bUseRawXP)          ->  AddNodeExperience
//    ServerGMGetNodeData(NodeName)                                              ->  GetNode (the answer goes to the log)
//    ServerGMWhatTimeIsIt()                                                     ->  GetClock (the answer goes to the log)
//    ServerGMFastForwardRealTime(FastForwardDuration)                           ->  AdvanceClock
//
//  The session calls these on worldd's network thread, so an implementation never blocks it (svc/city_node_control.h queues the calls to a worker).  Nothing is returned:
//  a change comes back through the registry's own feed (svc/city_node_feed.h), which updates every client's GameState, and a read answers in the log ("[gm] ...").
//  `by` names who asked ("ServerSetNodeLevel by Cyb") for those lines.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace aoc::worldd::world {

class NodeControl {
public:
    virtual ~NodeControl() = default;
    virtual void set_level(const std::string& node, int level, bool disable_vassalship, const std::string& by) = 0;
    virtual void add_experience(const std::string& node, int64_t amount, const std::string& by) = 0;
    virtual void node_data(const std::string& node, const std::string& by) = 0;
    virtual void clock(const std::string& by) = 0;
    virtual void advance_clock(int64_t seconds, const std::string& by) = 0;
};

}  // namespace aoc::worldd::world
