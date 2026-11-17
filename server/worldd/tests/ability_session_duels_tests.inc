// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.
// ============================================================================
//  tests/ability_session_duels_tests.inc: the world's duels (world/duels.h) on a real registry: the request and its refusals, the banner's replicated Duel
//  through the request, the countdown and the fight, the answer's lapse, a decline's cooldown, and every end (a flight from the radius, a forfeit, a defeat,
//  the time running out, an interruption, a logout).  The duels between two real sessions are in ability_session_pvp_tests.inc.
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================
#include "world/duels.h"

namespace duels_tests {
using aoc::worldd::world::DuelBook;
using aoc::worldd::world::DuelGuid;
using aoc::worldd::world::DuelNotice;
using aoc::worldd::world::DuelResult;
const DuelGuid kDuelA{0x11111111, 0x22222222, 0x33333333, 0x44444444}, kDuelB{0x55555555, 0x66666666, 0x77777777, 0x08888888};
constexpr uint64_t kDuelPawnA = 2000, kDuelPawnB = 2002;

struct DuelRig {
    world::ActorRegistry registry;
    uint64_t next = 9000;
    DuelBook book{&registry, [this] { return aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(next += 2); }};
    DuelRig() {
        pawn(kDuelPawnA, 0, 0);
        pawn(kDuelPawnB, 5000, 0);   // 50 m apart: out of range
        book.connect(kDuelA, "Alpha", kDuelPawnA);
        book.connect(kDuelB, "Beta", kDuelPawnB);
    }
    void pawn(uint64_t id, double x, double y) {
        world::WorldActor w;
        w.kind = world::ActorKind::Player;
        w.id = id;
        w.actor.guid = aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(id);
        w.x = x;
        w.y = y;
        registry.add(std::move(w));
    }
    void move(uint64_t id, double x, double y) {
        registry.modify(id, world::Change::Movement, [&](world::WorldActor& w) {
            w.x = x;
            w.y = y;
        });
    }
    const world::WorldActor* banner() const {
        const world::WorldActor* found = nullptr;
        registry.for_each([&](const world::WorldActor& w) {
            if (w.kind == world::ActorKind::DuelBanner) found = &w;
        });
        return found;
    }
    const repl::Value* field(const char* path) const {
        const world::WorldActor* b = banner();
        return b ? b->actor.obj.state.get(*b->actor.obj.schema, path) : nullptr;
    }
    double number(const char* path) const {
        const repl::Value* v = field(path);
        const double* d = v ? v->get_if<double>() : nullptr;
        return d ? *d : -12345;
    }
    bool active() const {
        const repl::Value* v = field("Duel.bIsActive");
        const bool* b = v ? v->get_if<bool>() : nullptr;
        return b && *b;
    }
    const repl::ArrayValue* participants() const {
        const repl::Value* v = field("Duel.Participants");
        return v ? v->get_if<repl::ArrayValue>() : nullptr;
    }
    /// The last notice of a kind (a copy: the list is a temporary).
    static std::optional<DuelNotice> last(const std::vector<DuelNotice>& v, DuelNotice::Kind k) {
        std::optional<DuelNotice> out;
        for (const DuelNotice& n : v) {
            if (n.kind == k) out = n;
        }
        return out;
    }
};

int64_t duel_word_of(const repl::ElementState& e, uint16_t local) {
    const auto it = e.values.find(local);
    const int64_t* i = it != e.values.end() ? it->second.get_if<int64_t>() : nullptr;
    const uint64_t* u = it != e.values.end() ? it->second.get_if<uint64_t>() : nullptr;
    return i ? *i : u ? static_cast<int64_t>(*u) : -1;
}
}  // namespace duels_tests

static void duels_a_relog_keeps_the_new_sessions_entry() {
    using namespace duels_tests;
    std::printf("a relog: the old session's late leave does not erase the new session's entry nor end its duel\n");
    DuelRig r;
    r.move(kDuelPawnB, 1000, 0);
    CHECK(r.book.request(kDuelA, "Beta", 1.0).empty() && r.book.duels() == 1, "(Alpha asks Beta)");
    CHECK(r.book.accept(kDuelB, kDuelA, 2.0), "(Beta accepts)");
    r.book.tick(7.0);
    CHECK(r.book.fighting_key(kDuelA) != 0, "(the countdown is over: they fight)");
    // the character joins again with a new pawn, then the old session's leave arrives late
    r.book.connect(kDuelA, "Alpha", kDuelPawnA + 10);
    r.book.disconnect(kDuelA, 9.0, kDuelPawnA);
    CHECK(r.book.connected(kDuelA) && r.book.partner_pawn(kDuelA) == kDuelPawnB, "the new session's entry and the duel are still there after the old session's leave");
    CHECK(r.book.duels() == 1 && r.book.fighting_key(kDuelA) != 0, "... and the fight goes on");
    r.book.disconnect(kDuelA, 10.0, kDuelPawnA + 10);
    CHECK(!r.book.connected(kDuelA) && r.book.duels() == 0, "(his own leave is the one that ends it)");
}
AOC_REGISTER_TEST(duels_a_relog_keeps_the_new_sessions_entry);

static void duels_request_and_banner() {
    using namespace duels_tests;
    std::printf("a request: its refusals, the banner and its Duel (OnRep_DuelState shows the recipient the window)\n");
    DuelRig r;
    CHECK(DuelBook::banner_schema() != nullptr, "(the client's ADuelCenter schema is in the index)");
    CHECK(r.book.request(kDuelA, "Nobody", 0) == "You cannot duel that player.", "a name that is not online: one refusal, the same as for a busy or out-of-range target");
    CHECK(r.book.request(kDuelA, "alpha", 0) == "You cannot duel yourself.", "himself (the name in any case)");
    CHECK(r.book.request(kDuelA, "BETA", 0) == "You cannot duel that player.", "50 m apart: the same refusal (the distance does not tell who is online)");
    CHECK(r.banner() == nullptr && r.book.duels() == 0, "... and nothing is made");
    r.move(kDuelPawnB, 1000, 0);
    CHECK(r.book.request(kDuelA, "beta", 1.0).empty(), "10 m apart: the request goes");
    const world::WorldActor* b = r.banner();
    CHECK(b != nullptr && b->never_dormant && std::fabs(b->x - 500) < 1 && std::fabs(b->y) < 1, "a banner halfway between the two, never dormant");
    const repl::Value* word = r.field("Duel.InstanceGuid.A");
    const int64_t* a0 = word ? word->get_if<int64_t>() : nullptr;
    CHECK(a0 && static_cast<uint32_t>(*a0) == kDuelA[0], "Duel.InstanceGuid is the instigator's character guid");
    const repl::ArrayValue* parts = r.participants();
    CHECK(parts && parts->elements.size() == 2 && duel_word_of(parts->elements[0], 1) == 0 && duel_word_of(parts->elements[1], 1) == 1 &&
              static_cast<uint32_t>(duel_word_of(parts->elements[1], 2)) == kDuelB[0],
          "Participants: {Instigator 0, Alpha}, {Recipient 1, Beta}");
    CHECK(r.number("Duel.SyncBeginTime") == -1.0 && !r.active(), "SyncBeginTime -1, bIsActive false: a request");
    CHECK(b && b->actor.open.archetype.size() == 2 && b->actor.open.archetype[0].name == "Default__BP_DuelCenter_C" &&
              b->actor.open.archetype[1].name == "/Game/GameBlueprints/PC/BP_DuelCenter",
          "its archetype: BP_DuelCenter_C's default object in /Game/GameBlueprints/PC/BP_DuelCenter");
    const auto na = r.book.take_notices(kDuelA), nb = r.book.take_notices(kDuelB);
    CHECK(DuelRig::last(na, DuelNotice::Kind::Sent) && DuelRig::last(nb, DuelNotice::Kind::Asked) && DuelRig::last(nb, DuelNotice::Kind::Asked)->other == "Alpha",
          "the instigator is told it went, the recipient that Alpha asks");
    CHECK(r.book.request(kDuelB, "Alpha", 1.5) == "You are already in a duel.", "a second request of either: \"You are already in a duel.\"");
    CHECK(!r.book.accept(kDuelA, kDuelA, 2) && !r.book.accept(kDuelB, kDuelB, 2), "only the recipient accepts, and only that duel");
    CHECK(r.book.fighting_key(kDuelA) == 0 && r.book.partner_pawn(kDuelA) == 0, "no fight yet");
}
AOC_REGISTER_TEST(duels_request_and_banner);

static void duels_countdown_and_flight() {
    using namespace duels_tests;
    std::printf("the countdown (SyncBeginTime), the fight (bIsActive), the radius and a flight\n");
    DuelRig r;
    r.move(kDuelPawnB, 1000, 0);
    r.book.request(kDuelA, "Beta", 10.0);
    CHECK(r.book.accept(kDuelB, kDuelA, 12.0), "Beta accepts (ServerAcceptDuel with the duel's guid)");
    CHECKF(std::fabs(r.number("Duel.SyncBeginTime") - aoc::worldd::bootstrap::effect_clock(17.0)) < 1e-6 && !r.active(),
           "SyncBeginTime: the fight's start 5 s on, on the synchronized clock (%.1f); not active yet", r.number("Duel.SyncBeginTime"));
    r.book.tick(16.9);
    CHECK(r.book.fighting_key(kDuelA) == 0, "4.9 s later: still counting");
    r.book.tick(17.0);
    CHECK(r.active() && r.book.fighting_key(kDuelA) != 0 && r.book.fighting_key(kDuelA) == r.book.fighting_key(kDuelB), "5 s later: bIsActive, and both fight one duel (one key for the PvP rules)");
    CHECK(r.book.partner_pawn(kDuelA) == kDuelPawnB && r.book.partner_pawn(kDuelB) == kDuelPawnA && r.book.fighting(kDuelA) == kDuelA, "each names the other's pawn; the duel's guid is Alpha's");
    CHECK(DuelRig::last(r.book.take_notices(kDuelA), DuelNotice::Kind::Started) && DuelRig::last(r.book.take_notices(kDuelB), DuelNotice::Kind::Started) && r.book.take_notices(kDuelA).empty(),
          "both are told it began (the notices are taken once)");
    r.move(kDuelPawnA, -4000, 0);   // 45 m from the banner at (500, 0): outside its 40 m
    r.book.tick(20.0);
    const repl::ArrayValue* parts = r.participants();
    const auto ts = [&](size_t i) {
        const auto it = parts ? parts->elements[i].values.find(6) : std::map<uint16_t, repl::Value>::const_iterator{};
        const double* d = parts && it != parts->elements[i].values.end() ? it->second.get_if<double>() : nullptr;
        return d ? *d : -12345.0;
    };
    CHECKF(parts && std::fabs(ts(0) - aoc::worldd::bootstrap::effect_clock(20.0)) < 1e-6 && ts(1) == -1.0, "Alpha left the radius: his ExitRadiusTimestamp (%.1f), Beta's -1", ts(0));
    r.move(kDuelPawnA, 0, 0);
    r.book.tick(25.0);
    CHECK(r.participants() && ts(0) == -1.0, "back inside: -1 again");
    r.move(kDuelPawnA, -4000, 0);
    r.book.tick(26.0);
    r.book.tick(35.9);
    CHECK(r.book.duels() == 1, "outside again, 9.9 s: still fighting");
    r.book.tick(36.0);
    CHECK(r.book.duels() == 0 && r.banner() == nullptr, "10 s outside: the duel ends and the banner goes");
    const auto na = r.book.take_notices(kDuelA), nb = r.book.take_notices(kDuelB);
    const auto ea = DuelRig::last(na, DuelNotice::Kind::Ended);
    const auto eb = DuelRig::last(nb, DuelNotice::Kind::Ended);
    CHECK(ea && eb && ea->result == DuelResult::Flee && !ea->won && eb->won && ea->fought && ea->text == "Alpha fled the duel: Beta wins.", "Flee: Alpha lost, Beta won");
}
AOC_REGISTER_TEST(duels_countdown_and_flight);

static void duels_answers() {
    using namespace duels_tests;
    std::printf("a decline and its cooldown, a request that lapses\n");
    DuelRig r;
    r.move(kDuelPawnB, 1000, 0);
    r.book.request(kDuelA, "Beta", 0);
    CHECK(r.book.decline(kDuelB, kDuelA, 1.0) && r.book.duels() == 0 && r.banner() == nullptr, "Beta declines: no duel, no banner");
    const auto d = DuelRig::last(r.book.take_notices(kDuelA), DuelNotice::Kind::Declined);
    CHECK(d && d->text == "Beta declined your duel.", "Alpha is told");
    CHECK(r.book.request(kDuelA, "Beta", 5.0) == "Wait a moment before asking that player again.", "asked again within 10 s: refused (DuelRequestCooldown)");
    CHECK(r.book.request(kDuelA, "Beta", 11.5).empty(), "after it: it goes");
    r.book.tick(41.4);
    CHECK(r.book.duels() == 1, "29.9 s unanswered: it waits");
    r.book.tick(41.5);
    CHECK(r.book.duels() == 0 && r.banner() == nullptr, "30 s: it lapses, the banner goes");
    const auto lapsed = DuelRig::last(r.book.take_notices(kDuelA), DuelNotice::Kind::Declined);
    CHECK(lapsed && lapsed->text == "Beta did not answer your duel.", "Alpha is told nobody answered");
}
AOC_REGISTER_TEST(duels_answers);

static void duel_fight(duels_tests::DuelRig& r, double t) {
    using namespace duels_tests;
    r.move(kDuelPawnB, 1000, 0);
    r.book.request(kDuelA, "Beta", t);
    r.book.accept(kDuelB, kDuelA, t);
    r.book.tick(t + 5);
    r.book.take_notices(kDuelA);
    r.book.take_notices(kDuelB);
}

static void duels_ends() {
    using namespace duels_tests;
    std::printf("the other ends: a forfeit, a defeat, the time, an interruption, a logout; a request called off\n");
    {
        DuelRig r;
        duel_fight(r, 0);
        CHECK(r.book.forfeit(kDuelB, 6), "Beta forfeits (ServerForfeitDuel)");
        const auto e = DuelRig::last(r.book.take_notices(kDuelA), DuelNotice::Kind::Ended);
        CHECK(e && e->result == DuelResult::Forfeit && e->won && e->text == "Beta forfeited the duel to Alpha.", "Forfeit: Alpha wins");
    }
    {
        DuelRig r;
        duel_fight(r, 0);
        r.book.defeated(kDuelA, 7);
        const auto e = DuelRig::last(r.book.take_notices(kDuelB), DuelNotice::Kind::Ended);
        CHECK(e && e->result == DuelResult::WinLoss && e->won && e->text == "Beta has defeated Alpha in a duel.", "a blow that would kill Alpha: WinLoss, Beta wins");
        CHECK(r.book.duels() == 0 && r.banner() == nullptr, "... the banner goes");
    }
    {
        DuelRig r;
        duel_fight(r, 0);
        r.book.tick(1204.9);
        CHECK(r.book.duels() == 1, "1199.9 s of fighting: still on");
        r.book.tick(1205);
        const auto e = DuelRig::last(r.book.take_notices(kDuelA), DuelNotice::Kind::Ended);
        CHECK(e && e->result == DuelResult::TimeOut && !e->won, "1200 s: TimeOut, nobody wins (MaxDuelDuration, the Duel alignment's RemovalTimer)");
    }
    {
        DuelRig r;
        duel_fight(r, 0);
        r.book.interrupted(kDuelB, "another player joined the fight", 8);
        const auto e = DuelRig::last(r.book.take_notices(kDuelA), DuelNotice::Kind::Ended);
        CHECK(e && e->result == DuelResult::System && !e->won && e->text == "The duel between Alpha and Beta was interrupted (another player joined the fight).",
              "another player's harm: System, interrupted");
    }
    {
        DuelRig r;
        duel_fight(r, 0);
        r.book.disconnect(kDuelB, 9);
        const auto e = DuelRig::last(r.book.take_notices(kDuelA), DuelNotice::Kind::Ended);
        CHECK(e && e->result == DuelResult::System && r.book.duels() == 0 && !r.book.connected(kDuelB), "Beta logs out: the duel ends (System), he leaves the book");
    }
    {
        DuelRig r;
        r.move(kDuelPawnB, 1000, 0);
        r.book.request(kDuelA, "Beta", 0);
        r.book.accept(kDuelB, kDuelA, 1);
        CHECK(r.book.forfeit(kDuelA, 2), "a forfeit in the countdown");
        const auto e = DuelRig::last(r.book.take_notices(kDuelB), DuelNotice::Kind::Ended);
        CHECK(e && e->result == DuelResult::System && !e->fought && !e->won, "... calls it off: no result, no fight");
    }
    CHECK(world::duel_key(kDuelA) != 0 && world::duel_key(kDuelA) != world::duel_key(kDuelB), "(the duel's PvP key is never 0, and one per duel)");
}
AOC_REGISTER_TEST(duels_ends);
