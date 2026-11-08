// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Random coffers and choose-one selectors: contributed by war-drayv in #16.
// ============================================================================
//  session/player_chests.cpp
//
//  The two kinds of chest an item can be:
//
//    * a random coffer (content.db's item_box): its use rolls the reward table with combat::RewardBook and the rolled items go into the bags (the direct form; the
//      loot window is a later refinement);
//    * a selector (content.db's item_reward_selection, Activation = OpenRewardSelection): its use opens the client's reward-selection window
//      (ClientOpenRewardSelection with the options as one FItemInstance each); the player answers ServerSelectReward(Index), the chosen item is granted and the box is
//      consumed.  ServerCancelSelectReward closes the window and leaves the box.  The window lists the options in table order, the index is 0-based,
//      the grant is an Add and the chest's removal a Remove (ClientReceiveStorageEvent).
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/rewards.h"
#include "content/content_db.h"
#include "items/acquired.h"
#include "protocol/repl/rpc_fields.h"
#include "world/city_nodes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;

namespace {

/// The options one selector may list (the exe's reward instance limit is 0x11111; a selector has 2..9 in the data).
constexpr size_t kMaxSelectionOptions = 64;

/// A fresh instance of `record` at `quality` and `quantity`, as the reward tables make one: a new guid, the record's default quality when the caller
/// gives none, and its durability there.
svc::InventoryItem fresh_reward(int64_t record, int quality, const std::string& guid, int64_t created_ms, int64_t quantity, const content::ContentDb& db) {
    std::string error;
    const auto rec = db.item(record, error);
    svc::InventoryItem item;
    item.item_record_id = record;
    item.instance_guid = guid;
    item.quantity = static_cast<int32_t>(std::max<int64_t>(1, quantity));
    item.quality = quality > 0 ? quality : (rec ? std::max(1, rec->default_quality) : 1);
    item.durability = rec ? static_cast<float>(rec->max_durability_at(item.quality)) : 0.0f;
    item.created_at_ms = created_ms;
    return item;
}

}  // namespace

bool PlayerSession::open_chest(Connection& conn, const repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now) {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db) return false;
    std::string error;
    const content::ContentDb::ChestTable chest = db->chest_table(item.item_record_id, error);
    if (!error.empty()) {
        spdlog::warn("[chests] {}: item {} is not opened: {}", name(), item.item_record_id, error);
        return false;
    }
    if (chest.kind == content::ContentDb::ChestKind::Selector) return open_reward_selection(conn, use, item, chest.table, now);
    if (chest.kind == content::ContentDb::ChestKind::Random) return open_coffer(conn, use, item, chest.table, now);
    return false;
}

bool PlayerSession::open_reward_selection(Connection& conn, const repl::ItemUse& use, const svc::InventoryItem& item, int64_t table, Clock::time_point now) {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || !catalog_ || !room_for(2)) return false;
    std::string error;
    auto tables = db->loot_tables({table}, error);
    if (!tables) {
        spdlog::warn("[chests] {}: the selector {} has no reward table {}: {}", name(), item.item_record_id, table, error);
        return false;
    }
    // The options in the table's own order (the client's Index is 0-based over this list): the root's containers, then its sub tables.
    std::vector<content::LootItem> lines;
    std::function<void(int64_t)> collect = [&](int64_t guid) {
        const auto t = tables->find(guid);
        if (t == tables->end()) return;
        for (const content::LootContainer& c : t->second.containers) {
            for (const content::LootReward& r : c.rewards) {
                for (const content::LootItem& li : r.items) lines.push_back(li);
            }
        }
        for (const int64_t sub : t->second.subs) collect(sub);
    };
    collect(table);
    if (lines.empty() || lines.size() > kMaxSelectionOptions) {
        spdlog::warn("[chests] {}: the selector {} lists {} options: {}", name(), item.item_record_id, lines.size(),
                     lines.empty() ? "nothing to show" : "more than the window holds");
        return false;
    }
    // A line's Quantity is an expression id: the same evaluation the rolls use turns it into the stack the window shows and the reward grants.
    const combat::RewardBook* book = world_ ? world_->rewards() : nullptr;
    std::optional<combat::RewardBook> local_book;
    if ((!book || !book->table(table)) && world_ && world_->ability_catalog()) {
        local_book.emplace(*world_->ability_catalog(), std::move(*tables), 0);
        book = &*local_book;
    }
    combat::RollContext quantity_ctx;
    std::string quantity_error;
    auto quantity_run = gather_run(quantity_error);
    if (quantity_run) {
        quantity_ctx.npc = quantity_run.get();
        quantity_ctx.player_level = progress_.level;
        quantity_ctx.loot_max_level = progress_.level;
        quantity_ctx.rng = &chest_rng_;
    }
    std::vector<RewardSelectionOption> options;
    options.reserve(lines.size());
    for (const content::LootItem& line : lines) {
        const int64_t quantity = book && quantity_run ? book->quantity(line.qty, quantity_ctx) : 1;
        options.push_back({line.item, quantity});
    }
    const int64_t created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    repl::RewardInstance reward;
    reward.source_reward_tables.emplace_back(static_cast<uint64_t>(table), 0);
    for (const RewardSelectionOption& option : options) {
        const svc::InventoryItem fresh = fresh_reward(option.item, 0, fresh_guid(), created_ms, option.quantity, *db);
        reward.items.push_back(items::to_instance(fresh));
    }
    // The window names the chest's own instance (the client's optimistic-concurrency key): the same ExpectedItem the use carried.
    const repl::GuidValue activated = use.expected_item;
    queue("ClientOpenRewardSelection", [activated, reward, table](bootstrap::PlayerBootstrap& pb) {
        return pb.reward_selection(activated, reward, static_cast<uint64_t>(table));
    }, conn, /*must_build=*/true);
    reward_selection_ = RewardSelection{};
    reward_selection_->item_record = item.item_record_id;
    reward_selection_->instance_guid = item.instance_guid;
    reward_selection_->container = static_cast<int>(item.container);
    reward_selection_->slot = item.slot;
    reward_selection_->table = table;
    reward_selection_->options = std::move(options);
    spdlog::info("[chests] {}: the selector {} lists {} options (table {})", name(), item.item_record_id, reward_selection_->options.size(), table);
    return true;
}

bool PlayerSession::open_coffer(Connection& conn, const repl::ItemUse& use, const svc::InventoryItem& item, int64_t table, Clock::time_point now) {
    (void)use;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const combat::RewardBook* book = world_ ? world_->rewards() : nullptr;
    if (!db || !catalog_ || !room_for(16)) return false;
    // A world that has not started its aftermath (a test) has no book of the area's tables: read this chest's table on its own.  A world
    // whose book lacks the table (an item's table is not a mob's or a quest's) is completed the same way.
    std::optional<combat::RewardBook> local_book;
    if ((!book || !book->table(table)) && world_ && world_->ability_catalog()) {
        std::string why;
        auto tables = db->loot_tables({table}, why);
        if (tables) {
            local_book.emplace(*world_->ability_catalog(), std::move(*tables), 0);
            book = &*local_book;
        } else {
            spdlog::warn("[chests] {}: the coffer {} has no reward table {}: {}", name(), item.item_record_id, table, why);
            return false;
        }
    }
    if (!book || !book->table(table)) {
        spdlog::warn("[chests] {}: the coffer {} has no rollable table {}", name(), item.item_record_id, table);
        return false;
    }
    std::string error;
    auto run = gather_run(error);
    if (!run) {
        spdlog::warn("[chests] {}: the coffer {} is not opened: no stat run ({})", name(), item.item_record_id, error);
        return false;
    }
    combat::RollContext ctx;
    ctx.npc = run.get();
    ctx.player_level = progress_.level;
    ctx.loot_max_level = progress_.level;
    ctx.rng = &chest_rng_;
    const auto at = checker_.good();
    if (const world::CityNodes* nodes = world_->city_nodes()) {
        const world::NodeSite site = nodes->node_at(at.x, at.y);
        ctx.nodes = nodes;
        ctx.node = site.node_rank;
        ctx.biome = site.biome;
    }
    const combat::Rolled rolled = book->roll({table}, ctx);
    gather::Harvest harvest;
    harvest.durability_loss = 0;
    harvest.items = rolled.loot;
    harvest.items.insert(harvest.items.end(), rolled.direct.begin(), rolled.direct.end());
    for (const combat::RolledXp& x : rolled.xp) {
        // A coffer's table gives Adventuring experience; node and weapon experience belong to the kill and quest paths (no chest table carries them today).
        if (x.type == "Adventuring") harvest.adventuring_xp += x.amount;
        else spdlog::debug("[chests] {}: the coffer's {} experience is not granted", name(), x.type);
    }
    harvest.tables = rolled.tables;
    harvest.gated = rolled.gated;
    HarvestFrame frame;
    frame.what = "the random coffer " + std::to_string(item.item_record_id);
    frame.harvest = &harvest;
    frame.tag = "chest";
    deliver_harvest(conn, now, frame);
    const items::Outcome consumed = inventory_.consume(item.container, item.slot, item.instance_guid, 1);
    if (consumed.ok) {
        apply_outcome(conn, consumed, "a coffer is opened", /*with_toasts=*/true);
    } else {
        spdlog::warn("[chests] {}: the coffer {} stayed: {}", name(), item.item_record_id, consumed.refusal);
    }
    spdlog::info("[chests] {}: the coffer {} rolled {} item(s){}", name(), item.item_record_id, harvest.items.size(),
                 rolled.currency ? " (currency is not granted yet)" : "");
    return true;
}

void PlayerSession::on_select_reward(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!reward_selection_) {
        spdlog::debug("[chests] {}: ServerSelectReward with no window open: ignored", name());
        return;
    }
    if (!rpc.call.decoded || !rpc.call.rpc) {
        spdlog::warn("[chests] {}: a ServerSelectReward does not read: the window stays", name());
        return;
    }
    // Index is an int32 with a default of 0, and UE writes a parameter at its default with a 0 send bit: choosing the FIRST option sends no
    // Index at all.  An absent value is therefore index 0, not "no index" (only a value of another kind is refused).
    const repl::Value* value = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "Index");
    const std::optional<int64_t> index = repl::rpcf::int_or(value, 0);
    if (!index || *index < 0 || static_cast<size_t>(*index) >= reward_selection_->options.size()) {
        spdlog::warn("[chests] {}: ServerSelectReward names no option of the window ({})", name(), index ? *index : -1);
        return;
    }
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || !catalog_) return;
    const RewardSelection selection = *reward_selection_;
    const svc::InventoryItem* box = inventory_.at(static_cast<svc::ItemContainer>(selection.container), selection.slot);
    if (!box || box->item_record_id != selection.item_record || box->instance_guid != selection.instance_guid) {
        spdlog::warn("[chests] {}: the selector {} is no longer where it was used: the window is dropped", name(), selection.item_record);
        reward_selection_.reset();
        return;
    }
    const RewardSelectionOption& option = selection.options[static_cast<size_t>(*index)];
    const int64_t record = option.item;
    const int64_t created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const svc::InventoryItem chosen = fresh_reward(record, 0, fresh_guid(), created_ms, option.quantity, *db);
    items::Outcome outcome = inventory_.grant({chosen}, *catalog_);
    if (!outcome.ok) {
        spdlog::warn("[chests] {}: the reward {} of the selector {} is not granted: {}", name(), record, selection.item_record, outcome.refusal);
        return;   // the window stays: the player can make room and pick again
    }
    const items::Outcome consumed = inventory_.consume(static_cast<svc::ItemContainer>(selection.container), selection.slot, selection.instance_guid, 1);
    if (!consumed.ok) {
        spdlog::warn("[chests] {}: the selector {} stayed after its reward: {}", name(), selection.item_record, consumed.refusal);
    } else {
        outcome.events.insert(outcome.events.end(), std::make_move_iterator(consumed.events.begin()), std::make_move_iterator(consumed.events.end()));
        outcome.equipment_changed = outcome.equipment_changed || consumed.equipment_changed;
        outcome.bags.insert(outcome.bags.end(), std::make_move_iterator(consumed.bags.begin()), std::make_move_iterator(consumed.bags.end()));
    }
    reward_selection_.reset();
    apply_outcome(conn, outcome, "a reward is chosen", /*with_toasts=*/true);
    spdlog::info("[chests] {}: the selector {} gave option {} (item {}, {} of them) and was consumed", name(), selection.item_record, *index, record, option.quantity);
}

void PlayerSession::on_cancel_select_reward(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    (void)rpc;
    (void)now;
    if (reward_selection_) {
        spdlog::info("[chests] {}: the selector {} window is cancelled; the box stays", name(), reward_selection_->item_record);
        reward_selection_.reset();
    }
}

void PlayerSession::register_chests_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerSelectReward", [](PlayerSession& s, const RpcContext& c) { s.on_select_reward(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerCancelSelectReward", [](PlayerSession& s, const RpcContext& c) { s.on_cancel_select_reward(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
