// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/weapon_lines.h
//
//  The character's weapon lines (weapon XP, part W0) as pure logic over content.db: the 21 weapon lines (the leveling types named Weapon_<line>), all on one 25-level
//  curve, and what a character holds in them: one experience total per line.
//
//  What the client does with it, and so what the server keeps and sends:
//    - the client works out a line's level, its bar and its talent points from ONE number per line, the line's experience total in the XPTracker's ExperienceFastMap
//      (the key is the line's leveling type; a line with no entry has no XP and is at level 1).  The server never sends the points: the client adds 1 Weapon point for each
//      level of the line from level 2 (24 at level 25) and takes away what the bought ranks cost, so the server's own purchase check has to count the same points;
//    - a line shows up in a join's ExperienceFastMap once it has XP, as an absolute total.
//
//  Held here:
//    - the level is the highest level of the curve whose XP total the line's XP reaches; the XP is held at the curve's last total (level 25), so a maxed line stays there;
//    - the 21 lines share one curve (content.db is refused if one of them has a curve of its own);
//    - a weapon line carries no experience debt (only Adventuring has one);
//    - a kill pays the kill's Weapon experience to every weapon line of what is worn, each line once however many hands hold it (two daggers are one line), and to no
//      other; the amount is the kill's own, not cut by a line's level and not held back by the Adventuring debt.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "session/leveling.h"
#include "svc/services.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// ~ C.O.A & Cyb ~

namespace aoc::worldd::weaponxp {

/// One weapon line: its leveling type (the XPTracker's key) and its name ("Weapon_1H_Axe": also the name of the line's weapon tree and the line name the weapon items carry).
struct Line {
    int64_t     leveling_type = 0;
    std::string name;
};

/// The weapon lines of content.db with the one curve they share.
class Lines {
public:
    /// From content.db: every leveling type of the Weapon experience type whose name starts with "Weapon_" (the type named just "Weapon" belongs to no line).  nullopt with `error`
    /// empty for a file with none; nullopt with `error` set for one that has them damaged (a curve that is no level curve, or lines on different curves).
    static std::optional<Lines> from(const content::ContentDb& db, std::string& error) {
        error.clear();
        std::string why;
        const std::vector<std::string> names = db.leveling_type_names("Weapon", "Weapon_", why);
        if (!why.empty()) {
            error = why;
            return std::nullopt;
        }
        if (names.empty()) return std::nullopt;
        Lines out;
        std::vector<std::pair<int, int64_t>> shared;
        for (const std::string& n : names) {
            const std::optional<content::LevelingType> type = db.leveling_type_named(n, why);
            if (!type) {
                error = "the weapon line " + n + ": " + why;
                return std::nullopt;
            }
            if (out.lines_.empty()) {
                std::optional<leveling::LevelCurve> curve = leveling::LevelCurve::from(*type, why);
                if (!curve) {
                    error = "the weapon line " + n + ": " + why;
                    return std::nullopt;
                }
                out.curve_ = std::move(*curve);
                shared = type->curve;
            } else if (type->curve != shared) {
                error = "the weapon line " + n + " has a level curve of its own (the lines share one)";
                return std::nullopt;
            }
            out.lines_.push_back({type->guid, type->name});
        }
        return out;
    }

    /// The lines in guid order: the order the join lists them in.
    const std::vector<Line>& all() const { return lines_; }
    size_t                   size() const { return lines_.size(); }
    const Line*              find(int64_t leveling_type) const {
        for (const Line& l : lines_) {
            if (l.leveling_type == leveling_type) return &l;
        }
        return nullptr;
    }
    /// The line whose whole name is `name` ("Weapon_Ranged_Wand": the line name a weapon item carries); case does not count.  Null for any other text.
    const Line* find_named(std::string_view name) const {
        const std::string t = lowered(name);
        for (const Line& l : lines_) {
            if (lowered(l.name) == t) return &l;
        }
        return nullptr;
    }
    /// The lines a name can mean (case, spaces and hyphens do not count): a name or its short form without "Weapon_" ("Ranged_Wand") names one line; a word that ends a line's short
    /// name ("wand", "longbow", "2H_Axe") names it when it names no other.  More than one line comes back for a word that fits several ("axe": 1H_Axe and 2H_Axe).
    std::vector<const Line*> match(std::string_view text) const {
        std::string t = lowered(text);
        for (char& c : t) {
            if (c == ' ' || c == '-') c = '_';
        }
        if (t.rfind("weapon_", 0) == 0) t.erase(0, 7);
        std::vector<const Line*> exact, ending;
        if (t.empty()) return exact;
        for (const Line& l : lines_) {
            std::string s = lowered(l.name);
            if (s.rfind("weapon_", 0) == 0) s.erase(0, 7);
            if (s == t) exact.push_back(&l);
            else if (s.size() > t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0 && s[s.size() - t.size() - 1] == '_') ending.push_back(&l);
        }
        return exact.empty() ? ending : exact;
    }

    const leveling::LevelCurve& curve() const { return curve_; }
    int                         max_level() const { return curve_.cap(); }
    int                         level_of(int64_t xp) const { return curve_.level_of(xp); }
    /// The XP total at which `level` is reached.
    int64_t                     xp_for(int level) const { return curve_.xp_for(level); }
    /// The most XP a line holds: the last total of the curve.
    int64_t                     max_xp() const { return curve_.max_xp(); }

private:
    static std::string lowered(std::string_view s) {
        std::string out(s);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    std::vector<Line>    lines_;
    leveling::LevelCurve curve_;
};

/// What a line did when its total changed.
struct Change {
    int64_t line = 0;   // the leveling type
    int64_t xp_before = 0, xp_after = 0;
    int     level_before = 1, level_after = 1;
    bool leveled() const { return level_after > level_before; }
    bool changed() const { return xp_before != xp_after; }
};

/// What a character holds in the weapon lines: an XP total for each line that has a row.  A default-constructed set has no lines: every answer is "no XP, level 1" and every change is refused.
class State {
public:
    State() = default;
    explicit State(const Lines& lines) : lines_(&lines) {}

    /// From the character record's experience rows: the rows of the lines are taken (a total past what a line holds is held at it; `notes` says so), the rows of the other leveling types are not
    /// this set's.
    static State load(const Lines& lines, std::span<const svc::Experience> rows, std::vector<std::string>* notes = nullptr) {
        State s(lines);
        for (const svc::Experience& r : rows) {
            const Line* line = lines.find(r.leveling_type_guid);
            if (!line) continue;
            int64_t xp = r.xp < 0 ? 0 : r.xp;
            if (xp > lines.max_xp()) {
                if (notes) notes->push_back("the record's " + line->name + " XP " + std::to_string(xp) + " is past what a line holds: " + std::to_string(lines.max_xp()) + " is used");
                xp = lines.max_xp();
            }
            s.xp_[line->leveling_type] = xp;
        }
        return s;
    }

    bool         valid() const { return lines_ != nullptr; }
    const Lines* lines() const { return lines_; }

    /// A line's total (0 for a line with none, and for a type that is no line).
    int64_t xp(int64_t leveling_type) const {
        const auto it = xp_.find(leveling_type);
        return it == xp_.end() ? 0 : it->second;
    }
    /// A line's level (1 with no XP).
    int level(int64_t leveling_type) const { return lines_ ? lines_->level_of(xp(leveling_type)) : 1; }

    /// Sets a line's total (held at the most a line holds).  The change comes back with line 0 for a type that is no line, or for a set with no lines (nothing changed).
    Change set_xp(int64_t leveling_type, int64_t xp) {
        Change c;
        if (!lines_ || !lines_->find(leveling_type) || xp < 0) return c;
        c.line = leveling_type;
        c.xp_before = this->xp(leveling_type);
        c.level_before = level(leveling_type);
        xp_[leveling_type] = std::min(xp, lines_->max_xp());
        c.xp_after = xp_[leveling_type];
        c.level_after = level(leveling_type);
        return c;
    }
    /// Adds XP (0 or less adds nothing); the total is held at the most a line holds.
    Change add_xp(int64_t leveling_type, int64_t delta) {
        const int64_t now = xp(leveling_type);
        const int64_t room = lines_ ? lines_->max_xp() - now : 0;
        return set_xp(leveling_type, delta <= 0 ? now : (delta > room ? lines_->max_xp() : now + delta));
    }

    /// Every line's level, by leveling type (1 for a line with no XP): what the talents book judges a weapon tree at.
    std::map<int64_t, int> levels() const {
        std::map<int64_t, int> out;
        if (!lines_) return out;
        for (const Line& l : lines_->all()) out[l.leveling_type] = level(l.leveling_type);
        return out;
    }
    /// The lines that have XP, in the lines' order: what the XPTracker's ExperienceFastMap lists at the join.
    std::vector<std::pair<int64_t, int64_t>> with_xp() const {
        std::vector<std::pair<int64_t, int64_t>> out;
        if (!lines_) return out;
        for (const Line& l : lines_->all()) {
            if (xp(l.leveling_type) > 0) out.emplace_back(l.leveling_type, xp(l.leveling_type));
        }
        return out;
    }
    /// The rows the character service stores (no debt): a line with XP, and a line that was set back to none while this set was held (its row is written over with 0).
    std::vector<svc::Experience> to_svc() const {
        std::vector<svc::Experience> out;
        if (!lines_) return out;
        for (const Line& l : lines_->all()) {
            const auto it = xp_.find(l.leveling_type);
            if (it != xp_.end()) out.push_back({l.leveling_type, it->second, 0});
        }
        return out;
    }

    bool operator==(const State& o) const { return lines_ == o.lines_ && xp_ == o.xp_; }

private:
    const Lines*              lines_ = nullptr;
    std::map<int64_t, int64_t> xp_;   // by leveling type; a line with no entry has no XP
};

}  // namespace aoc::worldd::weaponxp
