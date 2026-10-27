// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "quest/quest_log.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace aoc::worldd::quest {

// ---- CityHash64, version 1.1 (Google's; the 64-bit string hash the client keys quest variables by) -----------------------------------------------------------------

namespace {

constexpr uint64_t k0 = 0xc3a5c85c97cb3127ULL;
constexpr uint64_t k1 = 0xb492b66fbe98f273ULL;
constexpr uint64_t k2 = 0x9ae16a3b2f90404fULL;

uint64_t fetch64(const char* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;   // little-endian host
}
uint32_t fetch32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
uint64_t rotate(uint64_t v, int shift) { return shift == 0 ? v : ((v >> shift) | (v << (64 - shift))); }
uint64_t shift_mix(uint64_t v) { return v ^ (v >> 47); }
uint64_t bswap64(uint64_t v) {
    v = ((v & 0x00000000ffffffffULL) << 32) | (v >> 32);
    v = ((v & 0x0000ffff0000ffffULL) << 16) | ((v >> 16) & 0x0000ffff0000ffffULL);
    return ((v & 0x00ff00ff00ff00ffULL) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffULL);
}

uint64_t hash_len16(uint64_t u, uint64_t v, uint64_t mul) {
    uint64_t a = (u ^ v) * mul;
    a ^= a >> 47;
    uint64_t b = (v ^ a) * mul;
    b ^= b >> 47;
    b *= mul;
    return b;
}
uint64_t hash128_to_64(uint64_t lo, uint64_t hi) { return hash_len16(lo, hi, 0x9ddfea08eb382d69ULL); }

uint64_t hash_len0_to16(const char* s, size_t len) {
    if (len >= 8) {
        const uint64_t mul = k2 + len * 2;
        const uint64_t a = fetch64(s) + k2;
        const uint64_t b = fetch64(s + len - 8);
        const uint64_t c = rotate(b, 37) * mul + a;
        const uint64_t d = (rotate(a, 25) + b) * mul;
        return hash_len16(c, d, mul);
    }
    if (len >= 4) {
        const uint64_t mul = k2 + len * 2;
        const uint64_t a = fetch32(s);
        return hash_len16(len + (a << 3), fetch32(s + len - 4), mul);
    }
    if (len > 0) {
        const uint8_t a = static_cast<uint8_t>(s[0]);
        const uint8_t b = static_cast<uint8_t>(s[len >> 1]);
        const uint8_t c = static_cast<uint8_t>(s[len - 1]);
        const uint32_t y = static_cast<uint32_t>(a) + (static_cast<uint32_t>(b) << 8);
        const uint32_t z = static_cast<uint32_t>(len) + (static_cast<uint32_t>(c) << 2);
        return shift_mix(y * k2 ^ z * k0) * k2;
    }
    return k2;
}

uint64_t hash_len17_to32(const char* s, size_t len) {
    const uint64_t mul = k2 + len * 2;
    const uint64_t a = fetch64(s) * k1;
    const uint64_t b = fetch64(s + 8);
    const uint64_t c = fetch64(s + len - 8) * mul;
    const uint64_t d = fetch64(s + len - 16) * k2;
    return hash_len16(rotate(a + b, 43) + rotate(c, 30) + d, a + rotate(b + k2, 18) + c, mul);
}

std::pair<uint64_t, uint64_t> weak_hash32(uint64_t w, uint64_t x, uint64_t y, uint64_t z, uint64_t a, uint64_t b) {
    a += w;
    b = rotate(b + a + z, 21);
    const uint64_t c = a;
    a += x;
    a += y;
    b += rotate(a, 44);
    return {a + z, b + c};
}
std::pair<uint64_t, uint64_t> weak_hash32(const char* s, uint64_t a, uint64_t b) {
    return weak_hash32(fetch64(s), fetch64(s + 8), fetch64(s + 16), fetch64(s + 24), a, b);
}

uint64_t hash_len33_to64(const char* s, size_t len) {
    const uint64_t mul = k2 + len * 2;
    uint64_t a = fetch64(s) * k2;
    uint64_t b = fetch64(s + 8);
    const uint64_t c = fetch64(s + len - 24);
    const uint64_t d = fetch64(s + len - 32);
    const uint64_t e = fetch64(s + 16) * k2;
    const uint64_t f = fetch64(s + 24) * 9;
    const uint64_t g = fetch64(s + len - 8);
    const uint64_t h = fetch64(s + len - 16) * mul;
    const uint64_t u = rotate(a + g, 43) + (rotate(b, 30) + c) * 9;
    const uint64_t v = ((a + g) ^ d) + f + 1;
    const uint64_t w = bswap64((u + v) * mul) + h;
    const uint64_t x = rotate(e + f, 42) + c;
    const uint64_t y = (bswap64((v + w) * mul) + g) * mul;
    const uint64_t z = e + f + c;
    a = bswap64((x + z) * mul + y) + b;
    b = shift_mix((z + a) * mul + d + h) * mul;
    return b + x;
}

}  // namespace

uint64_t city_hash64(std::string_view bytes) {
    const char* s = bytes.data();
    size_t len = bytes.size();
    if (len <= 32) return len <= 16 ? hash_len0_to16(s, len) : hash_len17_to32(s, len);
    if (len <= 64) return hash_len33_to64(s, len);
    // for strings over 64 bytes the end is hashed first, and the loop keeps 56 bytes of state: v, w, x, y and z
    uint64_t x = fetch64(s + len - 40);
    uint64_t y = fetch64(s + len - 16) + fetch64(s + len - 56);
    uint64_t z = hash128_to_64(fetch64(s + len - 48) + len, fetch64(s + len - 24));
    std::pair<uint64_t, uint64_t> v = weak_hash32(s + len - 64, len, z);
    std::pair<uint64_t, uint64_t> w = weak_hash32(s + len - 32, y + k1, x);
    x = x * k1 + fetch64(s);
    len = (len - 1) & ~static_cast<size_t>(63);
    do {
        x = rotate(x + y + v.first + fetch64(s + 8), 37) * k1;
        y = rotate(y + v.second + fetch64(s + 48), 42) * k1;
        x ^= w.second;
        y += v.first + fetch64(s + 40);
        z = rotate(z + w.first, 33) * k1;
        v = weak_hash32(s, v.second * k1, x + w.first);
        w = weak_hash32(s + 32, z + y, y + fetch64(s + 16));
        std::swap(z, x);
        s += 64;
        len -= 64;
    } while (len != 0);
    return hash128_to_64(hash128_to_64(v.first, w.first) + shift_mix(y) * k1 + z, hash128_to_64(v.second, w.second) + x);
}

std::string quest_variable_name(int64_t quest, std::string_view suffix) { return std::to_string(quest) + "." + std::string(suffix); }

std::string objective_variable_name(int64_t quest, int64_t objective, std::string_view suffix) {
    return std::to_string(quest) + "." + std::to_string(objective) + "." + std::string(suffix);
}

// ---- guids -------------------------------------------------------------------------------------------------------------------------------------------------------------

std::string Guid128::hex() const {
    static const char* const digits = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const uint32_t w : {a, b, c, d}) {
        for (int shift = 28; shift >= 0; shift -= 4) out += digits[(w >> shift) & 15u];
    }
    return out;
}

std::optional<Guid128> Guid128::from_hex(std::string_view s) {
    if (s.size() != 32) return std::nullopt;
    uint32_t words[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 32; ++i) {
        const char ch = s[i];
        uint32_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint32_t>(ch - 'a' + 10);
        else return std::nullopt;
        words[i / 8] = (words[i / 8] << 4) | d;
    }
    return Guid128{words[0], words[1], words[2], words[3]};
}

const char* create_name(Create c) {
    switch (c) {
        case Create::Ok: return "ok";
        case Create::InvalidStaticData: return "the quest is not in the data";
        case Create::MaxQuestLimitReached: return "the log is full";
        case Create::QuestDisabled: return "the quest is disabled";
        case Create::DefaultActivateNotSet: return "the quest is not switched on for activation";
        case Create::QuestAlreadyActive: return "the quest is in the log already";
        case Create::QuestNotRepeatable: return "the quest was finished and is not repeatable";
        case Create::FailedActivationRules: return "the quest's activation rule does not hold";
    }
    return "?";
}

// ---- the log ---------------------------------------------------------------------------------------------------------------------------------------------------------

QuestLog::QuestLog(uint64_t seed) {
    if (seed == 0) {
        std::random_device rd;
        seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    }
    rng_.seed(seed);
}

Guid128 QuestLog::new_guid() {
    Guid128 g;
    do {
        const uint64_t lo = rng_(), hi = rng_();
        g = Guid128{static_cast<uint32_t>(lo), static_cast<uint32_t>(lo >> 32), static_cast<uint32_t>(hi), static_cast<uint32_t>(hi >> 32)};
    } while (g.zero());
    return g;
}

const QuestInstance* QuestLog::find(int64_t record) const {
    for (const QuestInstance& q : instances_) {
        if (q.record == record) return &q;
    }
    return nullptr;
}

QuestInstance* QuestLog::find(int64_t record) {
    return const_cast<QuestInstance*>(static_cast<const QuestLog*>(this)->find(record));
}

const QuestInstance* QuestLog::find_guid(const Guid128& guid) const {
    for (const QuestInstance& q : instances_) {
        if (q.guid == guid) return &q;
    }
    return nullptr;
}

int32_t QuestLog::completions(int64_t record) const {
    const auto it = completed_.find(record);
    return it == completed_.end() ? 0 : it->second;
}

int64_t QuestLog::variable(const std::string& name) const {
    const auto it = variables_.find(name);
    return it == variables_.end() ? 0 : it->second;
}

void QuestLog::set_variable(const std::string& name, int64_t value) {
    if (value == 0) variables_.erase(name);   // (a variable at 0 is the same as none: the rules read 0 for both)
    else variables_[name] = value;
    changed_ = true;
}

Create QuestLog::can_start(const content::QuestRow* def, dialogue::Evaluator& ev, bool bypass_rules) const {
    if (!def) return Create::InvalidStaticData;
    if (instances_.size() >= kMaxQuests) return Create::MaxQuestLimitReached;
    if (def->disabled) return Create::QuestDisabled;
    if (!def->default_activate) return Create::DefaultActivateNotSet;
    if (find(def->guid)) return Create::QuestAlreadyActive;
    if (!def->repeatable && completions(def->guid) > 0) return Create::QuestNotRepeatable;
    if (!bypass_rules && !ev.holds(def->activation)) return Create::FailedActivationRules;
    return Create::Ok;
}

QuestInstance* QuestLog::start_instance(const content::QuestRow& def, double now, int64_t source) {
    QuestInstance q;
    q.guid = new_guid();
    q.record = def.guid;
    q.status = Status::Active;
    q.start_time = now;
    q.source = source;
    for (const content::QuestObjectiveRow& o : def.objectives) {
        ObjectiveState s;
        s.guid = new_guid();
        s.record = o.guid;
        s.hidden = o.hidden;
        s.awake = o.awake;
        s.active = o.awake;           // an objective that starts awake starts active (the game's own quest: Awake 1 and Active 1)
        s.start_time = o.awake ? now : 0;
        q.objectives.push_back(s);
    }
    instances_.push_back(std::move(q));
    changed_ = true;
    return &instances_.back();
}

Started QuestLog::start(const content::QuestRow& def, double now, int64_t source) {
    Started out;
    QuestInstance* q = start_instance(def, now, source);
    out.instance = q;
    for (const content::QuestStartAction& a : def.start_actions) {
        const auto objective = [&]() -> ObjectiveState* {
            for (ObjectiveState& o : q->objectives) {
                if (o.record == a.objective) return &o;
            }
            return nullptr;
        };
        if (a.conditional) {
            out.skipped.push_back(a.action + " (with a condition)");
        } else if (a.action == "ActivateObjective" && a.objective != 0) {
            if (ObjectiveState* o = objective()) {
                o->awake = o->active = true;
                o->start_time = now;
            }
        } else if (a.action == "RevealObjective" && a.objective != 0) {
            if (ObjectiveState* o = objective()) o->hidden = false;
        } else if (a.action == "WakeObjective" && a.objective != 0) {
            if (ObjectiveState* o = objective()) o->awake = true;
        } else if (a.action == "SetPlayerVariable" && !a.variable.empty()) {
            set_variable(a.variable, a.value);
        } else if (a.action == "IncrementPlayerVariable" && !a.variable.empty()) {
            set_variable(a.variable, variable(a.variable) + a.value);
        } else {
            out.skipped.push_back(a.action);
        }
    }
    q->wire_dirty = true;
    return out;
}

bool QuestLog::remove(int64_t record) {
    for (auto it = instances_.begin(); it != instances_.end(); ++it) {
        if (it->record != record) continue;
        removed_.push_back({it->cache_id, it->vars_id});
        instances_.erase(it);
        changed_ = true;
        return true;
    }
    return false;
}

bool QuestLog::finish(int64_t record) {
    if (!remove(record)) return false;
    ++completed_[record];
    changed_ = true;
    return true;
}

void QuestLog::clear(int64_t record) {
    if (record == 0) {
        completed_.clear();
        variables_.clear();
    } else {
        completed_.erase(record);
    }
    changed_ = true;
}

std::vector<QuestLog::Removed> QuestLog::take_removed() { return std::exchange(removed_, {}); }

std::optional<int64_t> QuestLog::quest_count(int64_t quest, const std::string& query) const {
    if (query == "Active") return find(quest) ? 1 : 0;
    if (query == "Completed") return completions(quest);
    if (query == "Failed" || query == "ReadyForHandIn") return 0;
    return std::nullopt;
}

std::optional<int64_t> QuestLog::objective_count(int64_t quest, int64_t objective, const std::string& query) const {
    const QuestInstance* q = find(quest);
    if (!q) return 0;   // an objective of a quest the character does not have is in no state
    for (const ObjectiveState& o : q->objectives) {
        if (o.record != objective) continue;
        if (query == "Active") return o.active ? 1 : 0;
        if (query == "Awake") return o.awake ? 1 : 0;
        if (query == "Hidden") return o.hidden ? 1 : 0;
        if (query == "Revealed") return o.hidden ? 0 : 1;
        if (query == "Completed") return o.completed ? 1 : 0;
        if (query == "Succeeded") return o.succeeded ? 1 : 0;
        if (query == "Failed") return o.failed ? 1 : 0;
        if (query == "Count") return o.count;
        return std::nullopt;
    }
    return 0;
}

svc::SavedQuestLog QuestLog::to_saved() const {
    svc::SavedQuestLog out;
    for (const QuestInstance& q : instances_) {
        svc::SavedQuest s;
        s.instance_guid = q.guid.hex();
        s.record_id = q.record;
        s.status = static_cast<int32_t>(q.status);
        s.start_time = q.start_time;
        s.source_guid = q.source;
        for (const ObjectiveState& o : q.objectives) {
            svc::SavedObjective so;
            so.instance_guid = o.guid.hex();
            so.record_id = o.record;
            so.active = o.active;
            so.awake = o.awake;
            so.hidden = o.hidden;
            so.completed = o.completed;
            so.succeeded = o.succeeded;
            so.failed = o.failed;
            so.count = o.count;
            so.start_time = o.start_time;
            s.objectives.push_back(std::move(so));
        }
        out.instances.push_back(std::move(s));
    }
    for (const auto& [record, count] : completed_) out.completed.push_back({record, count});
    for (const auto& [name, value] : variables_) out.variables.push_back({name, value});
    return out;
}

size_t QuestLog::load(const svc::SavedQuestLog& saved, const content::QuestData& book, double now) {
    size_t dropped = 0;
    instances_.clear();
    completed_.clear();
    variables_.clear();
    removed_.clear();
    for (const svc::SavedQuest& s : saved.instances) {
        const auto def = book.quests.find(s.record_id);
        const auto guid = Guid128::from_hex(s.instance_guid);
        if (def == book.quests.end() || !guid || guid->zero() || find(s.record_id) || s.status < 0 || s.status > 3) {
            ++dropped;
            continue;
        }
        QuestInstance q;
        q.guid = *guid;
        q.record = s.record_id;
        q.status = static_cast<Status>(s.status);
        q.start_time = std::min(s.start_time, now);
        q.source = s.source_guid;
        // one state per objective of the record, in the record's order: the saved one when there is one, else a fresh one (the data may have changed since)
        for (const content::QuestObjectiveRow& o : def->second.objectives) {
            ObjectiveState st;
            bool found = false;
            for (const svc::SavedObjective& so : s.objectives) {
                if (so.record_id != o.guid) continue;
                const auto og = Guid128::from_hex(so.instance_guid);
                if (!og || og->zero()) break;
                st.guid = *og;
                st.record = so.record_id;
                st.active = so.active;
                st.awake = so.awake;
                st.hidden = so.hidden;
                st.completed = so.completed;
                st.succeeded = so.succeeded;
                st.failed = so.failed;
                st.count = so.count;
                st.start_time = std::min(so.start_time, now);
                found = true;
                break;
            }
            if (!found) {
                st.guid = new_guid();
                st.record = o.guid;
                st.hidden = o.hidden;
                st.awake = o.awake;
                st.active = o.awake;
                ++dropped;   // (a repair: the log is saved again)
            }
            q.objectives.push_back(st);
        }
        instances_.push_back(std::move(q));
    }
    for (const svc::SavedCompleted& c : saved.completed) {
        if (c.record_id > 0 && c.count > 0) completed_[c.record_id] = c.count;
    }
    for (const svc::SavedVariable& v : saved.variables) {
        if (!v.name.empty() && v.value != 0) variables_[v.name] = v.value;
    }
    changed_ = dropped != 0;
    return dropped;
}

std::vector<std::pair<uint64_t, int32_t>> instance_variables(const QuestInstance& q) {
    std::vector<std::pair<uint64_t, int32_t>> out;
    const auto put = [&](const std::string& name, bool on) { out.emplace_back(city_hash64(name), on ? 1 : 0); };
    // the order the game lists them in (a quest with one objective, captured: Completed, Active, Failed, Paused; then the objective's Completed, Active, Failed, Succeeded, Hidden, Count, Awake)
    put(quest_variable_name(q.record, "Completed"), q.status == Status::Completed);
    put(quest_variable_name(q.record, "Active"), q.status == Status::Active);
    put(quest_variable_name(q.record, "Failed"), q.status == Status::Failed);
    put(quest_variable_name(q.record, "Paused"), q.status == Status::Paused);
    for (const ObjectiveState& o : q.objectives) {
        put(objective_variable_name(q.record, o.record, "Completed"), o.completed);
        put(objective_variable_name(q.record, o.record, "Active"), o.active);
        put(objective_variable_name(q.record, o.record, "Failed"), o.failed);
        put(objective_variable_name(q.record, o.record, "Succeeded"), o.succeeded);
        put(objective_variable_name(q.record, o.record, "Hidden"), o.hidden);
        out.emplace_back(city_hash64(objective_variable_name(q.record, o.record, "Count")), o.count);
        put(objective_variable_name(q.record, o.record, "Awake"), o.awake);
    }
    return out;
}

}  // namespace aoc::worldd::quest
