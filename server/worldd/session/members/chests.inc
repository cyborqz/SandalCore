// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Random coffers and choose-one selectors: contributed by war-drayv in #16.
// ============================================================================
//  session/members/chests.inc: the members of PlayerSession's chests (session/player_chests.cpp)
//  Included at the end of the class (session/members_registered.inc).
// ============================================================================
private:
    /// A selector whose window is open: the box, where it was used, its selection table and the options in window order.
    struct RewardSelectionOption {
        int64_t item = 0;
        int64_t quantity = 1;
    };
    struct RewardSelection {
        int64_t                            item_record = 0;
        std::string                        instance_guid;
        int                                container = 0;
        int32_t                            slot = -1;
        int64_t                            table = 0;
        std::vector<RewardSelectionOption> options;
    };
    /// True when the used item is a chest this session handles (a random coffer or a selector); the use then does not reach the generic item actions.
    bool open_chest(Connection& conn, const protocol::repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now);
    bool open_reward_selection(Connection& conn, const protocol::repl::ItemUse& use, const svc::InventoryItem& item, int64_t table, Clock::time_point now);
    bool open_coffer(Connection& conn, const protocol::repl::ItemUse& use, const svc::InventoryItem& item, int64_t table, Clock::time_point now);
    void on_select_reward(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_cancel_select_reward(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    std::optional<RewardSelection>     reward_selection_;
    /// The dice of a random coffer: a sequence of its own in every session.
    stats::SplitMix                    chest_rng_{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                                                   static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this))};
private:
