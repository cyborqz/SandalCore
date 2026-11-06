// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  effects/effect_book.h
//
//  The effects an actor holds: the instances an effect record's hits put on it, with the stacking and refresh rules of the record, the moments of their ticks and
//  ends, and the list of changes a replicated array needs.  Creatures keep one (combat/combat_world.h); the player's own effects are still the ability runtime's.
//
//  The book knows nothing of expressions, hits or the world.  Whoever resolved the hit has already worked out every number an application needs (`Applied`: the
//  stacks, the duration, the in-place limit and the values of one stack's stat mods, evaluated for the character that applied it), so a book can be driven from the
//  world's tick with nothing but the clock.
//
//  The rules (the same the ability runtime applies to the player's effects):
//    OverrideAlways    one instance; a new application replaces it (a new instance, a new key), unless the record never refreshes
//    StackInPlace      one instance whose stacks grow up to the in-place limit; every application refreshes it
//    StackSeperate     a new instance per application, up to the record's global limit
//    OverrideDuration  one instance; an application refreshes it
//  A record that never refreshes keeps its times (a new stack still counts).  An effect that extends its duration (ExtendDuration) grows its time left by the amount the
//  application worked out (`Applied::extension`) and never past the record's longest (`extended_max`); one whose amount is not known is refreshed to its full duration.
//  A record that compares sources keeps one instance per instigator: an application refreshes, stacks or replaces the instance of the SAME instigator, and another instigator's
//  makes an instance of its own (up to the record's global limit, 0 for no limit).
//  A hit's effect that works out to no stacks is never applied: that is the data's way to make an effect conditional.
//  The instances also answer the stat engine: `engine_effects()` is the list a character's stats read (the stat mods of its stacks, GetEffectCount, GetEffectCountByTag).
// ============================================================================
#pragma once

#include "session/ability_catalog.h"
#include "stats/stat_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace aoc::worldd::effects {

inline constexpr double kForever = std::numeric_limits<double>::infinity();

/// One application of an effect with all its numbers worked out.
struct Applied {
    const abilities::EffectDef*   def = nullptr;
    int                           stacks = 1;            // 1 or more
    double                        duration = kForever;   // seconds; kForever: it does not end by itself
    int                           in_place_limit = 1;    // the most stacks one instance holds (StackInPlace)
    std::vector<stats::EffectMod> mods;                  // one stack's stat mod values
    double                        extension = -1;        // seconds one more application adds under ExtendDuration; negative: the record gives none, the duration is refreshed in full
    double                        extended_max = 0;      // the most seconds an extended instance has left; 0: no limit
};

/// An effect on its holder.
struct Instance {
    int64_t                       key = 0;               // what the replicated item is known by (EffectId)
    const abilities::EffectDef*   def = nullptr;
    int                           stacks = 1;
    double                        start = 0, end = kForever;
    double                        next_tick = kForever;  // kForever: it does not tick
    std::vector<stats::EffectMod> mods;
    uint64_t                      instigator = 0;        // the actor that applied it (the one it is shown to be from); 0: none
};

class Book {
public:
    /// The most instances one holder keeps (a hostile list cannot grow the book without bound).
    static constexpr size_t kMaxInstances = 256;

    /// What a call changed, in the order the replicated array must see it: the keys that went, then the instances added and the ones changed in place.
    struct Changes {
        std::vector<int64_t>  removed;
        std::vector<Instance> added;
        std::vector<Instance> changed;
        bool empty() const { return removed.empty() && added.empty() && changed.empty(); }
    };

    /// The earliest thing due at or before `now`: an end, or a tick.  An end comes before a tick of the same moment.
    struct Due {
        enum class Kind : uint8_t { None, End, Tick };
        Kind    kind = Kind::None;
        int64_t key = 0;
        double  at = 0;
    };

    explicit Book(int64_t first_key = 1) : next_key_(first_key) {}

    const std::vector<Instance>& instances() const { return items_; }
    bool   empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }

    /// Applies `a` for `instigator` at time `t`.  When it made a NEW instance (the caller runs that effect's Apply hits then) `created` holds a copy of it and the
    /// result is true; a stack added in place, a refresh and an application that changed nothing give false.
    bool apply(const Applied& a, uint64_t instigator, double t, Changes& out, Instance* created = nullptr) {
        if (!a.def || a.stacks < 1) return false;
        const content::EffectRow& row = a.def->row;
        const bool never_refresh = row.stack_duration_refresh.find("NeverRefresh") != std::string::npos;
        const bool extend = row.stack_duration_refresh.find("ExtendDuration") != std::string::npos;
        const bool in_place = row.stack_type.find("StackInPlace") != std::string::npos;
        const bool separate = row.stack_type.find("StackSeperate") != std::string::npos;
        const bool override_duration = row.stack_type.find("OverrideDuration") != std::string::npos;
        // the instance this application is for: the first of the record, or (a record that compares sources) the first of the same instigator
        Instance* existing = nullptr;
        int same = 0;
        for (Instance& e : items_) {
            if (e.def != a.def) continue;
            ++same;
            if (!existing && (!row.compare_source || e.instigator == instigator)) existing = &e;
        }
        const auto refresh = [&](Instance& e) {
            e.mods = a.mods;
            e.instigator = instigator;   // (a record that compares sources only reaches an instance of this same instigator)
            if (never_refresh) return;
            if (extend && a.extension >= 0 && !std::isinf(e.end) && !std::isinf(a.duration)) {
                // the time left grows by the amount the application worked out, up to the longest the record allows (what is left already is never cut)
                double end = e.end + a.extension;
                if (a.extended_max > 0) end = std::min(end, t + a.extended_max);
                e.start = t;
                e.end = std::max(end, e.end);
                return;
            }
            e.start = t;
            e.end = std::isinf(a.duration) ? kForever : t + a.duration;
        };
        const auto fresh = [&](int stacks) {
            if (items_.size() >= kMaxInstances) return false;
            Instance e;
            e.key = next_key_++;
            e.def = a.def;
            e.stacks = stacks;
            e.start = t;
            e.end = std::isinf(a.duration) ? kForever : t + a.duration;
            e.mods = a.mods;
            e.next_tick = row.tick_timer > 0 ? (row.ignore_initial_tick ? t + row.tick_timer : t) : kForever;
            e.instigator = instigator;
            items_.push_back(e);
            out.added.push_back(e);
            if (created) *created = e;
            return true;
        };
        if (!existing) {
            // a record that compares sources and is on the holder already from other instigators: one more instance only up to the record's global limit (0: no limit)
            if (row.compare_source && same > 0 && row.global_stack_limit > 0 && same >= row.global_stack_limit) return false;
            return fresh(in_place ? std::min(a.stacks, std::max(1, a.in_place_limit)) : 1);
        }
        if (in_place) {
            existing->stacks = std::min(std::max(1, a.in_place_limit), existing->stacks + a.stacks);
            refresh(*existing);
            note_changed(*existing, out);
        } else if (separate) {
            if (same < std::max(1, row.global_stack_limit)) return fresh(1);
        } else if (override_duration) {
            refresh(*existing);
            note_changed(*existing, out);
        } else {   // OverrideAlways: one instance, replaced (a record that never refreshes keeps what it has)
            if (never_refresh) return false;
            const int64_t old_key = existing->key;
            remove_key(old_key, out);
            return fresh(1);
        }
        return false;
    }

    /// The earliest end or tick due at or before `now`.
    Due next_due(double now) const {
        Due best;
        for (const Instance& e : items_) {
            if (e.end <= now && (best.kind == Due::Kind::None || e.end < best.at || (e.end == best.at && best.kind == Due::Kind::Tick))) best = {Due::Kind::End, e.key, e.end};
            if (e.next_tick <= now && (best.kind == Due::Kind::None || e.next_tick < best.at)) best = {Due::Kind::Tick, e.key, e.next_tick};
        }
        return best;
    }

    /// The instance reached its end: it is taken off and returned (its Expire and End hits are the caller's).  False when there is none with that key.
    bool end_of(int64_t key, Instance& gone, Changes& out) {
        for (const Instance& e : items_) {
            if (e.key != key) continue;
            gone = e;
            remove_key(key, out);
            return true;
        }
        return false;
    }

    /// The instance ticked at `at`: its next tick is a period later.  False when there is none with that key.
    bool ticked(int64_t key, double at, Instance& copy) {
        for (Instance& e : items_) {
            if (e.key != key) continue;
            const double period = e.def->row.tick_timer;
            e.next_tick = period > 0 ? at + period : kForever;
            copy = e;
            return true;
        }
        return false;
    }

    /// Takes the instance with `key` off.  False when there is none.
    bool remove(int64_t key, Changes& out) { return remove_key(key, out); }

    /// Takes off everything `instigator` applied (it is gone); the number of instances that went.
    size_t remove_from(uint64_t instigator, Changes& out) {
        size_t n = 0;
        for (size_t i = 0; i < items_.size();) {
            if (items_[i].instigator == instigator) {
                out.removed.push_back(items_[i].key);
                items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
                ++n;
            } else {
                ++i;
            }
        }
        return n;
    }

    /// Takes everything off (a death).
    void clear(Changes& out) {
        for (const Instance& e : items_) out.removed.push_back(e.key);
        items_.clear();
    }

    /// Seconds left of the longest-lasting instance of an effect record (0: it is not on; a record without an end reads as a very long time).
    double remaining(int64_t effect_guid, double now) const {
        double best = 0;
        for (const Instance& e : items_) {
            if (e.def->row.guid != effect_guid) continue;
            best = std::max(best, std::isinf(e.end) ? 1.0e9 : std::max(0.0, e.end - now));
        }
        return best;
    }

    /// The stacks of the instances of an effect record.
    int stacks_of(int64_t effect_guid) const {
        int n = 0;
        for (const Instance& e : items_) {
            if (e.def->row.guid == effect_guid) n += e.stacks;
        }
        return n;
    }

    /// The instances as the stat engine takes them: what a holder's stats read (the stat mods of each stack, GetEffectCount, GetEffectCountByTag; a tag is one of the record's
    /// Tags or EffectTags, as the player's list gives them).
    std::vector<stats::ActiveEffect> engine_effects() const {
        std::vector<stats::ActiveEffect> out;
        out.reserve(items_.size());
        for (const Instance& e : items_) {
            stats::ActiveEffect a;
            a.effect = {stats::kRefTypeEffect, e.def->row.guid};
            a.stacks = e.stacks;
            a.tags = e.def->row.tags;
            a.tags.insert(a.tags.end(), e.def->row.effect_tags.begin(), e.def->row.effect_tags.end());
            a.mods = e.mods;
            out.push_back(std::move(a));
        }
        return out;
    }

    /// The crowd control the instances put on their holder: the status flags of every live instance's crowd-control record together (content::kCcSnare ...), the strength of the strongest live snare, whether
    /// a live record overrides the client's own movement, and the animation of the strongest live record that asks for one (a stun's, a trip's: the higher priority plays; of two equal, the one that lasts
    /// longer).  Snares do not add up or multiply: a holder under two keeps the speed the stronger one leaves it (1 - snare).  Equal when two books hold the same crowd control.
    struct Crowd {
        int64_t flags = 0;
        double  snare = 0;
        bool    override_move = false;                // bOverrideClientAuthMovement
        int     anim = content::kCcAnimNone;          // content::kCcAnimStunned, kCcAnimTripped or none
        int64_t anim_record = 0;                      // ... the crowd-control record to animate,
        double  anim_start = 0;                       // ... when its effect began (this book's clock)
        double  anim_seconds = 0;                     // ... and how long it lasts, as the effect's item states it (a float's worth)
        bool operator==(const Crowd&) const = default;
    };
    Crowd cc() const {
        Crowd out;
        const Instance* shown = nullptr;
        for (const Instance& e : items_) {
            if (!e.def || !e.def->cc) continue;
            const content::CrowdControlRow& r = *e.def->cc;
            out.flags |= r.applied_status;
            if (r.applied_status & content::kCcSnare) out.snare = std::max(out.snare, r.snare_strength);
            out.override_move = out.override_move || r.override_client_move;
            if (animation_of(r) == content::kCcAnimNone) continue;
            if (!shown || r.priority > shown->def->cc->priority ||
                (r.priority == shown->def->cc->priority && (e.end > shown->end || (e.end == shown->end && e.start > shown->start)))) {
                shown = &e;
            }
        }
        if (shown) {
            out.anim = animation_of(*shown->def->cc);
            out.anim_record = shown->def->cc->guid;
            out.anim_start = shown->start;
            out.anim_seconds = std::isinf(shown->end) ? static_cast<double>(std::numeric_limits<float>::max()) : static_cast<double>(static_cast<float>(shown->end - shown->start));
        }
        return out;
    }
    /// The animation a crowd-control record asks the client to play: content::kCcAnimStunned, kCcAnimTripped, or kCcAnimNone (a record that asks for none or for one of its own).
    static int animation_of(const content::CrowdControlRow& r) {
        if (r.animation_request.find("Stunned") != std::string::npos) return content::kCcAnimStunned;
        if (r.animation_request.find("Tripped") != std::string::npos) return content::kCcAnimTripped;
        return content::kCcAnimNone;
    }

private:
    bool remove_key(int64_t key, Changes& out) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].key != key) continue;
            // an instance this same change added never reached the replicated array: it is dropped from the change, not removed
            bool unsent = false;
            for (auto it = out.added.begin(); it != out.added.end(); ++it) {
                if (it->key == key) {
                    out.added.erase(it);
                    unsent = true;
                    break;
                }
            }
            for (auto it = out.changed.begin(); it != out.changed.end();) it = it->key == key ? out.changed.erase(it) : it + 1;
            if (!unsent) out.removed.push_back(key);
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        return false;
    }

    // an instance changed in place; one that is new in this same change stays an addition with its latest values
    static void note_changed(const Instance& e, Changes& out) {
        for (Instance& a : out.added) {
            if (a.key == e.key) {
                a = e;
                return;
            }
        }
        for (Instance& c : out.changed) {
            if (c.key == e.key) {
                c = e;
                return;
            }
        }
        out.changed.push_back(e);
    }

    std::vector<Instance> items_;
    int64_t               next_key_ = 1;
};

}  // namespace aoc::worldd::effects
