// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/professions.cpp (see professions.h)
// ============================================================================
#include "session/professions.h"

#include "session/ability_catalog.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace aoc::worldd::professions {

namespace {

// FNames compare without regard to case (the expressions say "Lumbermilling" where the record is LumberMilling)
bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

}  // namespace

// ---- the book ----------------------------------------------------------------------------------------------------------------

std::optional<Book> Book::from(const content::ProfessionData& data, std::string& error) {
    Book b;
    if (data.certifications.size() != static_cast<size_t>(kTiers)) {
        error = "content.db has " + std::to_string(data.certifications.size()) + " certification levels, not " + std::to_string(kTiers);
        return std::nullopt;
    }
    int last_max = 0;
    for (size_t i = 0; i < data.certifications.size(); ++i) {
        const content::CertificationRow& c = data.certifications[i];
        if (c.tier != static_cast<int>(i)) {
            error = "the certification levels are not tiers 0 to 4 in order (" + c.name + " is tier " + std::to_string(c.tier) + " where tier " + std::to_string(i) + " belongs)";
            return std::nullopt;
        }
        if (c.max_level <= last_max || c.min_level > c.max_level) {
            error = "the certification level " + c.name + " spans levels " + std::to_string(c.min_level) + " to " + std::to_string(c.max_level) + ", not above the one before";
            return std::nullopt;
        }
        last_max = c.max_level;
        b.tiers_.push_back({c.guid, c.name, c.tier, c.min_level, c.max_level, c.limited, c.max_allowable});
    }
    for (const content::ProfessionRow& r : data.professions) {
        if (b.find(r.guid)) {
            error = "profession " + r.name + " (" + std::to_string(r.guid) + ") is listed twice";
            return std::nullopt;
        }
        std::string why;
        auto curve = leveling::LevelCurve::from(r.leveling, why);
        if (!curve) {
            error = "the level curve of profession " + r.name + " is not one this worldd can level with: " + why;
            return std::nullopt;
        }
        if (curve->max_level() < last_max) {
            error = "the level curve of profession " + r.name + " has " + std::to_string(curve->max_level()) + " levels, the top certification level needs " + std::to_string(last_max);
            return std::nullopt;
        }
        Profession p;
        p.guid = r.guid;
        p.name = r.name;
        p.category = r.category;
        p.leveling_type = r.leveling_type_guid;
        p.curve = std::move(*curve);
        b.professions_.push_back(std::move(p));
    }
    for (const auto& [guid, a] : data.actions) {
        if (a.profession_guid != 0 && !b.find(a.profession_guid)) {
            error = "the certification action " + a.name + " names profession " + std::to_string(a.profession_guid) + ", which is none of the " +
                    std::to_string(b.professions_.size()) + " professions";
            return std::nullopt;
        }
        b.actions_[guid] = {a.profession_guid, a.promote};
    }
    return b;
}
// (C | Y | B | C.O.A)

const Profession* Book::find(int64_t guid) const {
    for (const Profession& p : professions_) {
        if (p.guid == guid) return &p;
    }
    return nullptr;
}

const Profession* Book::named(std::string_view name) const {
    for (const Profession& p : professions_) {
        if (same_name(p.name, name)) return &p;
    }
    return nullptr;
}

int Book::index_of(int64_t guid) const {
    for (size_t i = 0; i < professions_.size(); ++i) {
        if (professions_[i].guid == guid) return static_cast<int>(i);
    }
    return -1;
}

int Book::tier_of_guid(int64_t guid) const {
    for (const Tier& t : tiers_) {
        if (t.guid == guid) return t.tier;
    }
    return -1;
}

const CertAction* Book::action(int64_t action_guid) const {
    const auto it = actions_.find(action_guid);
    return it == actions_.end() ? nullptr : &it->second;
}

int64_t Book::max_xp(const Profession& p, int tier_index) const {
    const int cap = tier(tier_index).max_level;
    if (cap < p.curve.max_level()) return p.curve.xp_for(cap + 1) - 1;
    return p.curve.xp_for(p.curve.max_level());
}

int Book::level_of(const Profession& p, int64_t xp, int tier_index) const {
    return std::min(p.curve.level_of(xp), tier(tier_index).max_level);
}

// ---- what a character holds ---------------------------------------------------------------------------------------------------

Professions::Professions(const Book& book) : book_(&book), states_(book.size()) {}

State Professions::state(int64_t profession) const {
    if (!book_) return {};
    const int i = book_->index_of(profession);
    return i < 0 ? State{} : states_[static_cast<size_t>(i)];
}

bool Professions::can_use(int64_t profession, int tier_needed, int level_needed) const {
    if (!book_ || book_->index_of(profession) < 0) return false;
    const State s = state(profession);
    return s.tier >= tier_needed && s.level >= level_needed;
}

void Professions::settle(int index) {
    State& s = at(index);
    const Profession& p = book_->all()[static_cast<size_t>(index)];
    s.tier = std::clamp(s.tier, kTierNovice, kTierGrandMaster);
    s.xp = std::clamp<int64_t>(s.xp, 0, book_->max_xp(p, s.tier));
    s.level = std::clamp(std::max(s.level, book_->level_of(p, s.xp, s.tier)), 1, book_->tier(s.tier).max_level);
}

Professions Professions::load(const Book& book, std::span<const svc::ProfessionState> rows, std::vector<std::string>* notes) {
    Professions out(book);
    std::vector<bool> seen(book.size(), false);
    const auto say = [&](std::string s) {
        if (notes) notes->push_back(std::move(s));
    };
    for (const svc::ProfessionState& r : rows) {
        const int i = book.index_of(r.profession_guid);
        if (i < 0) {
            say("a stored profession row names " + std::to_string(r.profession_guid) + ", which is none of the " + std::to_string(book.size()) + " professions: left out");
            continue;
        }
        if (seen[static_cast<size_t>(i)]) {
            say("the profession " + book.all()[static_cast<size_t>(i)].name + " is stored twice: the first row is used");
            continue;
        }
        seen[static_cast<size_t>(i)] = true;
        const Profession& p = book.all()[static_cast<size_t>(i)];
        State s;
        s.tier = std::clamp<int>(r.certification_tier, kTierNovice, kTierGrandMaster);
        if (s.tier != r.certification_tier) say("the stored tier " + std::to_string(r.certification_tier) + " of " + p.name + " is outside 0 to 4: " + std::to_string(s.tier) + " is used");
        const int cap = book.tier(s.tier).max_level;
        s.level = std::clamp<int>(r.level, 1, cap);
        if (s.level != r.level) say("the stored level " + std::to_string(r.level) + " of " + p.name + " is outside 1 to " + std::to_string(cap) + " (its tier): " + std::to_string(s.level) + " is used");
        s.xp = std::max<int64_t>(r.xp, p.curve.xp_for(s.level));   // the XP of the level at least (a level set by hand, or XP that trails it)
        if (s.xp != r.xp) say("the stored XP " + std::to_string(r.xp) + " of " + p.name + " is below level " + std::to_string(s.level) + "'s threshold: " + std::to_string(s.xp) + " is used");
        out.states_[static_cast<size_t>(i)] = s;
        out.settle(i);
    }
    return out;
}

Change Professions::add_xp(int64_t profession, int64_t delta) {
    Change c;
    const int i = book_ ? book_->index_of(profession) : -1;
    if (i < 0) return c;
    State& s = at(i);
    c.profession = profession;
    c.before = s;
    if (delta > 0) {
        const Profession& p = book_->all()[static_cast<size_t>(i)];
        const int64_t hold = book_->max_xp(p, s.tier);
        s.xp = delta > hold - s.xp ? hold : s.xp + delta;   // held at what the tier holds, no overflow
        settle(i);
    }
    c.after = s;
    return c;
}

Change Professions::promote(int64_t profession, bool needs_cap, bool* skipped) {
    Change c;
    if (skipped) *skipped = false;
    const int i = book_ ? book_->index_of(profession) : -1;
    if (i < 0) return c;
    State& s = at(i);
    c.profession = profession;
    c.before = s;
    if (s.tier < kTierGrandMaster) {
        if (needs_cap && s.level < book_->tier(s.tier).max_level) {
            if (skipped) *skipped = true;
        } else {
            ++s.tier;
            settle(i);
        }
    }
    c.after = s;
    return c;
}
// [C O A ~ C_Y_B]

Change Professions::demote(int64_t profession) {
    Change c;
    const int i = book_ ? book_->index_of(profession) : -1;
    if (i < 0) return c;
    State& s = at(i);
    c.profession = profession;
    c.before = s;
    if (s.tier > kTierNovice) {
        --s.tier;
        settle(i);
    }
    c.after = s;
    return c;
}

Change Professions::raise_xp_to(int64_t profession, int64_t xp) {
    const int64_t have = this->xp(profession);
    return add_xp(profession, xp > have ? xp - have : 0);
}

void Professions::set_state(int64_t profession, const State& state) {
    const int i = book_ ? book_->index_of(profession) : -1;
    if (i < 0) return;
    at(i) = state;
    settle(i);
}

std::map<std::string, int> Professions::levels_by_name() const {
    std::map<std::string, int> out;
    if (!book_) return out;
    for (size_t i = 0; i < book_->size(); ++i) out[book_->all()[i].name] = states_[i].level;
    return out;
}

std::vector<svc::ProfessionState> Professions::to_svc() const {
    std::vector<svc::ProfessionState> out;
    if (!book_) return out;
    out.reserve(book_->size());
    for (size_t i = 0; i < book_->size(); ++i) {
        svc::ProfessionState r;
        r.profession_guid = book_->all()[i].guid;
        r.certification_tier = states_[i].tier;
        r.level = states_[i].level;
        r.xp = states_[i].xp;
        out.push_back(r);
    }
    return out;
}

std::vector<Change> Professions::diff(const Professions& before, const Professions& after) {
    std::vector<Change> out;
    if (!before.book_ || before.book_ != after.book_) return out;
    for (size_t i = 0; i < before.states_.size(); ++i) {
        if (before.states_[i] == after.states_[i]) continue;
        Change c;
        c.profession = before.book_->all()[i].guid;
        c.before = before.states_[i];
        c.after = after.states_[i];
        out.push_back(c);
    }
    return out;
}

// ---- the expressions -----------------------------------------------------------------------------------------------------------

std::string profession_named_in(std::string_view expression) {
    static constexpr std::string_view kCall = "GetPlayerProfessionLevel";
    const size_t call = expression.find(kCall);
    if (call == std::string_view::npos) return {};
    const size_t open = expression.find('"', call);
    if (open == std::string_view::npos) return {};
    const size_t close = expression.find('"', open + 1);
    if (close == std::string_view::npos || close == open + 1) return {};
    return std::string(expression.substr(open + 1, close - open - 1));
}
// C O A.Cyb

XpRunner make_xp_runner(const abilities::Catalog& catalog, const stats::CharacterStats& stats, std::span<const stats::WornItem> worn, std::string& error) {
    std::unique_ptr<stats::CharacterStats::Evaluation> ev = stats.evaluation(worn, error);
    if (!ev) return {};
    std::shared_ptr<stats::CharacterStats::Evaluation> shared(std::move(ev));
    const abilities::Catalog* cat = &catalog;
    return [shared, cat](int64_t expr, const std::map<std::string, int>& levels, std::string& why) -> std::optional<double> {
        const int program = cat->program(expr);
        if (program < 0) {
            const content::ExprRow* row = cat->expr(expr);
            why = "expression " + std::to_string(expr) + " has no program" + (row && !row->error.empty() ? " (" + row->error + ")" : std::string());
            return std::nullopt;
        }
        shared->set_profession_levels(levels);
        stats::ExprHost host;
        double out = 0;
        if (!shared->eval(program, host, out, why)) return std::nullopt;
        if (!std::isfinite(out)) {
            why = "expression " + std::to_string(expr) + " gave " + std::to_string(out);
            return std::nullopt;
        }
        return out;
    };
}

}  // namespace aoc::worldd::professions
