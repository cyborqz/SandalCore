// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/professions.h
//
//  The character's professions (Gathering stage 2, part "Professions and GM items") as pure logic over content.db: the 22 CharacterProfessions,
//  each with its level-up curve (50 levels, 400 x level^2 XP: `mapc artisan-curves`), the five certification levels (Novice 0 ... GrandMaster 4) and
//  what a character holds in them: a certification tier, a level and an XP total per profession.
//
//  What the data and the retail captures show [R: the survey, 1198 pkt 1510's join, evidence.md]:
//    - the XPTracker's LevelingCertificationsFastMap lists the 22 professions (key = the profession's leveling type, value = the certification guid) at
//      every join, in the order of the CharacterProfessions guids; its ExperienceFastMap lists a profession only once it has XP (19 of 22 at that join);
//    - a certification level spans a range of profession levels (Novice 1-10, Apprentice 10-20, Journeyman 20-30, Master 30-40, GrandMaster 40-50);
//      the retail join's professions sit in the range of their certification (XP 150,451 = level 19 = Apprentice);
//    - the GM artisan scrolls repeat their XP after each Promote (`xp - cur + 1`: the XP from the start of the current level to the level asked, plus 1),
//      which only makes sense if a profession's XP stops at its tier's MaxLevel until it is promoted.
//
//  Modelled, not captured (the defaults and the decisions of this part):
//    - the level is the highest level of the curve whose XP total the XP reaches, held at the tier's MaxLevel; the XP is held just below the threshold of the
//      level past the cap (the Adventuring cap rule, leveling.h), or at the curve's last total at level 50;
//    - Promote is one tier up, from the tier the profession is in, and takes effect only when the profession is at its tier's MaxLevel (the XP before it
//      in the scrolls is there to bring it to the cap): a Promote below the cap is skipped and the item says so. Demote is one tier down: the level and the
//      XP are held at the lower tier's cap. A GM item ignores the certification's MaximumAllowable (a character may hold 5 Apprentice, 4 Journeyman ...);
//    - the XP of a Profession reward is the compiled expression of the reward (contentc's reward_xp.expr: `xp - cur + 1`) run with the live profession
//      level (GetPlayerProfessionLevel); it is added to the XP total, never taken away.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "session/leveling.h"
#include "stats/character_stats.h"
#include "svc/services.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// ~ C_Y_B & C O A ~

namespace aoc::worldd::abilities { class Catalog; }

namespace aoc::worldd::professions {

inline constexpr int kTierNovice = 0;
inline constexpr int kTierGrandMaster = 4;
inline constexpr int kTiers = 5;

/// One certification level (certification_level).
struct Tier {
    int64_t     guid = 0;
    std::string name;
    int         tier = 0;
    int         min_level = 1;
    int         max_level = 10;       // a profession's level stops here until it is promoted
    bool        limited = false;      // the certification caps how many professions a character may hold at it
    int         max_allowable = 0;
};

/// One profession with its curve.
struct Profession {
    int64_t              guid = 0;
    std::string          name;
    std::string          category;
    int64_t              leveling_type = 0;   // the XPTracker's key
    leveling::LevelCurve curve;
};

/// What a certification action does (action_certification): one tier up or one tier down for a profession.
struct CertAction {
    int64_t profession = 0;
    bool    promote = true;
};

/// The professions and the certification levels of content.db, built once at startup.
class Book {
public:
    /// nullopt, with `error`, for data that is not what the rules need: not 22 professions' worth of curve, a curve that is not a level curve, certification levels
    /// that are not tiers 0 to 4 in order with rising caps, an action naming a profession that is none.
    static std::optional<Book> from(const content::ProfessionData& data, std::string& error);

    /// The professions in guid order: the order the character service stores them in and the join lists them in.
    const std::vector<Profession>& all() const { return professions_; }
    size_t                         size() const { return professions_.size(); }
    const Profession*              find(int64_t guid) const;
    /// A profession by its name, without regard to case (FNames: the expressions say "Lumbermilling" where the record is LumberMilling).
    const Profession*              named(std::string_view name) const;
    /// The index of a profession in all() (-1: none).
    int                            index_of(int64_t guid) const;
    /// The certification level of a tier (clamped to 0 .. 4).
    const Tier&                    tier(int tier) const { return tiers_[static_cast<size_t>(tier < 0 ? 0 : tier >= kTiers ? kTiers - 1 : tier)]; }
    /// The tier of a certification level guid (-1: none).
    int                            tier_of_guid(int64_t guid) const;
    /// The action response guid's meaning, when it is a certification action.
    const CertAction*              action(int64_t action_guid) const;

    /// The XP total a profession at `tier` holds at most: one below the threshold of the level past the tier's cap, or the curve's last total at its last level.
    int64_t max_xp(const Profession& p, int tier) const;
    /// The level an XP total gives a profession at `tier`: the highest level the curve reaches, held at the tier's MaxLevel.
    int     level_of(const Profession& p, int64_t xp, int tier) const;

private:
    std::vector<Profession>          professions_;
    std::vector<Tier>                tiers_;
    std::map<int64_t, CertAction>    actions_;
};

/// What a profession holds.
struct State {
    int     tier = kTierNovice;
    int     level = 1;
    int64_t xp = 0;
    bool operator==(const State&) const = default;
};

/// One profession's change by one or several steps (before and after; the profession's guid).
struct Change {
    int64_t profession = 0;
    State   before, after;
    bool leveled() const { return after.level > before.level; }
    bool promoted() const { return after.tier > before.tier; }
    bool demoted() const { return after.tier < before.tier; }
    bool changed() const { return !(before == after); }
    /// The level or the tier moved (what a player sees; XP alone is not a change a scroll is worth using for).
    bool moved() const { return before.level != after.level || before.tier != after.tier; }
};

/// What a character holds in the 22 professions. A default-constructed set has no book: it answers Novice, level 1 for everything and refuses every change.
class Professions {
public:
    Professions() = default;
    /// The 22 at Novice, level 1, no XP.
    explicit Professions(const Book& book);
    /// From the character record: a profession with no row is Novice, level 1, no XP. A row is trusted as the leveling rules would have it: the tier 0-4, the
    /// level at least what the XP gives and at most the tier's MaxLevel, the XP at least the level's threshold and at most what the tier holds. `notes` (optional)
    /// says what was corrected or left out.
    static Professions load(const Book& book, std::span<const svc::ProfessionState> rows, std::vector<std::string>* notes = nullptr);

    bool        valid() const { return book_ != nullptr; }
    const Book* book() const { return book_; }

    State state(int64_t profession) const;
    int     level(int64_t profession) const { return state(profession).level; }
    int     tier(int64_t profession) const { return state(profession).tier; }
    int64_t xp(int64_t profession) const { return state(profession).xp; }
    /// Whether the profession is certified at `tier` or above and at `level` or above (a tool, a node or a recipe asks this).
    bool    can_use(int64_t profession, int tier, int level) const;

    /// Adds XP (0 or less adds nothing): the level follows the curve, held at the tier's cap, the XP at what the tier holds.
    Change add_xp(int64_t profession, int64_t delta);
    /// One tier up. With `needs_cap` the profession must be at its tier's MaxLevel (a Promote below it is skipped: `skipped` true, nothing changes); a GrandMaster stays.
    Change promote(int64_t profession, bool needs_cap = true, bool* skipped = nullptr);
    /// One tier down (a Novice stays): the level and the XP are held at the lower tier's cap.
    Change demote(int64_t profession);
    /// Raises the XP total to at least `xp` (held at what the tier holds): the settlement of a scroll's level.
    Change raise_xp_to(int64_t profession, int64_t xp);
    /// Puts a profession back to `state` (held to the rules of its tier): a plan keeps the XP only of the professions it moved. Nothing for an unknown profession.
    void   set_state(int64_t profession, const State& state);
// C-Y-B and CoA

    /// The levels by profession name, what GetPlayerProfessionLevel reads.
    std::map<std::string, int> levels_by_name() const;
    /// All 22 as the character service stores them, in guid order (empty for a set with no book).
    std::vector<svc::ProfessionState> to_svc() const;
    /// The changes between two sets of the same book, in profession order (only the professions that differ).
    static std::vector<Change> diff(const Professions& before, const Professions& after);

    bool operator==(const Professions& o) const { return book_ == o.book_ && states_ == o.states_; }

private:
    State& at(int index) { return states_[static_cast<size_t>(index)]; }
    /// Re-derives the level of a profession from its XP and tier, holds the XP at what the tier holds.
    void settle(int index);

    const Book*        book_ = nullptr;
    std::vector<State> states_;   // index = the book's order
};

/// The profession a Profession reward's expression is for: the name in `GetPlayerProfessionLevel(GetPlayer(), "Mining")`; empty when the text has none.
std::string profession_named_in(std::string_view expression);

/// Runs the compiled expression `expr` (an ab_expr id) of a Profession reward with the professions at `levels` (GetPlayerProfessionLevel); nullopt with `error`
/// set when it cannot run (no program, a fault).
using XpRunner = std::function<std::optional<double>(int64_t expr, const std::map<std::string, int>& levels, std::string& error)>;

/// An XpRunner over the ability catalog's programs and one character's stats (worn items as the stat engine takes them). The runner keeps its evaluation
/// alive; `catalog` and `stats` need only be valid during this call, the bundle they point at must outlive the runner. Empty function, with `error`, when
/// the stat engine refuses.
XpRunner make_xp_runner(const abilities::Catalog& catalog, const stats::CharacterStats& stats, std::span<const stats::WornItem> worn, std::string& error);

}  // namespace aoc::worldd::professions
