// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/actor_view_shadow_redo_tests.inc: two connections' shadows of one mob, and the update built once for both (session/shared_updates.h)
//
//  An update is built from the mob and the connection's shadow: the handles whose value differs from the shadow's entry go out, a handle with no entry goes out as new.  If two
//  connections' shadows are at the same version, is the next update the same for both, even when one shadow was started by an open at that version and the other reached it by updates?
//  Not always: an entry stays in a shadow after the value it held was taken away from the mob (RepState::erase), so a value set again is compared against it by one shadow and sent as
//  new by the other; a subobject that was taken away and added again is new to the shadow that never had it.  The registry counts those changes in WorldActor::shape_epoch, and a view
//  shares only when its shadow was started under the mob's present epoch.
//
//  1. The shape epoch counts what it should (a value or a subobject taken away, an array that loses an element value; not a value set, a subobject added, or a position).
//  2. The property test.  Mobs with the schemas of the ones the loader builds (the actor, its alignment, info, combat, ability and interactable components), random changes (values set to
//     a new value, to what they were and to the default, values taken away and set again, arrays, a cast in the ability fast array, subobjects taken away and added again, nothing at all, a
//     step), and connections that open and update at random, so that pairs of shadows at the same version were reached in different ways.  Every pair at the same version is compared: the
//     update each would build (the bytes, the record of what it carried) and the shadow after it, against the first one's update with its ShadowRedo applied to the second shadow.
//       - where the epoch says both may share, nothing may differ;
//       - the same sequences with the epoch ignored DO differ (the check that the property test can see the difference it guards against);
//       - sequences that only add and change (no value or subobject taken away) never differ, with or without the epoch.
//  3. An array that loses an element value that was sent as a zero (the zero is not sent again when the value goes, so the shadow that was sent it keeps it and a shadow started after does
//     not have it): built step by step, since the random sequences meet it only by chance.  The two shadows build different updates, and the epoch counts the array that lost the value.
// ============================================================================
#include "actor_view_scene.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace redo_checks {

using namespace view_scene;

/// One connection: its shadow, the version it was brought to and the shape epoch it was started under.
struct Conn {
    repl::ActorShadow shadow;
    uint32_t          sent_version = 0;
    uint32_t          epoch = 0;
    bool              open = false;
};

struct Scene {
    world::ActorRegistry reg;
    uint64_t             id = 4000;
    Rng                  rng;
    combat::NpcCastState cast;
    struct Erased {
        size_t        object;
        uint16_t      handle;
        repl::Value   value;
    };
    std::vector<Erased>                          erased;
    std::vector<std::pair<size_t, repl::Subobject>> removed;
    bool                                         shape_changes = true;
    size_t ops = 0, sets = 0, arrays = 0, casts = 0, erases = 0, resets = 0, removals = 0, readds = 0, moves = 0;
};

/// One random change of the mob, through the registry (so its counters and its shape epoch follow).
inline void mutate(Scene& sc) {
    Rng& rng = sc.rng;
    ++sc.ops;
    const int roll = static_cast<int>(rng.below(1000));
    const world::Change kind = world::Change::State;
    if (roll < 380 || (sc.shape_changes && roll >= 860)) {   // a scalar set to a new value, to what it was, or to the default
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            repl::ObjectState& o = object_of(w.actor, static_cast<size_t>(rng.below(static_cast<int64_t>(w.actor.subobjects.size()) + 1)));
            const auto& cmds = o.schema->commands;
            const schema::RepCommand& c = cmds[static_cast<size_t>(rng.below(static_cast<int64_t>(cmds.size())))];
            if (c.type == schema::PropType::NetSerializeStruct || c.type == schema::PropType::DynamicArray || (c.flags & schema::kRepCustomDelta)) return;
            std::optional<repl::Value> v;
            if (const repl::Value* cur = o.state.get(c.handle); cur && rng.chance(40)) v = *cur;   // what it was
            else v = scalar_value(c, rng.chance(25) ? 0 : rng.below(4));
            if (v && o.state.set(*o.schema, c.handle, std::move(*v))) ++sc.sets;
        });
    } else if (roll < 480) {   // an array
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            repl::ObjectState& o = object_of(w.actor, static_cast<size_t>(rng.below(static_cast<int64_t>(w.actor.subobjects.size()) + 1)));
            std::vector<const schema::RepCommand*> arrays;
            for (const schema::RepCommand& c : o.schema->commands) {
                if (c.type == schema::PropType::DynamicArray && !(c.flags & schema::kRepCustomDelta)) arrays.push_back(&c);
            }
            if (arrays.empty()) return;
            const schema::RepCommand& c = *arrays[static_cast<size_t>(rng.below(static_cast<int64_t>(arrays.size())))];
            if (auto v = array_value(*o.schema, c, rng); v && o.state.set(*o.schema, c.handle, std::move(*v))) ++sc.arrays;
        });
    } else if (roll < 580) {   // a cast in the ability component's fast array: an item added, then changed
        combat::NpcCast c;
        c.ability = 1000 + rng.below(3);
        c.montage = 0x9000 + 2 * static_cast<uint64_t>(rng.below(3));
        const repl::ExportChain montage = {link(c.montage, "Montage", false, 0x55)};
        if (!sc.cast.playing) combat::start_npc_cast(sc.reg, sc.id, sc.cast, c, montage);
        else combat::end_npc_cast(sc.reg, sc.id, sc.cast);
        ++sc.casts;
    } else if (roll < 680) {   // a position alone
        sc.reg.modify(sc.id, world::Change::Movement, [&](world::WorldActor& w) {
            repl::RepMovement m;
            m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, rng.below(100000), rng.below(100000), 1200000};
            m.rotation = repl::Rot{0, static_cast<uint16_t>(rng.below(200)), 0};   // (the mob's rotation is written in a byte: a larger component is not carried)
            m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 100, 0, 0};
            w.actor.obj.state.set(*w.actor.obj.schema, "ReplicatedMovement", repl::Value(m));
        });
        ++sc.moves;
    } else if (roll < 780 || !sc.shape_changes) {   // the version moves and nothing changes
        sc.reg.modify(sc.id, kind, [](world::WorldActor&) {});
    } else if (roll < 810) {   // a value taken away (a template's value an instance leaves unset)
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            const size_t oi = static_cast<size_t>(rng.below(static_cast<int64_t>(w.actor.subobjects.size()) + 1));
            repl::ObjectState& o = object_of(w.actor, oi);
            if (o.state.empty()) return;
            auto it = o.state.values().begin();
            std::advance(it, rng.below(static_cast<int64_t>(o.state.values().size())));
            if (it->second.get_if<repl::ArrayValue>()) return;   // (an array taken away would need its zero: the write refuses it)
            const uint16_t handle = it->first;
            repl::Value value = it->second;
            if (o.state.erase(handle)) {
                sc.erased.push_back({oi, handle, std::move(value)});
                ++sc.erases;
            }
        });
    } else if (roll < 840) {   // ... set again, to what it was (the counter-example) or to another value
        if (sc.erased.empty()) return;
        const size_t k = static_cast<size_t>(rng.below(static_cast<int64_t>(sc.erased.size())));
        const Scene::Erased e = sc.erased[k];
        sc.erased.erase(sc.erased.begin() + static_cast<std::ptrdiff_t>(k));
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            if (e.object > w.actor.subobjects.size()) return;
            repl::ObjectState& o = object_of(w.actor, e.object);
            repl::Value v = e.value;
            if (rng.chance(30)) {
                const schema::RepCommand* c = o.schema->command(e.handle);
                if (c) {
                    if (auto other = scalar_value(*c, rng.below(4))) v = std::move(*other);
                }
            }
            if (o.state.set(*o.schema, e.handle, std::move(v))) ++sc.resets;
        });
    } else if (roll < 850) {   // a subobject taken away
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            if (w.actor.subobjects.size() <= 3) return;
            const size_t i = 3 + static_cast<size_t>(rng.below(static_cast<int64_t>(w.actor.subobjects.size()) - 3));
            sc.removed.emplace_back(i, std::move(w.actor.subobjects[i]));
            w.actor.subobjects.erase(w.actor.subobjects.begin() + static_cast<std::ptrdiff_t>(i));
            ++sc.removals;
        });
    } else {   // ... and added again, at the end or where it was
        if (sc.removed.empty()) return;
        const size_t k = static_cast<size_t>(rng.below(static_cast<int64_t>(sc.removed.size())));
        auto back = std::move(sc.removed[k]);
        sc.removed.erase(sc.removed.begin() + static_cast<std::ptrdiff_t>(k));
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            const size_t at = rng.chance(50) ? w.actor.subobjects.size() : std::min(back.first, w.actor.subobjects.size());
            w.actor.subobjects.insert(w.actor.subobjects.begin() + static_cast<std::ptrdiff_t>(at), std::move(back.second));
            ++sc.readds;
        });
    }
}

struct PairResult {
    bool compared = false;
    bool same = false;      // the same bytes, the same record, and the redo gives the shadow the second connection's own update gives
    bool redo_self = false; // the redo gives the BUILDER's shadow its own update's result
};

/// The update each of the two shadows would build for the mob as it is, and the first one's redo applied to the second shadow.
inline PairResult compare_pair(const world::WorldActor& w, const repl::ActorShadow& sa, const repl::ActorShadow& sb) {
    PairResult r;
    const repl::ExportAcks acks{};
    repl::ActorShadow ca = sa, cb = sb;
    repl::ShadowUndo ua, ub;
    std::vector<repl::ContentBlock> ba, bb;
    if (!repl::collect_update_blocks(w.actor, ca, ba, &ua) || !repl::collect_update_blocks(w.actor, cb, bb, &ub)) return r;
    const auto ea = ba.empty() ? std::nullopt : repl::assemble_blocks(w.actor, ba, acks);
    const auto eb = bb.empty() ? std::nullopt : repl::assemble_blocks(w.actor, bb, acks);
    if (ba.empty() != !ea.has_value() || bb.empty() != !eb.has_value()) return r;   // (an assembly that fails: not what this compares)
    r.compared = true;
    const repl::ShadowRedo redo = repl::make_redo(ua, ca);
    repl::ActorShadow own = sa;
    redo.apply(own);
    r.redo_self = repl::shadows_equal(own, ca);
    repl::ActorShadow via = sb;
    redo.apply(via);
    const bool same_bytes = ea.has_value() == eb.has_value() && (!ea || repl::same_emission(*ea, *eb));
    const bool same_rec = repl::same_record(repl::sent_record(ua, ca), repl::sent_record(ub, cb));
    r.same = same_bytes && same_rec && repl::shadows_equal(via, cb);
    return r;
}

struct Tally {
    size_t pairs = 0;            // pairs of shadows at the same version compared
    size_t same = 0, differ = 0;
    size_t ok_pairs = 0, ok_differ = 0;            // where the epoch says both may share
    size_t private_pairs = 0, private_differ = 0;  // where it says at least one may not
    size_t updates = 0, redo_wrong = 0;            // updates the connections made, and redos that did not give the builder's shadow its own result
    size_t opens = 0;
    size_t unbuildable = 0;        // updates (a pair's or a connection's) the replicator could not build or assemble: not compared
};

inline Tally run_scene(uint64_t seed, int steps, bool shape_changes, Scene& sc) {
    sc.rng = Rng{seed};
    sc.shape_changes = shape_changes;
    Tally t;
    world::WorldActor w = make_real_npc(sc.id, 3000, 3000, sc.rng);
    sc.reg.add(std::move(w));
    constexpr int kConns = 4;
    std::vector<Conn> conns(kConns);
    const repl::ExportAcks acks{};
    const auto open = [&](Conn& c) {
        const world::WorldActor* a = sc.reg.find(sc.id);
        c.shadow = repl::ActorShadow{};
        bool failed = false;
        if (repl::open_actor(a->actor, c.shadow, acks, &failed) && !failed) {
            c.sent_version = a->version;
            c.epoch = a->shape_epoch;
            c.open = true;
            ++t.opens;
        } else {
            c.open = false;
        }
    };
    open(conns[0]);
    for (int step = 0; step < steps; ++step) {
        const int before = static_cast<int>(sc.ops);
        mutate(sc);
        (void)before;
        const world::WorldActor* a = sc.reg.find(sc.id);
        for (int i = 0; i < kConns; ++i) {
            for (int j = i + 1; j < kConns; ++j) {
                Conn& x = conns[static_cast<size_t>(i)];
                Conn& y = conns[static_cast<size_t>(j)];
                if (!x.open || !y.open || x.sent_version != y.sent_version || x.sent_version == a->version) continue;
                const PairResult r = compare_pair(*a, x.shadow, y.shadow);
                if (!r.compared) {
                    ++t.unbuildable;
                    continue;
                }
                ++t.pairs;
                (r.same ? t.same : t.differ) += 1;
                const bool ok = x.epoch == a->shape_epoch && y.epoch == a->shape_epoch;
                if (ok) {
                    ++t.ok_pairs;
                    t.ok_differ += r.same ? 0 : 1;
                } else {
                    ++t.private_pairs;
                    t.private_differ += r.same ? 0 : 1;
                }
                if (!r.redo_self) ++t.redo_wrong;
            }
        }
        // the connections: one opens late (and others now and then), the rest update when they like
        for (int i = 0; i < kConns; ++i) {
            Conn& c = conns[static_cast<size_t>(i)];
            if (!c.open ? (step >= 3 * i && sc.rng.chance(60)) : sc.rng.chance(i == 0 ? 2 : 4)) {
                open(c);
                continue;
            }
            if (!c.open || !sc.rng.chance(i == 0 ? 85 : 65)) continue;
            repl::ShadowUndo undo;
            std::vector<repl::ContentBlock> blocks;
            if (!repl::collect_update_blocks(a->actor, c.shadow, blocks, &undo)) {
                ++t.unbuildable;
                continue;
            }
            if (!blocks.empty() && !repl::assemble_blocks(a->actor, blocks, acks)) {
                undo.rollback(c.shadow);
                ++t.unbuildable;
                continue;
            }
            c.sent_version = a->version;
            ++t.updates;
        }
    }
    return t;
}

}  // namespace redo_checks

void test_shape_epoch_counts_what_leaves_a_shadow_different() {
    using namespace redo_checks;
    std::printf("the registry counts the changes of an actor that leave two connections' shadows different\n");
    world::ActorRegistry reg;
    Rng rng{99};
    reg.add(make_real_npc(5000, 0, 0, rng));
    const world::WorldActor* a = reg.find(5000);
    CHECK(a->shape_epoch == 0, "a new actor is at epoch 0");
    reg.modify(5000, world::Change::State, [](world::WorldActor& w) { w.actor.obj.state.set(*w.actor.obj.schema, "Role", repl::Value::u64(3)); });
    CHECK(a->shape_epoch == 0, "a value set does not count");
    reg.modify(5000, world::Change::State, [](world::WorldActor& w) { w.actor.obj.state.erase(w.actor.obj.schema->command("Role")->handle); });
    CHECK(a->shape_epoch == 1, "a value taken away counts");
    reg.modify(5000, world::Change::State, [](world::WorldActor& w) { w.actor.subobjects.push_back(w.actor.subobjects.back()); w.actor.subobjects.back().guid = guid(5100); });
    CHECK(a->shape_epoch == 1, "a subobject added at the end does not count");
    reg.modify(5000, world::Change::State, [](world::WorldActor& w) { w.actor.subobjects.pop_back(); });
    CHECK(a->shape_epoch == 2, "a subobject taken away counts");
    reg.modify(5000, world::Change::State, [](world::WorldActor& w) { w.actor.subobjects.insert(w.actor.subobjects.begin() + 1, w.actor.subobjects.back()); w.actor.subobjects[1].guid = guid(5102); });
    CHECK(a->shape_epoch == 3, "a subobject added before the others counts (the list is not the old one with more at the end)");
    const uint32_t v = a->version;
    reg.modify(5000, world::Change::Movement, [](world::WorldActor& w) { w.actor.obj.state.set(*w.actor.obj.schema, "Role", repl::Value::u64(2)); });
    CHECK(a->shape_epoch == 3 && a->version == v + 1, "a position alone does not count");
}
AOC_REGISTER_TEST(test_shape_epoch_counts_what_leaves_a_shadow_different);

void test_shared_update_is_the_same_for_shadows_at_one_version() {
    using namespace redo_checks;
    std::printf("two shadows at the same version, one started by an open and one by updates: the update built for one, with its redo applied to the other, is the other's own update\n");
    char text[500];
    // (1) every kind of change, the epoch decides
    Tally all;
    size_t erases = 0, resets = 0, removals = 0, readds = 0, casts = 0, sets = 0, arrays = 0, moves = 0;
    for (uint64_t seed = 1; seed <= 12; ++seed) {
        Scene sc;
        const Tally t = run_scene(seed * 7919, 400, true, sc);
        all.pairs += t.pairs;
        all.same += t.same;
        all.differ += t.differ;
        all.ok_pairs += t.ok_pairs;
        all.ok_differ += t.ok_differ;
        all.private_pairs += t.private_pairs;
        all.private_differ += t.private_differ;
        all.updates += t.updates;
        all.unbuildable += t.unbuildable;
        all.redo_wrong += t.redo_wrong;
        all.opens += t.opens;
        erases += sc.erases;
        resets += sc.resets;
        removals += sc.removals;
        readds += sc.readds;
        casts += sc.casts;
        sets += sc.sets;
        arrays += sc.arrays;
        moves += sc.moves;
    }
    std::snprintf(text, sizeof(text), "12 sequences of 400 changes (%zu values set, %zu arrays, %zu casts, %zu moves, %zu values taken away and %zu set again, %zu subobjects taken away and %zu added again) against 4 connections (%zu opens, %zu updates, %zu the replicator could not build): %zu pairs of shadows at one version compared",
                  sets, arrays, casts, moves, erases, resets, removals, readds, all.opens, all.updates, all.unbuildable, all.pairs);
    CHECK(all.pairs > 3000 && erases > 100 && resets > 50 && removals > 10 && readds > 5 && casts > 100 && sets > 1000, text);
    std::snprintf(text, sizeof(text), "...where the shape epoch lets both share (%zu pairs) not one pair differs; %zu of the %zu pairs it keeps apart do differ", all.ok_pairs, all.private_differ, all.private_pairs);
    CHECK(all.ok_pairs > 1500 && all.ok_differ == 0, text);
    std::snprintf(text, sizeof(text), "...and the property test can see what the epoch guards against: %zu pairs differ in all, %zu of them pairs the epoch keeps apart", all.differ, all.private_differ);
    CHECK(all.differ > 0 && all.private_differ == all.differ, text);
    std::snprintf(text, sizeof(text), "the redo gives the builder's own shadow exactly its own update's result (%zu of %zu pairs wrong)", all.redo_wrong, all.pairs);
    CHECK(all.redo_wrong == 0, text);
    // (2) only values set and changed: nothing ever differs, with or without the epoch
    Tally mono;
    for (uint64_t seed = 1; seed <= 12; ++seed) {
        Scene sc;
        const Tally t = run_scene(seed * 104729, 400, false, sc);
        mono.pairs += t.pairs;
        mono.differ += t.differ;
        mono.ok_pairs += t.ok_pairs;
        mono.private_pairs += t.private_pairs;
        mono.redo_wrong += t.redo_wrong;
        mono.unbuildable += t.unbuildable;
    }
    std::snprintf(text, sizeof(text), "changes that only set values, add and cast (%zu pairs, %zu not buildable; the epoch moved only where an array lost an element value: %zu of the pairs are kept apart by it): not one pair differs, even without the epoch",
                  mono.pairs, mono.unbuildable, mono.private_pairs);
    CHECK(mono.pairs > 3000 && mono.differ == 0 && mono.redo_wrong == 0, text);
}
AOC_REGISTER_TEST(test_shared_update_is_the_same_for_shadows_at_one_version);

// An array element value that was sent as a zero stays in the shadow of the connection that was sent it, and is not sent again when the value goes (rep_block.h): the shadow keeps the zero.  A
// shadow started after the value went has no entry for it, and sends the zero when the value comes back.  Two shadows at one version then build different updates, though both are clean copies
// of the mob.  The random sequences of the property test reach this case only by chance, so it is built here step by step.
void test_shared_update_an_array_that_loses_a_value_sent_as_a_zero() {
    using namespace redo_checks;
    std::printf("an array that loses an element value that was sent as a zero: the shadow that holds the zero and one started after build different updates, and the shape epoch keeps them apart\n");
    constexpr uint64_t kId = 7000;
    world::ActorRegistry reg;
    Rng rng{31};
    reg.add(make_real_npc(kId, 0, 0, rng));
    // an array of one of the mob's objects whose element has a value with a known zero
    size_t object = 0;
    const schema::RepCommand* array_cmd = nullptr;
    uint16_t local = 0;
    repl::Value zero;
    const auto make_array = [&](bool with_zero) {
        repl::ArrayValue arr;
        repl::ElementState es;
        if (with_zero) es.values[local] = zero;
        arr.elements.push_back(std::move(es));
        return repl::Value(std::move(arr));
    };
    {
        const world::WorldActor* a = reg.find(kId);
        for (size_t oi = 0; oi <= a->actor.subobjects.size() && !array_cmd; ++oi) {
            const repl::ObjectState& o = oi == 0 ? a->actor.obj : a->actor.subobjects[oi - 1].obj;
            for (const schema::RepCommand& c : o.schema->commands) {
                if (c.type != schema::PropType::DynamicArray || (c.flags & schema::kRepCustomDelta) || c.elem_end <= c.elem_begin || c.elem_end > o.schema->element_commands.size()) continue;
                const auto elems = o.schema->element_commands.subspan(c.elem_begin, c.elem_end - c.elem_begin);
                for (size_t k = 0; k < elems.size(); ++k) {
                    if (elems[k].type == schema::PropType::DynamicArray) continue;
                    const std::optional<repl::Value> z = repl::zero_value(elems[k]);
                    if (!z) continue;
                    repl::RepState probe;   // (the state takes the array only if the element value fits its command)
                    repl::ArrayValue arr;
                    repl::ElementState es;
                    es.values[static_cast<uint16_t>(k + 1)] = *z;
                    arr.elements.push_back(std::move(es));
                    if (!probe.set(*o.schema, c.handle, repl::Value(std::move(arr)))) continue;
                    object = oi;
                    array_cmd = &c;
                    local = static_cast<uint16_t>(k + 1);
                    zero = *z;
                    break;
                }
                if (array_cmd) break;
            }
        }
    }
    CHECK(array_cmd != nullptr, "the mob has an array whose element has a value with a known zero");
    if (!array_cmd) return;
    const repl::ExportAcks acks{};
    struct Side {
        repl::ActorShadow shadow;
        uint32_t          sent_version = 0;
        uint32_t          epoch = 0;
    };
    const auto set_array = [&](bool with_zero) {
        bool ok = false;
        reg.modify(kId, world::Change::State, [&](world::WorldActor& w) {
            repl::ObjectState& o = object_of(w.actor, object);
            ok = o.state.set(*o.schema, array_cmd->handle, make_array(with_zero));
        });
        return ok;
    };
    const auto open = [&](Side& s) {
        const world::WorldActor* cur = reg.find(kId);
        s.shadow = repl::ActorShadow{};
        bool failed = false;
        const bool ok = repl::open_actor(cur->actor, s.shadow, acks, &failed).has_value() && !failed;
        s.sent_version = cur->version;
        s.epoch = cur->shape_epoch;
        return ok;
    };
    const auto update = [&](Side& s, bool& sent) {   // the update the way a view makes it: nothing is sent when no handle changed
        const world::WorldActor* cur = reg.find(kId);
        repl::ShadowUndo undo;
        std::vector<repl::ContentBlock> blocks;
        if (!repl::collect_update_blocks(cur->actor, s.shadow, blocks, &undo)) return false;
        if (!blocks.empty() && !repl::assemble_blocks(cur->actor, blocks, acks)) {
            undo.rollback(s.shadow);
            return false;
        }
        s.sent_version = cur->version;
        sent = !blocks.empty();
        return true;
    };
    Side x, y;
    bool carried = false;
    CHECK(set_array(false) && open(x), "the array has one element with no value, and the shadow X is started");
    const uint32_t epoch0 = reg.find(kId)->shape_epoch;
    CHECK(set_array(true) && update(x, carried) && carried, "the element gets a value that is the zero: X is sent it");
    CHECK(reg.find(kId)->shape_epoch == epoch0, "...a value added to an element is not a change of the shape");
    CHECK(set_array(false) && update(x, carried) && !carried, "the element loses the value: X is sent nothing (the zero it holds is not sent again when the value goes)");
    CHECK(reg.find(kId)->shape_epoch == epoch0 + 1, "...and the registry counts the array that lost a value");
    CHECK(open(y), "the shadow Y is started now (its client was never sent the value)");
    CHECK(set_array(true), "the value comes back");
    const world::WorldActor* cur = reg.find(kId);
    const PairResult r = compare_pair(*cur, x.shadow, y.shadow);
    CHECK(x.sent_version == y.sent_version && x.sent_version != cur->version && r.compared, "X and Y stand at the same version, and the update of each could be built");
    CHECK(!r.same, "...X and Y build different updates: X sends nothing, Y sends the zero");
    CHECK(x.epoch != cur->shape_epoch && y.epoch == cur->shape_epoch, "...and the shape epoch keeps them apart: X was started under an epoch the mob has left, so a view with X builds its own update");
    CHECK(set_array(false) && update(y, carried) && !carried && reg.find(kId)->shape_epoch == epoch0 + 2, "an array that loses the value again counts again");
}
AOC_REGISTER_TEST(test_shared_update_an_array_that_loses_a_value_sent_as_a_zero);

void test_state_counts_an_array_that_loses_an_element_value() {
    std::printf("a state counts an array that is set again without an element value it had, nested arrays included, and nothing else\n");
    const schema::ClassSchema& sc = schema_named("UInteractableComponent");
    const auto outer_of = [](uint64_t state, uint64_t guid_word, bool with_inner, bool with_state) {   // CurrentInputStates = [{1: state, 2: [{1: guid_word}]}]
        repl::ArrayValue inner;
        repl::ElementState g;
        g.values[1] = repl::Value::i64(static_cast<int64_t>(guid_word));   // (the words of a GUID are 32-bit integers)
        inner.elements.push_back(std::move(g));
        repl::ElementState e;
        if (with_state) e.values[1] = repl::Value::u64(state);
        if (with_inner) e.values[2] = repl::Value(std::move(inner));
        repl::ArrayValue arr;
        arr.elements.push_back(std::move(e));
        return arr;
    };
    repl::RepState st;
    CHECK(st.set(sc, "CurrentInputStates", repl::Value(outer_of(0, 7, true, true))) && st.erasures() == 0, "an array set on a state that had none is not an erasure");
    CHECK(st.set(sc, "CurrentInputStates", repl::Value(outer_of(0, 7, true, true))) && st.erasures() == 0, "...nor the same array set again");
    CHECK(st.set(sc, "CurrentInputStates", repl::Value(outer_of(2, 8, true, true))) && st.erasures() == 0, "...nor values that changed, an inner one among them");
    repl::ArrayValue grown = outer_of(2, 8, true, true);
    grown.elements.push_back(repl::ElementState{});
    grown.elements[0].values[3] = repl::Value::u64(1);
    CHECK(st.set(sc, "CurrentInputStates", repl::Value(grown)) && st.erasures() == 0, "...nor an element and a value added");
    repl::ArrayValue shorter = grown;
    shorter.elements.pop_back();
    CHECK(st.set(sc, "CurrentInputStates", repl::Value(shorter)) && st.erasures() == 0, "...nor an element gone from the end (the next update of a shadow that saw the count change writes the array again)");
    repl::ArrayValue no_cached = outer_of(2, 8, true, true);   // the value 3 of the element is gone
    CHECK(st.set(sc, "CurrentInputStates", repl::Value(no_cached)) && st.erasures() == 1, "a value gone from an element counts");
    repl::ArrayValue inner_gone = outer_of(2, 8, true, true);
    repl::ArrayValue* inner = inner_gone.elements[0].values[2].get_if<repl::ArrayValue>();
    inner->elements[0].values.clear();
    CHECK(st.set(sc, "CurrentInputStates", repl::Value(inner_gone)) && st.erasures() == 2, "...a value gone from an element of a nested array counts too");
    CHECK(st.set(sc, "CurrentInputStates", repl::Value(outer_of(2, 8, false, true))) && st.erasures() == 3, "...and a nested array that is gone from its element");
    repl::RepState copy = st;
    CHECK(copy.erasures() == 3 && repl::RepState{}.erasures() == 0, "a copy of a state carries the count, a new state starts at 0");
}
AOC_REGISTER_TEST(test_state_counts_an_array_that_loses_an_element_value);
