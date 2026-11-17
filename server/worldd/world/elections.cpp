// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "world/elections.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <set>

namespace aoc::worldd::world {

namespace {

constexpr int64_t kDayMs = 86400000;
constexpr size_t kMaxMessage = 512;
constexpr int64_t kHourMs = 3600000;

// The civil calendar (proleptic Gregorian, UTC): days since 1970-01-01 of a date, and back.
int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int64_t& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = yoe + era * 400 + (m <= 2);
}

int days_in_month(int64_t y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : kDays[m - 1];
}

int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

nlohmann::json person_json(const ElectionPerson& p) { return {{"account", p.account}, {"character", p.character_id}, {"name", p.name}, {"at", p.at_ms}}; }

ElectionPerson person_of(const nlohmann::json& j) {
    ElectionPerson p;
    p.account = j.value("account", int64_t{0});
    p.character_id = j.value("character", std::string());
    p.name = j.value("name", std::string());
    p.at_ms = j.value("at", int64_t{0});
    return p;
}

}  // namespace

const char* election_state_name(ElectionState s) {
    switch (s) {
        case ElectionState::AwaitingFirst: return "AwaitingFirstElection";
        case ElectionState::Registration: return "RegistrationPeriod";
        case ElectionState::Active: return "ActiveElection";
        case ElectionState::AwaitingResults: return "AwaitingElectionResults";
        case ElectionState::Post: return "PostElection";
    }
    return "?";
}

int64_t ElectionRules::election_start_ms(int64_t not_before_ms) const {
    int64_t y = 0;
    int m = 0, d = 0;
    civil_from_days(floor_div(not_before_ms, kDayMs), y, m, d);
    const int64_t registration = static_cast<int64_t>(std::max(0, registration_days)) * kDayMs;
    for (int i = 0; i < 26; ++i) {   // (the first month whose registration has not begun is at most two months on)
        const int day = std::clamp(start_day, 1, days_in_month(y, m));
        const int64_t start = days_from_civil(y, m, day) * kDayMs + static_cast<int64_t>(std::clamp(start_hour, 0, 23)) * kHourMs;
        if (start - registration >= not_before_ms) return start;
        if (++m > 12) {
            m = 1;
            ++y;
        }
    }
    return not_before_ms + registration;
}

int64_t ElectionRules::election_end_ms(int64_t start_ms) const {
    const int64_t day0 = floor_div(start_ms, kDayMs);
    // through the end of the end hour: hour 23 of the last day ends at 00:00 of the day after
    const int64_t end = (day0 + std::max(1, election_days) - 1) * kDayMs + static_cast<int64_t>(std::clamp(end_hour, 0, 23) + 1) * kHourMs;
    return std::max(end, start_ms + kHourMs);   // (an end hour before the start hour on a one-day election: an hour)
}

std::optional<ElectionRules> ElectionRules::load(const std::string& path, std::string& error) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = "cannot open " + path;
        if (db) sqlite3_close(db);
        return std::nullopt;
    }
    sqlite3_stmt* st = nullptr;
    std::optional<ElectionRules> out;
    if (sqlite3_prepare_v2(db,
                           "SELECT election_type, registration_days, election_days, first_registration_secs, first_wait_for_results_secs, first_election_secs, start_day, "
                           "start_hour, end_hour, min_term_days FROM election_def "
                           "WHERE name NOT LIKE 'TEST%' ORDER BY guid LIMIT 1",
                           -1, &st, nullptr) != SQLITE_OK) {
        error = path + " has no election_def table";
    } else if (sqlite3_step(st) == SQLITE_ROW) {
        ElectionRules r;
        const unsigned char* type = sqlite3_column_text(st, 0);
        const std::string t = type ? reinterpret_cast<const char*>(type) : "";
        r.election_type = t == "Divine" ? 1 : t == "Military" ? 2 : t == "Economic" ? 3 : 0;
        if (const int d = sqlite3_column_int(st, 1); d > 0 && d <= 60) r.registration_days = d;
        if (const int d = sqlite3_column_int(st, 2); d > 0 && d <= 60) r.election_days = d;
        if (const int64_t s = sqlite3_column_int64(st, 3); s > 0 && s <= 60LL * 86400) r.first_registration_secs = s;
        if (const int64_t s = sqlite3_column_int64(st, 4); s > 0 && s <= 86400) r.results_secs = s;
        if (const int64_t s = sqlite3_column_int64(st, 5); s > 0 && s <= 60LL * 86400) r.first_election_secs = s;
        if (const int d = sqlite3_column_int(st, 6); d >= 1 && d <= 31) r.start_day = d;
        if (const int h = sqlite3_column_int(st, 7); h >= 0 && h <= 23) r.start_hour = h;
        if (const int h = sqlite3_column_int(st, 8); h >= 0 && h <= 23) r.end_hour = h;
        if (const int d = sqlite3_column_int(st, 9); d >= 0 && d <= 365) r.min_term_days = d;
        out = r;
    } else {
        error = path + "'s election_def has no definition";
    }
    if (st) sqlite3_finalize(st);
    st = nullptr;
    // CityNodeRecordBase's MayorMinimumCitizens (contentc's city_node.mayor_min_citizens; a file without the column: 1 for every node)
    if (out && sqlite3_prepare_v2(db, "SELECT guid, mayor_min_citizens FROM city_node", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const int n = sqlite3_column_int(st, 1);
            if (n > 0 && n <= 100000) out->min_citizens[sqlite3_column_int64(st, 0)] = n;
        }
    }
    if (st) sqlite3_finalize(st);
    st = nullptr;
    // ElectionConstants' RankedChoiceVoteCountLimit (contentc's node_constant; a file without it: 3)
    if (out && sqlite3_prepare_v2(db, "SELECT value FROM node_constant WHERE name = 'RankedChoiceVoteCountLimit'", -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        if (const int n = sqlite3_column_int(st, 0); n >= 1 && n <= 32) out->ranked_limit = static_cast<size_t>(n);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

void Elections::enter(int64_t node, NodeElection& e, ElectionState s, int64_t start_ms, std::vector<Change>& out, bool count) {
    Change c;
    c.node = node;
    c.state = s;
    e.state = s;
    e.phase_started_ms = start_ms;
    switch (s) {
        case ElectionState::AwaitingFirst: e.phase_ends_ms = start_ms; break;   // (due at once: it waits for the node's first citizen)
        case ElectionState::Registration:
            e.phase_ends_ms = e.first ? start_ms + rules_.first_registration_secs * 1000 : rules_.election_start_ms(start_ms);
            e.candidates.clear();
            e.votes.clear();
            e.voters.clear();
            break;
        case ElectionState::Active: e.phase_ends_ms = e.first ? start_ms + rules_.first_election_secs * 1000 : rules_.election_end_ms(start_ms); break;
        case ElectionState::AwaitingResults: e.phase_ends_ms = e.first ? start_ms + rules_.results_secs * 1000 : rules_.results_ms(start_ms); break;
        case ElectionState::Post: {
            // the term: until the next month's registration, at least min_term_days on
            const int64_t term_from = start_ms + static_cast<int64_t>(std::max(0, rules_.min_term_days)) * kDayMs;
            e.phase_ends_ms = rules_.election_start_ms(term_from) - static_cast<int64_t>(std::max(0, rules_.registration_days)) * kDayMs;
            e.first = false;
            if (!count) break;   // no candidate stood: no vote this month, the mayor stays
            c.results = true;
            if (const auto w = tally(e)) {
                const int64_t before = e.mayor.account;
                e.mayor = e.candidates[*w];
                e.mayor.at_ms = start_ms;
                if (e.mayor.account != before) e.mayor_message.clear();   // a new mayor starts with no message
                c.winner = e.mayor.name;
            } else {
                e.mayor = ElectionPerson{};
                e.mayor_message.clear();
            }
            break;
        }
    }
    ++version_;
    out.push_back(std::move(c));
}

std::vector<Elections::Change> Elections::advance(int64_t now_ms, const std::function<bool(int64_t)>& takes_citizens, const std::vector<int64_t>& nodes) {
    std::vector<Change> out;
    for (const int64_t node : nodes) {
        // a node whose cycle has not started (none yet, or only its citizens kept): it starts when the node takes citizens
        const auto it = nodes_.find(node);
        if ((it == nodes_.end() || it->second.phase_ends_ms == 0) && takes_citizens && takes_citizens(node)) enter(node, nodes_[node], ElectionState::AwaitingFirst, now_ms, out);
    }
    for (auto& [node, e] : nodes_) {
        for (int guard = 0; guard < 64 && e.phase_ends_ms != 0 && now_ms >= e.phase_ends_ms; ++guard) {
            const int64_t at = e.phase_ends_ms;
            bool waits = false;
            switch (e.state) {
                case ElectionState::AwaitingFirst:
                    // the first election starts once the node has the citizens it needs to elect a mayor (MayorMinimumCitizens)
                    if (e.citizens.empty() || e.citizens.size() < static_cast<size_t>(rules_.citizens_for_election(node))) {
                        waits = true;
                        break;
                    }
                    e.first = true;
                    enter(node, e, ElectionState::Registration, std::max(at, now_ms), out);
                    break;
                case ElectionState::Registration:
                    if (e.citizens.size() < static_cast<size_t>(rules_.citizens_for_election(node))) {
                        // too few citizens to elect a mayor: no vote; the first waits for them again, a monthly one keeps the mayor
                        if (e.first) enter(node, e, ElectionState::AwaitingFirst, at, out);
                        else enter(node, e, ElectionState::Post, at, out, /*count=*/false);
                        if (e.state == ElectionState::AwaitingFirst) waits = true;
                        break;
                    }
                    if (!e.candidates.empty()) enter(node, e, ElectionState::Active, at, out);
                    else if (e.first) enter(node, e, ElectionState::Registration, at, out);   // the first: the registration runs again
                    else enter(node, e, ElectionState::Post, at, out, /*count=*/false);        // a month with no candidate: no vote
                    break;
                case ElectionState::Active: enter(node, e, ElectionState::AwaitingResults, at, out); break;
                case ElectionState::AwaitingResults: enter(node, e, ElectionState::Post, at, out); break;
                case ElectionState::Post: enter(node, e, ElectionState::Registration, at, out); break;
            }
            if (waits) break;
        }
    }
    return out;
}

std::optional<Elections::Change> Elections::skip(int64_t node, int64_t now_ms) {
    auto it = nodes_.find(node);
    if (it == nodes_.end()) return std::nullopt;
    NodeElection& e = it->second;
    std::vector<Change> out;
    // a game master's: the next phase whatever the citizens (the minimum for an election only holds the cycle's own steps)
    switch (e.state) {
        case ElectionState::AwaitingFirst:
            e.first = true;
            enter(node, e, ElectionState::Registration, now_ms, out);
            break;
        case ElectionState::Registration:
            if (!e.candidates.empty()) enter(node, e, ElectionState::Active, now_ms, out);
            else if (e.first) enter(node, e, ElectionState::Registration, now_ms, out);
            else enter(node, e, ElectionState::Post, now_ms, out, /*count=*/false);
            break;
        case ElectionState::Active: enter(node, e, ElectionState::AwaitingResults, now_ms, out); break;
        case ElectionState::AwaitingResults: enter(node, e, ElectionState::Post, now_ms, out); break;
        case ElectionState::Post: enter(node, e, ElectionState::Registration, now_ms, out); break;
    }
    if (out.empty()) return std::nullopt;
    return out.back();
}

Elections::Change Elections::set_state(int64_t node, ElectionState s, int64_t now_ms, int64_t duration_secs) {
    std::vector<Change> out;
    NodeElection& e = nodes_[node];
    enter(node, e, s, now_ms, out);
    if (duration_secs > 0) e.phase_ends_ms = now_ms + duration_secs * 1000;
    return out.back();
}

const NodeElection* Elections::node(int64_t guid) const {
    const auto it = nodes_.find(guid);
    return it == nodes_.end() ? nullptr : &it->second;
}

void Elections::remove_citizen(int64_t account) {
    if (account == 0) return;
    for (auto& [node, e] : nodes_) {
        bool changed = e.citizens.erase(account) > 0;
        const auto before = e.candidates.size();
        e.candidates.erase(std::remove_if(e.candidates.begin(), e.candidates.end(), [&](const ElectionPerson& c) { return c.account == account; }), e.candidates.end());
        changed = changed || e.candidates.size() != before;
        changed = e.votes.erase(account) > 0 || changed;
        changed = e.voters.erase(account) > 0 || changed;
        if (e.mayor.account == account) {
            e.mayor = ElectionPerson{};
            e.mayor_message.clear();
            changed = true;
        }
        if (changed) ++version_;
    }
}

void Elections::set_citizen(int64_t node, const ElectionPerson& who) {
    if (who.account == 0 || node == 0) return;
    for (auto& [other, e] : nodes_) {
        if (other != node && e.citizens.count(who.account)) remove_citizen(who.account);
    }
    NodeElection& e = nodes_[node];   // (a node with no election yet keeps its citizens until its first one starts)
    if (e.phase_ends_ms == 0 && e.citizens.empty() && e.candidates.empty()) e.state = ElectionState::AwaitingFirst;
    const auto it = e.citizens.find(who.account);
    if (it != e.citizens.end() && it->second.character_id == who.character_id && it->second.name == who.name) return;
    e.citizens[who.account] = who;
    ++version_;
}

bool Elections::register_candidate(int64_t node, const ElectionPerson& who, int64_t now_ms, std::string& why) {
    auto it = nodes_.find(node);
    if (it == nodes_.end()) { why = "the node holds no election"; return false; }
    NodeElection& e = it->second;
    if (e.state != ElectionState::Registration) { why = std::string("the node's election is in ") + election_state_name(e.state) + ", not its registration period"; return false; }
    if (e.citizens.count(who.account) == 0) { why = "the account is not a citizen of the node"; return false; }
    if (is_candidate(node, who.account)) { why = "the account stands already"; return false; }
    ElectionPerson c = who;
    c.at_ms = now_ms;
    e.candidates.push_back(std::move(c));
    ++version_;
    return true;
}

bool Elections::vote(int64_t node, const ElectionPerson& who, const std::vector<std::string>& ranked, int64_t now_ms, std::string& why) {
    return vote_as(node, who, ranked, now_ms, why, /*citizens_only=*/true);
}

bool Elections::gm_vote(int64_t node, const ElectionPerson& who, const std::vector<std::string>& ranked, int64_t now_ms, std::string& why) {
    return vote_as(node, who, ranked, now_ms, why, /*citizens_only=*/false);
}

bool Elections::vote_as(int64_t node, const ElectionPerson& who, const std::vector<std::string>& ranked, int64_t now_ms, std::string& why, bool citizens_only) {
    auto it = nodes_.find(node);
    if (it == nodes_.end()) { why = "the node holds no election"; return false; }
    NodeElection& e = it->second;
    if (e.state != ElectionState::Active) { why = std::string("the node's election is in ") + election_state_name(e.state) + ", not voting"; return false; }
    if (who.account == 0) { why = "no account"; return false; }
    if (citizens_only && e.citizens.count(who.account) == 0) { why = "the account is not a citizen of the node"; return false; }
    std::vector<std::string> kept;
    std::set<std::string> seen;
    for (const std::string& id : ranked) {
        if (kept.size() >= rules_.ranked_limit || !seen.insert(id).second) continue;
        const bool known = std::any_of(e.candidates.begin(), e.candidates.end(), [&](const ElectionPerson& c) { return c.character_id == id; });
        if (known) kept.push_back(id);
    }
    if (kept.empty()) { why = "it names no candidate of the node"; return false; }
    e.votes[who.account] = std::move(kept);
    ElectionPerson v = who;
    v.at_ms = now_ms;
    e.voters[who.account] = std::move(v);
    ++version_;
    return true;
}

bool Elections::set_message(int64_t node, int64_t account, const std::string& message, std::string& why) {
    auto it = nodes_.find(node);
    if (it == nodes_.end() || it->second.mayor.account == 0 || it->second.mayor.account != account) { why = "the account is not the node's mayor"; return false; }
    if (message.size() > kMaxMessage) { why = "the message is longer than 512 bytes"; return false; }
    it->second.mayor_message = message;
    ++version_;
    return true;
}

void Elections::set_mayor(int64_t node, const ElectionPerson& who, int64_t now_ms) {
    NodeElection& e = nodes_[node];
    e.mayor = who;
    e.mayor.at_ms = now_ms;
    if (who.account == 0) e.mayor_message.clear();
    ++version_;
}

int Elections::citizen_rank(int64_t node, int64_t account) const {
    const NodeElection* e = this->node(node);
    if (!e) return 0;
    const auto me = e->citizens.find(account);
    if (me == e->citizens.end()) return 0;
    int rank = 1;
    for (const auto& [a, p] : e->citizens) {
        if (a != account && (p.at_ms < me->second.at_ms || (p.at_ms == me->second.at_ms && a < account))) ++rank;
    }
    return rank;
}

size_t Elections::citizen_count(int64_t node) const {
    const NodeElection* e = this->node(node);
    return e ? e->citizens.size() : 0;
}

bool Elections::gm_set_candidate(int64_t node, const ElectionPerson& who, bool adding, int64_t now_ms, std::string& why) {
    auto it = nodes_.find(node);
    if (it == nodes_.end()) { why = "the node holds no election"; return false; }
    NodeElection& e = it->second;
    auto& c = e.candidates;
    const auto at = std::find_if(c.begin(), c.end(), [&](const ElectionPerson& p) { return p.account == who.account; });
    if (!adding) {
        if (at == c.end()) { why = "the account does not stand"; return false; }
        c.erase(at);
        ++version_;
        return true;
    }
    if (e.citizens.count(who.account) == 0) { why = "the account is not a citizen of the node"; return false; }
    if (at != c.end()) { why = "the account stands already"; return false; }
    ElectionPerson p = who;
    p.at_ms = now_ms;
    c.push_back(std::move(p));
    ++version_;
    return true;
}

bool Elections::gm_set_message(int64_t node, const std::string& message, std::string& why) {
    auto it = nodes_.find(node);
    if (it == nodes_.end() || it->second.mayor.account == 0) { why = "the node has no mayor"; return false; }
    if (message.size() > kMaxMessage) { why = "the message is longer than 512 bytes"; return false; }
    it->second.mayor_message = message;
    ++version_;
    return true;
}

namespace {
bool same_text(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}
}  // namespace

const ElectionPerson* Elections::citizen_named(int64_t node, const std::string& name) const {
    const NodeElection* e = this->node(node);
    if (!e || name.empty()) return nullptr;
    for (const auto& [a, p] : e->citizens) {
        if (same_text(p.name, name)) return &p;
    }
    return nullptr;
}

const ElectionPerson* Elections::candidate_named(int64_t node, const std::string& name) const {
    const NodeElection* e = this->node(node);
    if (!e || name.empty()) return nullptr;
    for (const ElectionPerson& p : e->candidates) {
        if (same_text(p.name, name)) return &p;
    }
    return nullptr;
}

void Elections::set_tax(int64_t node, const std::string& tag, TaxOverride rate) {
    if (node == 0 || tag.empty()) return;
    NodeElection& e = nodes_[node];
    const auto it = e.taxes.find(tag);
    if (it != e.taxes.end() && it->second == rate) return;
    e.taxes[tag] = rate;
    ++version_;
}

const std::map<std::string, TaxOverride>* Elections::taxes(int64_t node) const {
    const NodeElection* e = this->node(node);
    return e && !e->taxes.empty() ? &e->taxes : nullptr;
}

const std::map<int64_t, int32_t>* Elections::stored(int64_t node) const {
    const auto it = nodes_.find(node);
    return it == nodes_.end() ? nullptr : &it->second.stored;
}

const NodeBuyOrders* Elections::buy_orders(int64_t node) const {
    const auto it = nodes_.find(node);
    return it == nodes_.end() ? nullptr : &it->second.buy_orders;
}

bool Elections::change_node(int64_t node, const std::function<bool(NodeElection&)>& f) {
    if (node == 0) return false;
    NodeElection& e = nodes_[node];
    const bool changed = f(e);
    e.treasury = std::max<int64_t>(0, e.treasury);
    for (auto it = e.stored.begin(); it != e.stored.end();) it = it->second <= 0 ? e.stored.erase(it) : std::next(it);
    if (changed) ++version_;
    return changed;
}

bool Elections::stock(int64_t node, int64_t money, const std::vector<std::pair<int64_t, int32_t>>& items) {
    if (node == 0) return false;
    NodeElection& e = nodes_[node];
    if (e.stocked) return false;
    e.stocked = true;
    e.treasury = std::max<int64_t>(0, e.treasury + money);
    for (const auto& [item, quantity] : items) {
        if (item == 0 || quantity <= 0) continue;
        const int64_t sum = static_cast<int64_t>(e.stored[item]) + quantity;
        e.stored[item] = static_cast<int32_t>(std::min<int64_t>(sum, INT32_MAX));
    }
    ++version_;
    return true;
}

void Elections::add_treasury(int64_t node, int64_t copper) {
    if (node == 0 || copper == 0) return;
    NodeElection& e = nodes_[node];
    e.treasury = std::max<int64_t>(0, e.treasury + copper);
    ++version_;
}

int64_t Elections::dues_anchor(int64_t node) const {
    const auto it = nodes_.find(node);
    return it == nodes_.end() ? 0 : it->second.dues_anchor_ms;
}

void Elections::set_dues_anchor(int64_t node, int64_t at_ms) {
    if (node == 0 || at_ms <= 0) return;
    NodeElection& e = nodes_[node];
    if (e.dues_anchor_ms != 0) return;   // (the schedule is the node's: set once)
    e.dues_anchor_ms = at_ms;
    ++version_;
}

void Elections::add_news(int64_t node, NodeNews n) {
    if (node == 0) return;
    auto& feed = nodes_[node].news;
    feed.push_back(std::move(n));
    if (feed.size() > kMaxNews) feed.erase(feed.begin(), feed.begin() + static_cast<std::ptrdiff_t>(feed.size() - kMaxNews));
    ++version_;
}

int64_t Elections::treasury(int64_t node) const {
    const NodeElection* e = this->node(node);
    return e ? e->treasury : 0;
}

void Elections::set_dues_disabled(int64_t node, bool off) {
    if (node == 0) return;
    NodeElection& e = nodes_[node];
    if (e.dues_disabled == off) return;
    e.dues_disabled = off;
    ++version_;
}

bool Elections::dues_disabled(int64_t node) const {
    const NodeElection* e = this->node(node);
    return e && e->dues_disabled;
}

bool Elections::is_candidate(int64_t node, int64_t account) const {
    const NodeElection* e = this->node(node);
    return e && std::any_of(e->candidates.begin(), e->candidates.end(), [&](const ElectionPerson& c) { return c.account == account; });
}

int64_t Elections::mayor_node(int64_t account) const {
    if (account == 0) return 0;
    for (const auto& [node, e] : nodes_) {
        if (e.mayor.account == account) return node;
    }
    return 0;
}

std::optional<size_t> Elections::tally(const NodeElection& e) {
    if (e.candidates.empty()) return std::nullopt;
    std::set<std::string> out;   // candidates out of the count
    for (size_t round = 0; round < e.candidates.size(); ++round) {
        std::set<std::string> standing_ids;
        for (const ElectionPerson& c : e.candidates) {
            if (!out.count(c.character_id)) standing_ids.insert(c.character_id);
        }
        // each ballot counts for its first choice still standing: a choice who withdrew (no candidate now) or was counted out passes it on
        std::map<std::string, size_t> firsts;
        size_t total = 0;
        for (const auto& [account, ranked] : e.votes) {
            for (const std::string& id : ranked) {
                if (!standing_ids.count(id)) continue;
                ++firsts[id];
                ++total;
                break;
            }
        }
        // a majority of the ballots still counting wins; else the weakest goes (ties: the later registered goes first)
        std::optional<size_t> best, weakest;
        size_t best_n = 0, weakest_n = SIZE_MAX, standing = 0;
        for (size_t i = 0; i < e.candidates.size(); ++i) {
            const std::string& id = e.candidates[i].character_id;
            if (out.count(id)) continue;
            ++standing;
            const size_t n = firsts[id];
            if (!best || n > best_n) { best = i; best_n = n; }
            if (!weakest || n <= weakest_n) { weakest = i; weakest_n = n; }
        }
        if (!best) return std::nullopt;
        if (standing == 1 || total == 0 || best_n * 2 > total) return best;
        out.insert(e.candidates[*weakest].character_id);
    }
    return std::nullopt;
}

std::string Elections::to_json() const {
    nlohmann::json j;
    j["version"] = 1;
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& [node, e] : nodes_) {
        nlohmann::json n;
        n["node"] = node;
        n["state"] = static_cast<int>(e.state);
        n["started"] = e.phase_started_ms;
        n["ends"] = e.phase_ends_ms;
        n["mayor"] = person_json(e.mayor);
        n["message"] = e.mayor_message;
        n["first"] = e.first;
        n["dues_off"] = e.dues_disabled;
        n["treasury"] = e.treasury;
        if (e.dues_anchor_ms != 0) n["dues_anchor"] = e.dues_anchor_ms;
        if (e.stocked) n["stocked"] = true;
        if (!e.stored.empty()) {
            nlohmann::json st = nlohmann::json::array();
            for (const auto& [item, quantity] : e.stored) st.push_back({item, quantity});
            n["stored"] = std::move(st);
        }
        if (!(e.buy_orders == NodeBuyOrders{})) {
            nlohmann::json b;
            b["serial"] = e.buy_orders.serial;
            b["active"] = nlohmann::json::array();
            for (const NodeBuyOrder& o : e.buy_orders.active) {
                nlohmann::json x = {{"guid", {o.guid.a, o.guid.b, o.guid.c, o.guid.d}}, {"commodity", o.commodity}, {"recipe", o.recipe}, {"reward", o.reward},
                                    {"created", o.created_ms}, {"expires", o.expires_ms}, {"limit", o.contribution_limit}, {"requested", o.requested},
                                    {"fulfilled", o.fulfilled}, {"reward_quantity", o.reward_quantity}, {"cost", o.node_cost}};
                x["materials"] = nlohmann::json::array();
                for (const auto& [item, quantity] : o.materials) x["materials"].push_back({item, quantity});
                x["given"] = nlohmann::json::array();
                for (const auto& [character, g] : o.given) x["given"].push_back({{"character", character}, {"units", g.units}, {"since", g.since_ms}});
                b["active"].push_back(std::move(x));
            }
            n["buy_orders"] = std::move(b);
        }
        if (!(e.buildings == NodeBuildings{})) n["buildings"] = node_buildings::to_json(e.buildings);
        for (const NodeNews& w : e.news) {
            n["news"].push_back({{"type", w.type}, {"at", w.at_ms}, {"mayor", w.mayor}, {"plot", w.plot}, {"building", w.building}, {"commission", w.commission},
                                 {"state", w.building_state}});
        }
        for (const auto& [tag, r] : e.taxes) n["taxes"].push_back({{"tag", tag}, {"percent", r.percent}, {"flat", r.flat}});
        for (const ElectionPerson& c : e.candidates) n["candidates"].push_back(person_json(c));
        for (const auto& [a, p] : e.citizens) n["citizens"].push_back(person_json(p));
        for (const auto& [a, ranked] : e.votes) {
            nlohmann::json v = person_json(e.voters.count(a) ? e.voters.at(a) : ElectionPerson{a, "", "", 0});
            v["ranked"] = ranked;
            n["votes"].push_back(std::move(v));
        }
        nodes.push_back(std::move(n));
    }
    j["nodes"] = std::move(nodes);
    return j.dump(1);
}

bool Elections::from_json(const std::string& text, std::string& error) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("nodes") || !j["nodes"].is_array()) {
        error = "not an elections file";
        return false;
    }
    std::map<int64_t, NodeElection> nodes;
    try {
        for (const nlohmann::json& n : j["nodes"]) {
            NodeElection e;
            const int s = n.value("state", 0);
            if (s < 0 || s > 4) throw std::runtime_error("a state out of range");
            e.state = static_cast<ElectionState>(s);
            e.phase_started_ms = n.value("started", int64_t{0});
            e.phase_ends_ms = n.value("ends", int64_t{0});
            if (n.contains("mayor")) e.mayor = person_of(n["mayor"]);
            e.mayor_message = n.value("message", std::string());
            e.first = n.value("first", false);
            e.dues_disabled = n.value("dues_off", false);
            e.treasury = std::max<int64_t>(0, n.value("treasury", int64_t{0}));
            e.dues_anchor_ms = std::max<int64_t>(0, n.value("dues_anchor", int64_t{0}));
            e.stocked = n.value("stocked", false);
            if (n.contains("stored") && n["stored"].is_array()) {
                for (const auto& x : n["stored"]) {
                    if (!x.is_array() || x.size() != 2 || !x[0].is_number_integer() || !x[1].is_number_integer()) continue;
                    const int64_t item = x[0].get<int64_t>(), quantity = x[1].get<int64_t>();
                    if (item != 0 && quantity > 0) e.stored[item] = static_cast<int32_t>(std::min<int64_t>(quantity, INT32_MAX));
                }
            }
            if (n.contains("buildings") && n["buildings"].is_object()) e.buildings = node_buildings::from_json(n["buildings"]);
            if (n.contains("buy_orders") && n["buy_orders"].is_object()) {
                const auto& b = n["buy_orders"];
                e.buy_orders.serial = b.value("serial", uint64_t{0});
                for (const auto& x : b.value("active", nlohmann::json::array())) {
                    NodeBuyOrder o;
                    const auto g = x.value("guid", std::vector<uint32_t>{});
                    if (g.size() == 4) o.guid = MayorGuid{g[0], g[1], g[2], g[3]};
                    o.commodity = x.value("commodity", int64_t{0});
                    o.recipe = x.value("recipe", int64_t{0});
                    o.reward = x.value("reward", int64_t{0});
                    o.created_ms = x.value("created", int64_t{0});
                    o.expires_ms = x.value("expires", int64_t{0});
                    o.contribution_limit = x.value("limit", int32_t{0});
                    o.requested = x.value("requested", int32_t{0});
                    o.fulfilled = x.value("fulfilled", int32_t{0});
                    o.reward_quantity = x.value("reward_quantity", int32_t{0});
                    o.node_cost = x.value("cost", int64_t{0});
                    for (const auto& m : x.value("materials", nlohmann::json::array())) {
                        if (m.is_array() && m.size() == 2 && m[0].is_number_integer() && m[1].is_number_integer()) {
                            o.materials.emplace_back(m[0].get<int64_t>(), static_cast<int32_t>(m[1].get<int64_t>()));
                        }
                    }
                    for (const auto& gv : x.value("given", nlohmann::json::array())) {
                        const std::string character = gv.value("character", std::string());
                        if (!character.empty()) o.given[character] = BuyOrderGiven{gv.value("units", int32_t{0}), gv.value("since", int64_t{0})};
                    }
                    if (!o.guid.zero() && o.recipe != 0 && o.requested > 0) e.buy_orders.active.push_back(std::move(o));
                }
            }
            if (n.contains("news")) {
                for (const auto& w : n["news"]) {
                    if (e.news.size() >= kMaxNews) break;
                    e.news.push_back(NodeNews{w.value("type", uint8_t{0}), w.value("at", int64_t{0}), w.value("mayor", std::string()), w.value("plot", int64_t{0}),
                                              w.value("building", int64_t{0}), w.value("commission", int64_t{0}), w.value("state", uint8_t{0})});
                }
            }
            if (n.contains("taxes")) {
                for (const auto& t : n["taxes"]) e.taxes[t.value("tag", std::string())] = TaxOverride{t.value("percent", 0.0f), t.value("flat", int32_t{0})};
                e.taxes.erase(std::string());
            }
            if (n.contains("candidates")) for (const auto& c : n["candidates"]) e.candidates.push_back(person_of(c));
            if (n.contains("citizens")) for (const auto& c : n["citizens"]) { const ElectionPerson p = person_of(c); if (p.account != 0) e.citizens[p.account] = p; }
            if (n.contains("votes")) {
                for (const auto& v : n["votes"]) {
                    const ElectionPerson p = person_of(v);
                    if (p.account == 0) continue;
                    e.votes[p.account] = v.value("ranked", std::vector<std::string>{});
                    e.voters[p.account] = p;
                }
            }
            nodes[n.value("node", int64_t{0})] = std::move(e);
        }
    } catch (const std::exception& ex) {
        error = std::string("the elections file does not read: ") + ex.what();
        return false;
    }
    nodes_ = std::move(nodes);
    ++version_;
    return true;
}

}  // namespace aoc::worldd::world
