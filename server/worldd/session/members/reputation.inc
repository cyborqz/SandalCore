// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/reputation.inc: the members of PlayerSession for node reputation (session/player_reputation.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
private:
    /// The character's reputation by node (world/node_reputation.h), as the login read it and the session changed it.
    std::map<int64_t, world::NodeRep> node_rep_;
    bool   node_rep_changed_ = false;   // once true, every progress hand-over carries the whole list
    size_t reputation_log_ = 0;
    size_t reputation_gains_ = 0;
    /// The world's rules (the data's numbers for a session without a world).
    const world::ReputationRules& reputation_rules_of() const;
    /// The node's display name, or "node <guid>".
    std::string node_display_name(int64_t node) const;
    /// At the join: the saved reputations, decayed to now, and what the purse kept before reputation was per node (moved to the citizen node).
    void init_node_reputation(const svc::Character& character);
    /// OwnerInfo.NodeToPlayerReputation from node_rep_ (sent with the next OwnerInfo update).
    void set_node_reputation_state();
    void on_gm_grant_reputation(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now, bool citizens);
public:
    size_t reputation_gains() const { return reputation_gains_; }
    /// The character's reputation with `node` now (decayed; 0 for none).
    double node_reputation(int64_t node) const;
    /// ENodeReputationRank with `node` (world::kNoReputationRank when the character is no citizen of it: the data's rank questions are about the settlement asked of).
    int reputation_rank_at(int64_t node) const;
    /// Adds `delta` to the character's reputation with `node`, within the node's range; OwnerInfo and the save follow.  Returns what was added.
    double give_node_reputation(Connection& conn, int64_t node, double delta, const std::string& why);
    /// The character's reputation with each node it has any with, and its rank there (as text).
    std::string reputation_text() const;
    /// A shop's tax for this character: `rate` scaled by its rank's TaxModifier at `node` (unchanged for no rank).
    vendor::TaxRate reputation_taxed(vendor::TaxRate rate, int64_t node) const;
