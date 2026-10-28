// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  respawn/embersprings.h  (Embersprings: the respawn points a player discovers, attunes to and comes back at)
//
//  The rules of the Embersprings, with no session, no wire and no clock in them (everything is passed in), so that the session's hooks stay small:
//
//    discovery   a player who comes within a shrine's radius (the place's AutoAttuneRadius, a sphere around the shrine) has discovered it: its number in the master list of places
//                goes into the character's set.  The server does this itself, no client report.
//    the home    one discovered place the character made its home by using the shrine (a prompt, then a yes); the home respawn waits two hours after its last use.
//    the choices the death window lists the discovered shrine nearest the body (by horizontal distance) and the home; the home waits for its cooldown.
//
//  Header-only on purpose (a new source file would have to be listed in worldd's CMakeLists.txt, which every player's navigation mesh build is stamped with).
// ============================================================================
#pragma once

#include "world/actor_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace aoc::worldd::respawn {

/// Seconds the home respawn waits after its last use (the client counts the same sum from the replicated last use).
inline constexpr double kHomeCooldownSeconds = 7200.0;
/// The largest number in the master list of places a character's set can hold (one byte for each, in blocks of 256).
inline constexpr int kMaxPlaceNumber = 255;
/// How close (cm) a player has to stand to use a shrine: the interaction's radius, with the room the client's position and ours may differ by.
inline constexpr double kUseRangeCm = 500.0;
/// The chat category of the discovery line and of the attunement line (the client's own copy of it uses the same one).
inline constexpr int kChatCategory = 124;

/// The EPlayerRespawnType of a respawn definition that belongs to an attuned place.
inline constexpr int kAttunedRespawnKind = 2;

inline uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline double unit_of(uint64_t h) { return static_cast<double>(h >> 11) / 9007199254740992.0; }

/// A character's places.
struct Attunements {
    std::vector<int> discovered;        // numbers in the master list, in the order they were discovered
    int              home = 0;          // the home place's number; 0 for none
    int64_t          home_last_used_ms = 0;   // Unix milliseconds on the server's wall clock when the home respawn was last used; 0 for never

    bool has(int number) const { return std::find(discovered.begin(), discovered.end(), number) != discovered.end(); }
    /// Adds a place; false (nothing changes) for a number out of range or one the set holds.
    bool discover(int number) {
        if (number < 1 || number > kMaxPlaceNumber || has(number)) return false;
        discovered.push_back(number);
        return true;
    }
    bool operator==(const Attunements& o) const { return discovered == o.discovered && home == o.home && home_last_used_ms == o.home_last_used_ms; }
};

/// The set as the client's PlayerInfo holds it: blocks of 256 numbers, each block the bytes `number % 256` in the order they were discovered (a block with none is kept so that the
/// block's place in the list is its number / 256).
inline std::vector<std::vector<uint8_t>> blocks_of(const Attunements& a) {
    std::vector<std::vector<uint8_t>> out;
    for (const int n : a.discovered) {
        if (n < 1 || n > kMaxPlaceNumber) continue;
        const size_t block = static_cast<size_t>(n / 256);
        if (out.size() <= block) out.resize(block + 1);
        out[block].push_back(static_cast<uint8_t>(n % 256));
    }
    return out;
}

/// True while the home respawn waits (the client and the server count it the same way).
inline bool home_on_cooldown(const Attunements& a, int64_t now_ms) {
    if (a.home_last_used_ms <= 0) return false;
    return static_cast<double>(now_ms) < static_cast<double>(a.home_last_used_ms) + kHomeCooldownSeconds * 1000.0;
}

/// Seconds of the cooldown that are left (0 when it is over).
inline double home_cooldown_left(const Attunements& a, int64_t now_ms) {
    if (!home_on_cooldown(a, now_ms)) return 0;
    return (static_cast<double>(a.home_last_used_ms) + kHomeCooldownSeconds * 1000.0 - static_cast<double>(now_ms)) / 1000.0;
}

/// One placed Emberspring.
struct Shrine {
    uint64_t    actor = 0;         // the WorldActor (registry key)
    int64_t     place = 0;         // attune_point guid (what the actor's AttuneRecordId holds, what the prompt names)
    int64_t     respawn = 0;       // respawn_point guid (what the death window's choices name)
    int         number = 0;        // the place's number in the master list (1 to 255); 0 for none
    double      x = 0, y = 0, z = 0, yaw = 0;   // cm, degrees: where the actor stands
    double      radius = 0;        // cm: the discovery radius (AutoAttuneRadius)
    double      box_x = 0, box_y = 0, box_z = 0;   // the respawn box's half sizes (cm)
    double      exclusion = 0;     // cm: how far from the middle a player comes back
    std::string name;              // the place's name ("Flame and Formula")
};

/// Where a player comes back at a shrine: a point of the ring between the exclusion radius and the box's smaller half size, a different one for each `seed` (the data gives the box and
/// the radius; where inside them retail puts the player is not known), facing the shrine.
struct Spot {
    double x = 0, y = 0, z = 0, yaw = 0;
};
inline Spot spot_at(const Shrine& s, uint64_t seed) {
    const double lo = std::max(0.0, s.exclusion);
    const double hi = std::max(lo, std::min(s.box_x > 0 ? s.box_x : lo, s.box_y > 0 ? s.box_y : lo));
    const double r = lo + (hi - lo) * unit_of(mix64(seed));
    const double a = 6.283185307179586 * unit_of(mix64(seed ^ 0xA5A5A5A5DEADBEEFull));
    Spot out;
    out.x = s.x + r * std::cos(a);
    out.y = s.y + r * std::sin(a);
    out.z = s.z;
    out.yaw = std::atan2(s.y - out.y, s.x - out.x) * 180.0 / 3.141592653589793;
    return out;
}

/// The Embersprings of the area.
class ShrineBook {
public:
    void add(Shrine s) { shrines_.push_back(std::move(s)); }
    const std::vector<Shrine>& all() const { return shrines_; }
    bool empty() const { return shrines_.empty(); }

    const Shrine* by_place(int64_t place) const { return find([&](const Shrine& s) { return s.place == place; }); }
    const Shrine* by_respawn(int64_t respawn) const { return respawn == 0 ? nullptr : find([&](const Shrine& s) { return s.respawn == respawn; }); }
    const Shrine* by_number(int number) const { return number <= 0 ? nullptr : find([&](const Shrine& s) { return s.number == number; }); }
    const Shrine* by_actor(uint64_t actor) const { return actor == 0 ? nullptr : find([&](const Shrine& s) { return s.actor == actor; }); }

    /// The shrines whose discovery radius holds the point (a sphere, as the shrine's own tick asks), nearest first.
    std::vector<const Shrine*> reaching(double x, double y, double z) const {
        std::vector<const Shrine*> out;
        for (const Shrine& s : shrines_) {
            if (s.number <= 0 || !(s.radius > 0)) continue;
            const double dx = s.x - x, dy = s.y - y, dz = s.z - z;
            if (dx * dx + dy * dy + dz * dz <= s.radius * s.radius) out.push_back(&s);
        }
        std::sort(out.begin(), out.end(), [&](const Shrine* a, const Shrine* b) {
            return (a->x - x) * (a->x - x) + (a->y - y) * (a->y - y) < (b->x - x) * (b->x - x) + (b->y - y) * (b->y - y);
        });
        return out;
    }

    /// The discovered shrine nearest the point by horizontal distance (the dead player's body); nullptr when the character discovered none that stands here.
    const Shrine* closest_discovered(const Attunements& a, double x, double y) const {
        const Shrine* best = nullptr;
        double best_d = 0;
        for (const Shrine& s : shrines_) {
            if (s.number <= 0 || !a.has(s.number)) continue;
            const double d = std::hypot(s.x - x, s.y - y);
            if (!best || d < best_d) {
                best = &s;
                best_d = d;
            }
        }
        return best;
    }

    /// The shrine of the character's home; nullptr for none, or one this area does not hold.
    const Shrine* home_of(const Attunements& a) const { return by_number(a.home); }

private:
    template <class F>
    const Shrine* find(F&& f) const {
        for (const Shrine& s : shrines_) {
            if (f(s)) return &s;
        }
        return nullptr;
    }
    std::vector<Shrine> shrines_;
};

/// What the death window lists.
struct Choices {
    std::vector<int64_t> closest;   // respawn definitions: the discovered shrine nearest the body (none, or one)
    int64_t              home = 0;  // the home's respawn definition; 0 for none
};

inline Choices choices_for(const ShrineBook& book, const Attunements& a, double body_x, double body_y) {
    Choices c;
    if (const Shrine* s = book.closest_discovered(a, body_x, body_y)) c.closest.push_back(s->respawn);
    if (const Shrine* h = book.home_of(a)) c.home = h->respawn;
    return c;
}

/// What the server makes of a choice the client sent.
enum class Pick : uint8_t {
    Accepted,         // a respawn definition of the list, or the home outside its cooldown
    HomeOnCooldown,   // the home, while it waits (the choices go again with the flag set)
    Invalid,          // anything else (the choices go again, marked)
};

/// A record of the list is accepted as it stands (the home too, when it is the list's: retail checks the list first); the home otherwise only outside its cooldown.
inline Pick judge(const Choices& c, int64_t record, const Attunements& a, int64_t now_ms) {
    if (record == 0) return Pick::Invalid;
    for (const int64_t r : c.closest) {
        if (r == record) return Pick::Accepted;
    }
    if (c.home != 0 && record == c.home) return home_on_cooldown(a, now_ms) ? Pick::HomeOnCooldown : Pick::Accepted;
    return Pick::Invalid;
}

/// The line the player reads when it discovers a place.
inline std::string discovery_line(const std::string& place) { return "You have discovered the " + place + " Emberspring."; }

}  // namespace aoc::worldd::respawn
