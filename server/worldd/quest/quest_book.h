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
    size_t quests() const { return d_.quests.size(); }
    const content::QuestData& data() const { return d_; }

private:
    content::QuestData d_;
};

}  // namespace aoc::worldd::quest
