// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/quest_book.h  (quests milestone, slice 1, "take a quest")
//
//  The quests of the data, read whole: each quest with its objectives, the quests an NPC offers (and the node its talk starts at while one is on offer), the nodes that offer
//  a quest, the quests a zone of influence starts and the items bound to quests.
// ============================================================================
#pragma once

#include "content/content_db.h"

#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::quest {

class QuestBook {
public:
    QuestBook() = default;
    explicit QuestBook(content::QuestData data) : d_(std::move(data)) {}

    const content::QuestRow* quest(int64_t guid) const {
        const auto it = d_.quests.find(guid);
        return it == d_.quests.end() ? nullptr : &it->second;
    }
    const content::QuestRow* by_name(const std::string& name) const {
        const auto it = d_.by_name.find(name);
        return it == d_.by_name.end() ? nullptr : quest(it->second);
    }
    /// A quest named by its record id (digits) or its design name; null for neither.
    const content::QuestRow* find(const std::string& ref) const {
        if (ref.empty()) return nullptr;
        bool digits = true;
        for (const char c : ref) digits = digits && std::isdigit(static_cast<unsigned char>(c));
        if (digits && ref.size() < 19) return quest(std::stoll(ref));
        return by_name(ref);
    }

    /// The quests an NPC offers: its own instance's list, else its asset's (the instance wins, as it does for the conversation); empty for an NPC that offers none.
    const std::vector<content::QuestOfferRow>& offers(int64_t instance, int64_t asset) const {
        static const std::vector<content::QuestOfferRow> none;
        if (const auto it = d_.instance_offers.find(instance); it != d_.instance_offers.end()) return it->second;
        if (const auto it = d_.asset_offers.find(asset); it != d_.asset_offers.end()) return it->second;
        return none;
    }
    const content::QuestAcceptRow* accept(int64_t node) const {
        const auto it = d_.accepts.find(node);
        return it == d_.accepts.end() ? nullptr : &it->second;
    }
    /// The quests the zone of influence starts for a character who stands in it.
    const std::vector<int64_t>& zone_quests(int64_t zoi) const {
        static const std::vector<int64_t> none;
        const auto it = d_.zoi_quests.find(zoi);
        return it == d_.zoi_quests.end() ? none : it->second;
    }
    /// The quests an item is bound to (null: none).
    const std::vector<content::QuestItemLink>* item_links(int64_t item) const {
        const auto it = d_.item_quests.find(item);
        return it == d_.item_quests.end() ? nullptr : &it->second;
    }
    /// Quests, slice 2: the population sets (of the Kill objectives) a creature stands in; empty for none.
    const std::vector<int64_t>& kill_sets(int64_t instance) const {
        static const std::vector<int64_t> none;
        const auto it = d_.kill_sets.find(instance);
        return it == d_.kill_sets.end() ? none : it->second;
    }
    /// ... the interactable definitions of the Emberspring of a place (an attune point); empty for none.
    const std::vector<int64_t>& shrine_icds(int64_t place) const {
        static const std::vector<int64_t> none;
        const auto it = d_.shrine_icds.find(place);
        return it == d_.shrine_icds.end() ? none : it->second;
    }
    /// ... and the crate type of a crate item (0: the item is no crate).
    int64_t crate_of(int64_t item) const {
        const auto it = d_.item_crate.find(item);
        return it == d_.item_crate.end() ? 0 : it->second;
    }
    size_t quests() const { return d_.quests.size(); }
    const content::QuestData& data() const { return d_; }

    /// Every reward table a quest can hand out: its own and its objectives', the ones its actions give (GrantItem), and the ones of the quest items that creatures drop for an objective.
    /// The world reads them into the reward book that the quests roll.
    std::set<int64_t> reward_tables() const {
        std::set<int64_t> out;
        const auto take = [&](const std::vector<content::QuestAction>& list) {
            for (const content::QuestAction& a : list) {
                if (a.reward_table != 0) out.insert(a.reward_table);
            }
        };
        for (const auto& [guid, q] : d_.quests) {
            out.insert(q.rewards.begin(), q.rewards.end());
            take(q.start_actions);
            take(q.on_success);
            take(q.on_fail);
            take(q.on_tick);
            for (const content::QuestObjectiveRow& o : q.objectives) {
                out.insert(o.rewards.begin(), o.rewards.end());
                take(o.on_start);
                take(o.on_tick);
                take(o.on_success);
                take(o.on_fail);
                for (const content::QuestObjectiveRow::ItemReward& ir : o.item_rewards) {
                    if (ir.table != 0) out.insert(ir.table);
                }
            }
        }
        out.erase(0);
        return out;
    }

private:
    content::QuestData d_;
};

}  // namespace aoc::worldd::quest
