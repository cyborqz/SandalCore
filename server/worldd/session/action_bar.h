// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/action_bar.h
//
//  The character's action bar (plan D1, stage 2) as pure data: the slots of the banks the client shows (the four class pages 0-3 of 12
//  slots, the weapon bank 4, the pet, mount, vehicle and fishing banks ...), each holding what FActionbarSlotUseData says.
//
//  What the retail captures show [R: scratchpad research/stage2/evidence.md section 6]: the whole bar is one ClientReceiveActionbarSlotPayload
//  on the PlayerController, sent about 20-40 packets after ServerRequestClientLoad; the client arranges its own bar and reports each change
//  with ServerSetActionbarSlotUseData (no reply; a UseType of 0 clears the slot); a bought skill gets a ClientUpdateSkillOnActionbar with a
//  placement hint. Whether the retail server places a bought skill on the stored bar itself, or the client reports it, or the bar is saved
//  some other way, is not in the captures.
//
//  Modelled, not captured (the part-2 spec's "Settled in D1, stage 2"):
//    - a bought or granted skill with an ability is placed by worldd on the first empty slot of the banks 0-3 (so that it survives a relog):
//      a DESIGN CHOICE, not retail behaviour; the client is told with the usual ClientUpdateSkillOnActionbar, and a ServerSetActionbarSlotUseData
//      that puts the same skill elsewhere moves it (a skill the server placed is on one slot at most);
//    - a slot the client sets must be inside the banks 0-19 and slots 0-15, hold an enumeration value the client has and a rarity and combo
//      button of 0-15; the bar holds at
//      most kMaxSlots slots; anything else is refused and nothing changes;
//    - a character with no stored bar gets the default: the weapon bank as the captured Bard's (Guard, Sprint, the weapon swap, Dodge) and
//      its class's auto-granted skills with an ability on the first empty slots.
// ============================================================================
#pragma once

#include "protocol/repl/skill_rpc.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::actionbar {

namespace repl = ::aoc::protocol::repl;

inline constexpr int    kMaxBank = 19;        // Fishing; the banks past it do not exist
inline constexpr int    kMaxSlot = 15;
inline constexpr int    kClassBanks = 4;      // banks 0-3: the class pages
inline constexpr int    kClassSlots = 12;     // slots 0-11 of a class page
inline constexpr size_t kMaxSlots = 256;

/// The weapon bank's abilities in the captured Bard's bar (slot 3 holds the weapon swap, whose AbilityId is Dodge's, as captured).
struct WeaponBank {
    int64_t guard = 0, sprint = 0, dodge = 0;
};

class Bar {
public:
    /// Sets a slot, or clears it when the use type is None. False, with `why`, for a bank or slot outside the banks, an enumeration value
    /// the client does not have, a rarity or combo button above 15 (the wire's 4 bits: the character service refuses a whole progress save for
    /// one), or a bar that would hold more than kMaxSlots.
    bool set(const repl::ActionbarSlot& slot, std::string* why = nullptr);

    /// The slots by bank then slot (what is stored and saved).
    std::vector<repl::ActionbarSlot> slots() const;
    /// The slots as the retail server lists them in ClientReceiveActionbarSlotPayload: the weapon bank's slots 1-4 each three times, in the
    /// captured order (1, 2, 4, 3, three times: capture 873 line 131; retail lists it two or three times); every other slot once, by bank
    /// then slot. A client that is sent the weapon bank once sets it back itself within 120 ms of the load (the D1 draft's log): the retail
    /// payload is the one it is known to take as it is, so the three copies are the probe for the bar animation.
    std::vector<repl::ActionbarSlot> wire_slots() const;
    size_t size() const { return slots_.size(); }
    bool   empty() const { return slots_.empty(); }
    const repl::ActionbarUseData* at(int bank, int slot) const;
// built by C-O-A with Cyb

    /// The first empty slot of the class pages.
    std::optional<std::pair<int, int>> first_free() const;
    /// Whether a Skill slot of the class pages holds the rank.
    bool has_skill(int64_t rank_guid) const;
    /// Puts the ability of a skill rank on the first free class slot (UseType Skill: the rank's ability and the rank); nullopt when the rank is
    /// on the bar already or the class pages are full.
    std::optional<repl::ActionbarSlot> place_skill(int64_t ability_guid, int64_t rank_guid);
    /// Takes every Skill slot whose rank is in `ranks` off the bar; the slots it freed, in order.
    std::vector<std::pair<int, int>> remove_skills(const std::set<int64_t>& ranks);
    /// Takes every other slot that holds the Skill `rank` off the bar but the one at (bank, slot); the slots it freed.
    std::vector<std::pair<int, int>> remove_other_copies(int64_t rank_guid, int bank, int slot);

    /// The bar a character has before it ever arranged one: the weapon bank. (The class skills are placed on top of it.)
    static Bar defaults(const WeaponBank& weapon);

    /// How many times retail lists each of the weapon bank's slots 1-4 in a payload.
    static constexpr int kWeaponBankCopies = 3;

    bool operator==(const Bar& o) const { return slots_ == o.slots_; }

private:
    std::map<std::pair<int, int>, repl::ActionbarUseData> slots_;
};

}  // namespace aoc::worldd::actionbar
