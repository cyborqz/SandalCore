// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/action_bar.cpp (see action_bar.h)
// ============================================================================
#include "session/action_bar.h"

namespace aoc::worldd::actionbar {

namespace {

constexpr uint8_t kMaxUseType = repl::ActionbarUseType::Dismount;     // 9
constexpr uint8_t kMaxInstigator = repl::ActionbarInstigator::Vehicle;   // 3
constexpr uint8_t kMaxRarityOrCombo = 15;   // the 4 bits the wire has, and the character service's CHECK: one more and the progress save is refused whole

}  // namespace

bool Bar::set(const repl::ActionbarSlot& slot, std::string* why) {
    const auto refuse = [&](const char* text) {
        if (why) *why = text;
        return false;
    };
    if (slot.bank_index < 0 || slot.bank_index > kMaxBank || slot.slot_index < 0 || slot.slot_index > kMaxSlot) return refuse("the bank or slot is outside the action bar");
    const repl::ActionbarUseData& u = slot.use;
    if (u.use_type > kMaxUseType || u.instigator > kMaxInstigator) return refuse("the slot's use type or instigator is not one the client has");
    if (u.item_quality < 0) return refuse("the slot's item quality is negative");
    if (u.item_rarity > kMaxRarityOrCombo) return refuse("the slot's item rarity is above 15");
    if (u.combo_button > kMaxRarityOrCombo) return refuse("the slot's combo button is above 15");
    const std::pair<int, int> key{slot.bank_index, slot.slot_index};
    if (u.use_type == repl::ActionbarUseType::None) {
        slots_.erase(key);
        return true;
    }
    if (slots_.size() >= kMaxSlots && slots_.find(key) == slots_.end()) return refuse("the action bar is full");
    slots_[key] = u;
    return true;
}

std::vector<repl::ActionbarSlot> Bar::slots() const {
    std::vector<repl::ActionbarSlot> out;
    out.reserve(slots_.size());
    for (const auto& [key, use] : slots_) out.push_back({key.first, key.second, use});
    return out;
}

std::vector<repl::ActionbarSlot> Bar::wire_slots() const {
    // retail's order inside a copy of the weapon bank: Guard (1), Sprint (2), Dodge (4), the weapon swap (3)
    constexpr int kOrder[] = {1, 2, 4, 3};
    std::vector<repl::ActionbarSlot> out;
    out.reserve(slots_.size() + 4 * (kWeaponBankCopies - 1));
    bool weapon_bank_listed = false;
    for (const auto& [key, use] : slots_) {
        const bool counted = key.first == repl::ActionbarBank::Weapon && key.second >= 1 && key.second <= 4;
        if (!counted) {
            out.push_back({key.first, key.second, use});
            continue;
        }
        if (weapon_bank_listed) continue;   // the first of its slots lists the whole block
        weapon_bank_listed = true;
        for (int copy = 0; copy < kWeaponBankCopies; ++copy) {
            for (const int slot : kOrder) {
                const auto it = slots_.find({repl::ActionbarBank::Weapon, slot});
                if (it != slots_.end()) out.push_back({repl::ActionbarBank::Weapon, slot, it->second});
            }
        }
    }
    return out;
}

const repl::ActionbarUseData* Bar::at(int bank, int slot) const {
    const auto it = slots_.find({bank, slot});
    return it == slots_.end() ? nullptr : &it->second;
}
// -- C-O-A . C Y B --

std::optional<std::pair<int, int>> Bar::first_free() const {
    for (int bank = 0; bank < kClassBanks; ++bank) {
        for (int slot = 0; slot < kClassSlots; ++slot) {
            if (!slots_.count({bank, slot})) return std::make_pair(bank, slot);
        }
    }
    return std::nullopt;
}

bool Bar::has_skill(int64_t rank_guid) const {
    for (const auto& [key, use] : slots_) {
        if (key.first < kClassBanks && use.use_type == repl::ActionbarUseType::Skill && use.skill_rank_id == rank_guid) return true;
    }
    return false;
}

std::optional<repl::ActionbarSlot> Bar::place_skill(int64_t ability_guid, int64_t rank_guid) {
    if (has_skill(rank_guid)) return std::nullopt;
    const auto free = first_free();
    if (!free) return std::nullopt;
    repl::ActionbarSlot s;
    s.bank_index = free->first;
    s.slot_index = free->second;
    s.use.use_type = repl::ActionbarUseType::Skill;
    s.use.ability_id = ability_guid;
    s.use.skill_rank_id = rank_guid;
    slots_[*free] = s.use;
    return s;
}

std::vector<std::pair<int, int>> Bar::remove_skills(const std::set<int64_t>& ranks) {
    std::vector<std::pair<int, int>> freed;
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (it->second.use_type == repl::ActionbarUseType::Skill && ranks.count(it->second.skill_rank_id)) {
            freed.push_back(it->first);
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
    return freed;
}

std::vector<std::pair<int, int>> Bar::remove_other_copies(int64_t rank_guid, int bank, int slot) {
    std::vector<std::pair<int, int>> freed;
    for (auto it = slots_.begin(); it != slots_.end();) {
        const bool same = it->first == std::pair<int, int>{bank, slot};
        if (!same && it->first.first < kClassBanks && it->second.use_type == repl::ActionbarUseType::Skill && it->second.skill_rank_id == rank_guid) {
            freed.push_back(it->first);
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
    return freed;
}

Bar Bar::defaults(const WeaponBank& w) {
    Bar b;
    const auto ability = [&](int slot, uint8_t type, int64_t id) {
        repl::ActionbarSlot s;
        s.bank_index = repl::ActionbarBank::Weapon;
        s.slot_index = slot;
        s.use.use_type = type;
        s.use.ability_id = id;
        b.slots_[{s.bank_index, s.slot_index}] = s.use;
    };
    ability(1, repl::ActionbarUseType::Ability, w.guard);
    ability(2, repl::ActionbarUseType::Ability, w.sprint);
    ability(3, repl::ActionbarUseType::SwapWeapons, w.dodge);
    ability(4, repl::ActionbarUseType::Ability, w.dodge);
    return b;
}

}  // namespace aoc::worldd::actionbar
